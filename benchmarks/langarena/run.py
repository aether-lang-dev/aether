#!/usr/bin/env python3
"""Re-run #1986's float workloads with prebuilt Aether and Go binaries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time


NAMES = (
    "Matmul::Single", "Matmul::T4", "Matmul::T8", "Matmul::T16",
    "Etc::NeuralNet", "CLBG::Nbody", "CLBG::Spectralnorm",
)
RESULT = re.compile(r"^(.+): OK in ([0-9.]+)s$", re.MULTILINE)


def command(args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def revision(path):
    return {
        "commit": command(["git", "rev-parse", "HEAD"], path),
        "status": command(["git", "status", "--short"], path),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--langarena", type=Path, required=True)
    parser.add_argument("--aether", type=Path, required=True, help="Aether benchmark executable")
    parser.add_argument("--go", type=Path, required=True, help="Go benchmark executable")
    parser.add_argument("--output", type=Path, required=True, help="New output directory")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--timeout", type=int, default=300, help="Seconds per process")
    args = parser.parse_args()
    if args.repeats < 1 or args.timeout < 1:
        parser.error("repeats and timeout must be positive")
    source = args.langarena.resolve()
    binaries = {"aether": args.aether.resolve(), "go": args.go.resolve()}
    for binary in binaries.values():
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"not an executable: {binary}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    configs = {}
    for mode in ("test", "run"):
        entries = json.loads((source / f"{mode}.js").read_text())
        selected = [entry for entry in entries if entry["name"] in NAMES]
        if sorted(entry["name"] for entry in selected) != sorted(NAMES):
            raise ValueError(f"{mode}.js must contain each requested benchmark exactly once")
        configs[mode] = output / f"{mode}.json"
        configs[mode].write_text(json.dumps(selected, indent=2) + "\n")
    cpu = platform.processor()
    if Path("/proc/cpuinfo").exists():
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    metadata = {
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": platform.platform(), "cpu": cpu, "cpu_count": os.cpu_count(),
        "langarena": revision(source),
        "aether_source": revision(Path(__file__).resolve().parents[2]),
        "binaries": {lang: {"path": str(path),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            for lang, path in binaries.items()},
        "repeats": args.repeats,
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    def run(lang, mode, repeat):
        print(f"{mode} {repeat}: {lang}", flush=True)
        result = subprocess.run([str(binaries[lang]), str(configs[mode])],
            cwd=output, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=args.timeout)
        (output / f"{mode}-{repeat}-{lang}.log").write_text(result.stdout)
        rows = RESULT.findall(result.stdout)
        if (result.returncode or "ERR[" in result.stdout or
                sorted(name for name, _ in rows) != sorted(NAMES)):
            raise RuntimeError(f"{lang} {mode} failed correctness checks; see {output}")
        return {name: float(seconds) for name, seconds in rows}

    # Correctness uses the suite's unchanged small inputs before timing its
    # unchanged production inputs. Every timed run also checks its checksum.
    for lang in binaries:
        run(lang, "test", 0)
    samples = {name: {lang: [] for lang in binaries} for name in NAMES}
    for repeat in range(args.repeats):
        order = ("aether", "go") if repeat % 2 == 0 else ("go", "aether")
        for lang in order:
            for name, seconds in run(lang, "run", repeat).items():
                if seconds <= 0:
                    raise ValueError(f"{name}: timing resolution too low")
                samples[name][lang].append(seconds)
                (output / "samples.json").write_text(json.dumps(samples, indent=2) + "\n")
    lines = ["| Benchmark | Aether median (s) | Go median (s) | Aether / Go |",
             "|---|---:|---:|---:|"]
    for name in NAMES:
        ae = statistics.median(samples[name]["aether"])
        go = statistics.median(samples[name]["go"])
        lines.append(f"| {name} | {ae:.3f} | {go:.3f} | {ae / go:.2f} |")
    summary = "\n".join(lines) + "\n"
    (output / "summary.md").write_text(summary)
    print(summary)


if __name__ == "__main__":
    main()
