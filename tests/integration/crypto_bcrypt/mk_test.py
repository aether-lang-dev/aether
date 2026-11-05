rows = [l.split() for l in open("expected.txt")]
vec, notrep = [], []
for ver, cost, salt, hexpw, exp, hc, hg in rows:
    vec.append(f'"{ver} {cost} {salt} {hexpw} {exp}"')
    for other, who in ((hc, "libxcrypt"), (hg, "freebsd")):
        if other != exp and other != "undef":
            notrep.append(f'"{who} {hexpw} {other}"')
V = ",\n        ".join(vec); N = ",\n        ".join(notrep)
open("test_crypto_bcrypt.ae", "w").write(f'''// std.cryptography.bcrypt against two independent bcrypt implementations.
//
// The vectors below are generated, not hand-copied: gen_inputs.py (here) makes
// a deterministic set of (version, cost, salt, password) rows, ref.pl hashes
// each with the C library's crypt() on two systems, and mk_test.py writes this
// file. The references are libxcrypt 4.4 on CachyOS (crypt_blowfish, the
// bcrypt of most Linux distributions and PHP) and FreeBSD 15.1's libc (from
// OpenBSD's bcrypt). They agree on 185 of the 195 rows; the other ten are the
// cases module.ae's header describes, and the expected value there is the one
// this module means:
//   - $2a$ of a password of 255+ bytes: FreeBSD keeps OpenBSD's original $2a$
//     length wrap; expected = libxcrypt's (72-byte truncation, as $2b$).
//   - $2a$ of \\xff\\xff\\xa3: crypt_blowfish's $2x$ countermeasure; expected =
//     FreeBSD's (the plain algorithm, as $2b$).
//   - $2$: libxcrypt has no $2$; expected = FreeBSD's.
// The rows cover every version, costs 4, 5, 8 and 10, passwords of 0 to 300
// bytes (every boundary around 72), bytes 0x01..0xff, UTF-8, and the Openwall
// crypt_blowfish vectors with their published salts. The other references'
// values for the ten rows are asserted NOT to verify, so the divergence stays
// deliberate. Regenerate (needs ssh to both hosts): see gen_inputs.py.
import std.cryptography.bcrypt
import std.bytes
import std.string

hv(c: int) -> int {{
    if c >= 48 && c <= 57 {{ return c - 48 }}
    if c >= 97 && c <= 102 {{ return c - 87 }}
    return 0
}}

// "-" is the empty password.
pw_from_hex(hexpw: string) -> ptr {{
    n = 0
    if hexpw != "-" {{ n = string.length(hexpw) / 2 }}
    b = bytes.new(n + 1)
    bytes.set_length(b, n)
    i = 0
    while i < n {{
        bytes.set(b, i, hv(string_char_at(hexpw, i * 2)) * 16 + hv(string_char_at(hexpw, i * 2 + 1)))
        i = i + 1
    }}
    return b
}}

field(s: string, idx: int) -> string {{
    rest = s
    k = 0
    while k < idx {{
        p = string.index_of(rest, " ")
        rest = string.substring(rest, p + 1, string.length(rest))
        k = k + 1
    }}
    p = string.index_of(rest, " ")
    if p < 0 {{ return rest }}
    return string.substring(rest, 0, p)
}}

main() {{
    // version cost salt hex(password)|- expected
    string[] vectors = [
        {V}
    ]
    // reference hex(password) its-hash: what another implementation gives that
    // this one deliberately does not (see the header).
    string[] not_reproduced = [
        {N}
    ]
    fails = 0
    i = 0
    while i < vectors.len {{
        r = vectors[i]
        ver = field(r, 0)
        cost = (string_char_at(field(r, 1), 0) - 48) * 10 + (string_char_at(field(r, 1), 1) - 48)
        want = field(r, 4)
        pw = pw_from_hex(field(r, 3))
        n = bytes.length(pw)
        salt = bcrypt.base64_decode_salt(field(r, 2))
        got = bcrypt.hash_pw_bytes(bytes.view(pw)[0..n], bytes.view(salt)[0..16], cost, ver)
        if got != want {{
            println("FAIL hash row ${{i}} (${{ver}}, ${{n}} bytes): got ${{got}} want ${{want}}")
            fails = fails + 1
        }}
        if bcrypt.check_pw_bytes(bytes.view(pw)[0..n], want) != 1 {{
            println("FAIL check row ${{i}} (${{ver}}, ${{n}} bytes)")
            fails = fails + 1
        }}
        bytes.free(pw)
        bytes.free(salt)
        i = i + 1
    }}
    println("ok   ${{vectors.len}} cross-implementation vectors hash and verify")

    j = 0
    while j < not_reproduced.len {{
        r = not_reproduced[j]
        pw = pw_from_hex(field(r, 1))
        n = bytes.length(pw)
        if bcrypt.check_pw_bytes(bytes.view(pw)[0..n], field(r, 2)) != 0 {{
            println("FAIL ${{field(r, 0)}} value verified, expected the documented divergence: ${{field(r, 2)}}")
            fails = fails + 1
        }}
        bytes.free(pw)
        j = j + 1
    }}
    println("ok   ${{not_reproduced.len}} divergent reference values do not verify")

    // Bouncy Castle OpenBSDBCryptTest / BCrypt.Net and CryptSharp vectors.
    s1 = bcrypt.base64_decode_salt("DCq7YPn5Rq63x1Lad4cll.")
    if bcrypt.hash_pw("", bytes.view(s1)[0..16], 6, "2a") != "$2a$06$DCq7YPn5Rq63x1Lad4cll.TV4S6ytwfsfvkgY8jIucDrjc8deX1s." {{ println("FAIL bc empty"); fails = fails + 1 }}
    s2 = bcrypt.base64_decode_salt("m0CrhHm10qJ3lXRY.5zDGO")
    if bcrypt.hash_pw("a", bytes.view(s2)[0..16], 6, "2a") != "$2a$06$m0CrhHm10qJ3lXRY.5zDGO3rS2KdeeWLuGmsfGlMfOxih58VYVfxe" {{ println("FAIL bc a"); fails = fails + 1 }}
    s3 = bcrypt.base64_decode_salt("If6bvum7DFjUnE9p2uDeDu")
    if bcrypt.hash_pw("abc", bytes.view(s3)[0..16], 6, "2a") != "$2a$06$If6bvum7DFjUnE9p2uDeDu0YHzrHM6tf.iqN8.yx.jNN1ILEf7h0i" {{ println("FAIL bc abc"); fails = fails + 1 }}
    h_cs = "$2a$05$24xz81ZZsMUMm940bbWMCeHsO.s6A3MG0JZzm4y3.Ti6P96bz6RN6"
    if bcrypt.check_pw(">l_7}}xxH3|Cr{{dCR[HTUN@k~", h_cs) != 1 {{ println("FAIL cryptsharp match"); fails = fails + 1 }}
    if bcrypt.check_pw("wrong_password", h_cs) != 0 {{ println("FAIL cryptsharp mismatch"); fails = fails + 1 }}
    h12 = "$2y$12$DB3BUbYa/SsEL7kCOVji0OauTkPkB5Y1OeyfxJHM7jvMrbml5sgD2"
    if bcrypt.check_pw("a", h12) != 1 {{ println("FAIL 2y cost 12"); fails = fails + 1 }}
    if bcrypt.check_pw("b", h12) != 0 {{ println("FAIL 2y cost 12 wrong"); fails = fails + 1 }}
    if bcrypt.check_pw("a", "$2b$12$DB3BUbYa/SsEL7kCOVji0OauTkPkB5Y1OeyfxJHM7jvMrbml5sgD2") != 1 {{ println("FAIL 2b cost 12"); fails = fails + 1 }}
    bytes.free(s1)
    bytes.free(s2)
    bytes.free(s3)
    println("ok   Bouncy Castle and CryptSharp vectors")

    if fails == 0 {{
        println("ALL BCRYPT INTEGRATION TESTS PASSED")
        return
    }}
    println("${{fails}} FAILURES")
    exit(1)
}}
''')
print(len(vec), "vectors,", len(notrep), "not-reproduced")
