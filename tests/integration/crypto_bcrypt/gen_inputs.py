# Regenerating test_crypto_bcrypt.ae's vectors (not run by the test suite):
#   python3 gen_inputs.py                       # inputs.txt: version cost salt hex(password)
#   for h in <linux-host> <freebsd-host>; do    # libxcrypt, FreeBSD libc
#     scp ref.pl inputs.txt $h:/tmp/ && ssh $h 'perl /tmp/ref.pl < /tmp/inputs.txt' > ref_$h.txt
#   done                                        # (2026-11-05: CachyOS libxcrypt 4.4, FreeBSD 15.1)
#   mv ref_<linux-host>.txt ref_cachyos.txt; mv ref_<freebsd-host>.txt ref_ghostbsd.txt
#   python3 mk_expected.py && python3 mk_test.py   # writes test_crypto_bcrypt.ae
# mk_expected.py stops on any disagreement between the two references it
# cannot explain by the documented cases (module.ae's header).
import random
r = random.Random(20261105)
B64 = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
def salt():
    s = "".join(r.choice(B64) for _ in range(21))
    return s + r.choice(".Oeu")          # canonical last char (low 4 bits zero)
named = [b"", b"a", b"abc", b"U*U", b"U*U*", b"U*U*U", b"password", b"Password",
         b"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789chars after 72 are ignored",
         b"\xa3", b"\xff\xff\xa3", b"1\xa3345", b"\xff\xa3345", b"\xa3ab", b"\xd1\x91", b"\xd0\xc1\xd2\xcf\xcc\xd8",
         b"\xaa" * 72 + b"chars after 72 are ignored as usual", b"\xaa\x55" * 36, b"\x55\xaa\xff" * 24,
         "pässwörd".encode(), "密码".encode(), "🔐key".encode()]
pws = list(named)
for n in [1, 2, 3, 4, 5, 8, 15, 16, 17, 31, 32, 33, 55, 56, 63, 64, 70, 71, 72, 73, 74, 100, 128, 254, 255, 256, 300]:
    pws.append(bytes(r.choice(b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#%^&*()-_=+ ") for _ in range(n)))
for n in [1, 2, 7, 23, 71, 72, 73, 90]:
    pws.append(bytes(r.randint(1, 255) for _ in range(n)))     # any byte but NUL
rows = []
for i, pw in enumerate(pws):
    for ver in ["2a", "2b", "2y"]:
        cost = 4 if i % 3 else 5
        rows.append((ver, cost, salt(), pw))
for pw in [b"", b"U*U", b"a", b"abc", b"\xa3", b"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789chars after 72 are ignored"]:
    rows.append(("2", 4, salt(), pw))
rows.append(("2b", 8, salt(), b"higher-cost"))
# The classic Openwall crypt_blowfish vectors, with their published salts.
OK = "/OK.fbVrR/bpIqNJ5ianF."
for ver, cost, s_, pw in [
    ("2a", 5, "CCCCCCCCCCCCCCCCCCCCC.", b"U*U"), ("2a", 5, "CCCCCCCCCCCCCCCCCCCCC.", b"U*U*"),
    ("2a", 5, "XXXXXXXXXXXXXXXXXXXXXO", b"U*U*U"), ("2a", 5, "CCCCCCCCCCCCCCCCCCCCC.", b""),
    ("2a", 5, "abcdefghijklmnopqrstuu", b"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789chars after 72 are ignored"),
    ("2y", 5, OK, b"\xff\xff\xa3"), ("2b", 5, OK, b"\xff\xff\xa3"), ("2y", 5, OK, b"\xa3"),
    ("2y", 5, OK, b"\xff\xa3" b"34" b"\xff\xff\xff\xa3" b"345"), ("2y", 5, OK, b"\xff\xa3" b"345"),
    ("2y", 5, OK, b"\xa3" b"ab"), ("2y", 5, "6bNw2HLQYeqHYyBfLMsv/O", b"\xd1\x91"),
    ("2y", 5, "6bNw2HLQYeqHYyBfLMsv/O", b"\xd0\xc1\xd2\xcf\xcc\xd8"),
    ("2y", 5, OK, b"\xaa" * 72 + b"chars after 72 are ignored as usual"),
    ("2y", 5, OK, b"\xaa\x55" * 36), ("2y", 5, OK, b"\x55\xaa\xff" * 24)]:
    rows.append((ver, cost, s_, pw))
rows.append(("2y", 10, salt(), b"cost-ten"))
with open("inputs.txt", "w") as f:
    for ver, cost, s, pw in rows:
        f.write(f"{ver} {cost:02d} {s} {pw.hex()}\n")
print(len(rows))
