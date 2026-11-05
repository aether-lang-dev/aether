a = [l.split() for l in open("ref_cachyos.txt")]
b = [l.split() for l in open("ref_ghostbsd.txt")]
out = open("expected.txt", "w"); why = {}
for x, y in zip(a, b):
    ver, cost, salt = x[0], x[1], x[2]
    hexpw = x[3] if len(x) == 5 else ""
    hc, hg = x[-1], y[-1]
    pw = bytes.fromhex(hexpw)
    if hc == hg: exp, src = hg, "both"
    elif ver == "2": exp, src = hg, "freebsd-only"
    elif ver == "2a" and len(pw) >= 255: exp, src = hc, "2a-long: libxcrypt (FreeBSD keeps the 2a length wrap)"
    elif ver == "2a": exp, src = hg, "2a-8bit: FreeBSD (libxcrypt countermeasure)"
    else: raise SystemExit(f"unexplained disagreement {x} {y}")
    why[src] = why.get(src, 0) + 1
    out.write(f"{ver} {cost} {salt} {hexpw or '-'} {exp} {hc} {hg}\n")
print(why)
