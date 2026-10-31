#!/usr/bin/env python3
# Writes the std.cbor fuzz seed corpus (tests/fuzz/cbor/corpus/) from the hex
# below, so a reviewer can read what each seed holds. Run it after editing
# this list:  python3 tests/fuzz/cbor/gen_corpus.py
import os, shutil

SEEDS = {
    # The cases the harness was asked to start from.
    "empty": "",
    "truncated_head": "18",
    "bstr_with_nul": "4100",
    "nested_255": "81" * 255 + "01",
    "nested_257": "81" * 257 + "01",
    "map_dup_int_key": "a2010a0114",
    "indef_array": "9f0102ff",

    # std/cbor/test_cbor_hardening.ae: duplicate keys, length bombs, limits.
    "dup_text_key": "a2656772616e74646e6f6e65656772616e7463616c6c",
    "dup_key_indef_text": "a2616101 7f6161ff 02",
    "nested_dup": "a101a2656772616e74646e6f6e65656772616e7463616c6c",
    "distinct_keys_mixed": "a20163696e74613163737472",
    "bomb_bstr32": "5affffffff",
    "bomb_bstr32_half": "5a80000000",
    "bomb_bstr64": "5bffffffffffffffff",
    "bomb_arr32": "9affffffff",
    "bomb_arr32_half": "9a80000000",
    "bomb_arr64": "9bffffffffffffffff",
    "bomb_map32": "baffffffff",
    "bomb_map32_half": "ba80000000",
    "bomb_map64": "bbffffffffffffffff",
    "nest4": "8181818101",
    "nest5": "818181818101",
    "arr3": "83010203",
    "arr4": "8401020304",
    "indef_arr3": "9f010203ff",
    "indef_arr4": "9f01020304ff",
    "map2": "a2010a0214",
    "map3": "a3010a0214031e",
    "indef_map2": "bf010a0214ff",
    "indef_map3": "bf010a0214031eff",
    "unsorted_map": "a2616202616101",
    "nonpreferred_int": "1900ff",
    "float64_shrinks": "fb3ff8000000000000",
    "float32_stays": "fa47c35000",

    # Sign bit set at each float width, and a 64-bit argument with its top bit:
    # each once shifted a set bit into a signed long's bit 63 (undefined in C).
    "float32_neg": "fac0000000",
    "float32_neg_subnormal": "fa80000001",
    "float16_neg_subnormal": "f98001",
    "tag_u64_top_bit": "db800000000000000000",

    # RFC 8949 Appendix A.
    "a_0": "00", "a_1": "01", "a_10": "0a", "a_23": "17", "a_24": "1818",
    "a_25": "1819", "a_100": "1864", "a_1000": "1903e8", "a_1000000": "1a000f4240",
    "a_1e12": "1b000000e8d4a51000", "a_u64_max": "1bffffffffffffffff",
    "a_bignum_pos": "c249010000000000000000", "a_neg_u64_max": "3bffffffffffffffff",
    "a_bignum_neg": "c349010000000000000000", "a_neg1": "20", "a_neg10": "29",
    "a_neg100": "3863", "a_neg1000": "3903e7",
    "a_f16_0": "f90000", "a_f16_neg0": "f98000", "a_f16_1": "f93c00",
    "a_f64_1_1": "fb3ff199999999999a", "a_f16_1_5": "f93e00", "a_f16_65504": "f97bff",
    "a_f32_100000": "fa47c35000", "a_f32_max": "fa7f7fffff", "a_f64_1e300": "fb7e37e43c8800759c",
    "a_f16_min_sub": "f90001", "a_f16_min_norm": "f90400", "a_f16_neg4": "f9c400",
    "a_f64_neg4_1": "fbc010666666666666",
    "a_f16_inf": "f97c00", "a_f16_nan": "f97e00", "a_f16_neginf": "f9fc00",
    "a_f32_inf": "fa7f800000", "a_f32_nan": "fa7fc00000", "a_f64_inf": "fb7ff0000000000000",
    "a_false": "f4", "a_true": "f5", "a_null": "f6", "a_undefined": "f7",
    "a_simple16": "f0", "a_simple255": "f8ff",
    "a_tag0_datetime": "c074323031332d30332d32315432303a30343a30305a",
    "a_tag1_epoch": "c11a514b67b0", "a_tag1_float": "c1fb41d452d9ec200000",
    "a_tag23": "d74401020304", "a_tag24": "d818456449455446",
    "a_tag32_uri": "d82076687474703a2f2f7777772e6578616d706c652e636f6d",
    "a_bytes_empty": "40", "a_bytes4": "4401020304",
    "a_text_empty": "60", "a_text_a": "6161", "a_text_ietf": "6449455446",
    "a_text_quote_bslash": "62225c", "a_text_u00fc": "62c3bc", "a_text_u6c34": "63e6b0b4",
    "a_text_u10151": "64f0908591",
    "a_arr_empty": "80", "a_arr123": "83010203", "a_arr_nested": "8301820203820405",
    "a_arr25": "98190102030405060708090a0b0c0d0e0f101112131415161718181819",
    "a_map_empty": "a0", "a_map_int": "a201020304", "a_map_text": "a26161016162820203",
    "a_arr_map": "826161a161626163",
    "a_map5": "a56161614161626142616361436164614461656145",
    "a_indef_bytes": "5f42010243030405ff", "a_indef_text": "7f657374726561646d696e67ff",
    "a_indef_arr_empty": "9fff", "a_indef_nested1": "9f018202039f0405ffff",
    "a_indef_nested2": "9f01820203820405ff", "a_indef_nested3": "83018202039f0405ff",
    "a_indef_nested4": "83019f0203ff820405",
    "a_indef_arr25": "9f0102030405060708090a0b0c0d0e0f101112131415161718181819ff",
    "a_indef_map": "bf61610161629f0203ffff", "a_arr_indef_map": "826161bf61626163ff",
    "a_indef_map2": "bf6346756ef563416d7421ff",

    # Malformed (RFC 8949 Appendix F).
    "bad_chunk_type": "5f4101 6101ff",
    "bad_nested_indef_chunk": "5f5f4101ffff",
    "bad_simple_lt32": "f814",
    "bad_lone_break": "ff",
    "bad_reserved_ai": "1c",
    "bad_indef_int": "1f",
    "trailing_bytes": "0102",
    "bad_utf8": "62c328",
    "truncated_map": "a201",
    "break_in_definite": "8301ff02",
}

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "corpus")
shutil.rmtree(out, ignore_errors=True)
os.makedirs(out)
for name, hx in SEEDS.items():
    with open(os.path.join(out, name), "wb") as f:
        f.write(bytes.fromhex(hx.replace(" ", "")))
print(f"{len(SEEDS)} seeds in {out}")
