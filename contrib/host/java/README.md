# contrib.host.java — Java Sandbox Agent (Panama FFI)

## Prerequisites

```bash
# Debian/Ubuntu — OpenJDK 22+ required for Panama FFI (stable)
# Amazon Corretto 24:
wget https://corretto.aws/downloads/latest/amazon-corretto-24-x64-linux-jdk.deb
sudo dpkg -i amazon-corretto-24-x64-linux-jdk.deb

# Verify
java -version    # must be 22+
javac -version
```

## Build the agent jar

```bash
./contrib/host/java/build.sh
# Creates: build/aether-sandbox.jar
```

## Usage

Java runs as a separate process (JVM), not embedded. Use
`spawn_sandboxed` or run directly with the agent:

```bash
java --enable-native-access=ALL-UNNAMED \
     -javaagent:build/aether-sandbox.jar \
     -cp build/aether-sandbox.jar:your-app.jar \
     com.example.Main
```

### `grant_jvm_runtime()` helper

JVM startup needs ~30 grants for the linker, trust stores, locale, and
`JAVA_*` env vars before any application code runs. Bundling them once
keeps spawn scripts readable:

```aether
import contrib.host.java
import std.sandbox

main() {
    worker = sandbox.new("my-java-app") {
        java.grant_jvm_runtime()         // JVM bring-up (29 grants)
        grant_fs_read("/app/data/*")      // application-specific
        grant_tcp("api.example.com", 443)
    }
    spawn_sandboxed(worker, "java",
        "--enable-native-access=ALL-UNNAMED",
        "-javaagent:build/aether-sandbox.jar",
        "-jar", "my-app.jar")
    sandbox.free(worker)
}
```

The grant set is conservative — it permits reads the JVM performs
during class loading and TLS init, and nothing more. Source paths were
captured empirically via `strace java -version` on Corretto 24 (Debian)
and Temurin 21 (Ubuntu).

## Testing

Three tests exercise the Java host. All **SKIP** (never fail) on Windows or
when `javac`/`java` aren't on `PATH`; the two FFI tests additionally SKIP
unless the JDK is 22+, the version that stabilized Panama
(`java.lang.foreign.*`).

The end-to-end test is
[`tests/integration/embedded_java_trading_e2e/`](../../../tests/integration/embedded_java_trading_e2e/)
— [`test_embedded_java_trading_e2e.sh`](../../../tests/integration/embedded_java_trading_e2e/test_embedded_java_trading_e2e.sh)
copies `examples/embedded-java/trading/` to a temp dir, runs its `build.sh`
(namespace build + `javac` + `java`), and asserts the full trading set-piece:
each event fires with the right id (`OrderPlaced 100`, `OrderRejected 101`,
`UnknownTicker 102`, `TradeKilled 100`), the book ends at `{100=KILLED}`, and
both the Java host's `[event]` lines and the Aether library's `[ae]` lines
reach stdout in order.

The namespace/FFI test is
[`tests/integration/namespace_java/`](../../../tests/integration/namespace_java/)
— [`test_namespace_java.sh`](../../../tests/integration/namespace_java/test_namespace_java.sh)
runs `ae build --namespace .` on [`calc.ae`](../../../tests/integration/namespace_java/calc.ae)
to emit both the `.so` and a generated Java SDK
(`com/example/calc/CalcGeneratedSdk.java`), then compiles and runs
[`Check.java`](../../../tests/integration/namespace_java/Check.java) against it
to confirm the generated SDK round-trips a call and the `[ae]` script-side
output is visible to the host.

The shared-map round-trip is
[`tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh)
— its Java case gates on `build/aether-sandbox.jar` (built by
[`build.sh`](build.sh) above) plus a JDK, then spawns `java` with the
`-javaagent` jar from a C parent that froze a shared map into shm, asserting
the agent reads and writes it back (`result=processed Frank`, `status=ok`) and
that the frozen input is untampered.

Note the Java host is a JVM-side jar, not a C bridge archive, so it is **not**
part of `make contrib` ([`tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh),
which archives the dlopen/Panama C hosts); build its jar with
[`build.sh`](build.sh) instead.

## Notes

- Requires `--enable-native-access=ALL-UNNAMED` for Panama FFI
- The agent reads grants from shared memory (`AETHER_SANDBOX_SHM`)
  or a grant file (`AETHER_SANDBOX_GRANTS`)
- LD_PRELOAD enforces file/network/exec at the libc level
- The agent provides `AetherSandboxHooks.checkEnv()` for env vars
  (Java's `System.getenv()` is cached and immutable)
- Shared map via `AetherMap.fromSharedMemory()` using Panama FFI
