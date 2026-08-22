# Task for Codex: fix the memory leaks in `std/message` (Phase 3 i18n), then prove clean with valgrind

## TL;DR

`std/message/module.ae` (ICU MessageFormat parser + formatter + catalog) is **functionally complete and correct** — the whole test suite passes. It was authored by Google Jules. The **only** thing blocking merge is that it leaks memory, and Aether's CI has a hard valgrind + macOS `leaks` gate that rejects **any** leak.

Your job: make `tests/regression/test_message.ae` run **leak-clean** under valgrind (`definitely lost: 0 bytes`, `indirectly lost: 0 bytes`) **without breaking any test assertion** and **without changing the public API or the `.ae fmt` formatting**.

Do **not** commit or open a PR. Leave the fixed files in-situ in the working copy. When done, paste the final valgrind summary and the `All PASS` line into your report.

Both files are already in the working copy on branch `feat/i18n-message-phase3` (untracked):
- `std/message/module.ae`  (the module — this is what you edit)
- `tests/regression/test_message.ae`  (the test — do **not** weaken it; you may add frees to the harness only if a leak genuinely originates in the test)

---

## Ground truth — the current leak (measured, authoritative)

Build + measure (this is also your proof recipe — see bottom):

```
rm -rf ~/.aether/cache && rm -f build/test_*
./build/ae build tests/regression/test_message.ae -o /tmp/msg
valgrind --leak-check=full --num-callers=8 /tmp/msg
./build/ae run tests/regression/test_message.ae      # must print "All PASS"
```

Current baseline (Jules's original, unmodified):
```
definitely lost: 677 bytes in 57 blocks
indirectly lost: 60 bytes in 20 blocks
ERROR SUMMARY: 57 errors from 57 contexts
```
Test result: **`All PASS`** (functionally correct — do not regress this).

There are three distinct leak classes, confirmed by valgrind stacks:

1. **~37 × "1 byte" leaks** — stack: `aether_caps_malloc → message_parse_pattern_err → message_parse → main`.
   These are the **`err` empty-string `""`** values that the parser returns on the *success* path (e.g. `parse_pattern_err` line ~202 `return nodes, ""`; `parse_block` success returns `n, ""`). Each `""` is a freshly heap-allocated 1-byte AetherString that the caller never frees (or frees with the wrong primitive — see the trap below).

2. **~20 × "35 (32 direct, 3 indirect) byte" leaks** — stack: `string_concat → message_format_pattern → main`.
   These are **owned strings built during formatting** (`string_concat`, `string_substring`, `string_trim`, `to_int` results) that are appended to the strbuilder or used transiently and never freed. NOTE: valgrind attributes these to `message_format_pattern` but the code is largely in `format_nodes` (inlined) — look in `format_nodes` (lines ~418–544), not just `format_pattern`.

3. **20 × "3 byte" indirectly-lost** — child allocations of the above (freed automatically once you fix their owners).

---

## CRITICAL — read this before you touch anything (it is why naïve fixes REGRESS)

I tried three "obvious" one-liner fixes and **every one made the leak total WORSE** (677 → up to 1,976 bytes). Here is exactly why, so you don't repeat them:

### Trap A: `string_free` IS `string_release` — they are identical
In `std/string/aether_string.c`:
```c
void string_free(const void* str) { string_release(str); }   // literal alias
```
`string_release` decrements a refcount and frees at zero, and is a **no-op on non-magic pointers** (`if (!str || !is_aether_string(str)) return;`). So:
- Swapping `string_release(x)` → `string_free(x)` changes **nothing**. Don't bother.
- Calling either on something that isn't a magic AetherString (see Trap B) is a silent no-op → the thing still leaks.

### Trap B: `strbuilder.finish` returns a PLAIN libc `char*`, NOT a magic AetherString
In `std/strbuilder/aether_strbuilder.c::aether_strbuilder_finish` — it hands off the raw data buffer as a plain `char*` and frees only the wrapper. Consequences:
- `format_nodes` returns `strbuilder.finish(sb)` → the returned string is **not** refcounted. `string_release`/`string_free` on it are **no-ops** (fail the `is_aether_string` check).
- The heap-tracker's reassignment wrapper frees these with a plain libc `free()` when the slot is overwritten/goes out of scope. So a `finish()` result is freed by **letting the variable's slot be reassigned or returned**, or by an explicit plain-`free` extern — NOT by `string_release`.
- This is why "free `branch_str` after append" regressed: `branch_str` is a `finish()` result; `string_free(branch_str)` no-op'd AND the extra churn shifted accounting.

### Trap C: reassigning a var to `""` does NOT reliably free the old owned value
Jules used `arg_name = ""` / `type_name = ""` (e.g. lines ~254, ~260-261, ~270-274) trying to release owned substrings. The heap-tracker's assignment wrapper frees the *previous* heap value on reassignment **only for slots it tracks as heap** — and only if the value is actually owned by that slot at that point. Where the value was already moved into a `Node.text` field, or where the slot's heap-ness wasn't inferred, this leaks or (worse) double-accounts. Don't rely on `x = ""` as a free; use the correct explicit free for the actual representation.

### Trap D: cache staleness will lie to you
You **must** `rm -rf ~/.aether/cache && rm -f build/test_*` before every measurement, or you'll measure a stale binary and chase ghosts. (This bit me — a "regression" was partly stale build.) Always clean-build before trusting a valgrind number.

---

## The ownership model you must apply (the actual rules)

| Value source | Representation | How to free |
|---|---|---|
| `string_concat(a,b)` | magic AetherString (refcount=1) | `string_release(x)` (or `string_free`, same thing) |
| `string_substring(...)` | magic AetherString | `string_release(x)` |
| `string_trim(...)` | magic AetherString | `string_release(x)` |
| `string_copy(x)` | magic AetherString | `string_release(x)` |
| a bare `""` literal returned/assigned | freshly-minted 1-byte magic AetherString | `string_release(x)` — **but** don't release something you're about to `return` as the tuple's owned value; the caller owns it |
| `strbuilder.finish(sb)` | **plain libc char\*** (NOT magic) | let the slot reassign/return (plain libc free via tracker); `string_release` is a NO-OP here |
| `map_get_raw(m,k)` | borrowed pointer (map still owns it) | **do not free** |
| `n.text`, `b.selector` (struct fields) | owned by the Node/Branch | freed by `node_free`/`pattern_free`; **don't free the borrowed reference** |
| `plural.plural_category(locale,n)` | **verify** — see task 2 | see task 2 |

The core discipline: **every `string_concat`/`string_substring`/`string_trim`/`string_copy` result must be released on every path once you're done with it** — UNLESS ownership is transferred (stored into a struct field that a `*_free` will reclaim, or returned as an owned tuple element). Appending to a strbuilder with `strbuilder.append(sb, x)` **copies** `x` (it does not take ownership), so after appending an owned temporary you must still release it.

---

## Step-by-step

### Task 1 — the `err = ""` success-path leaks (leak class 1, ~37 blocks)
Every parser fn that returns `(ptr, string)` returns `""` for "no error" on success. That `""` is owned by the **caller**. Audit each caller of `parse`, `parse_block`, `parse_pattern_err`:
- `format` (line ~565), `catalog_add` (line ~584), `format_pattern` (line ~552 via `language.parse`), and `parse` itself (line ~394).
- On the success branch (`err != ""` is false), the code already does `string_release(err)` in several places — **verify each one actually runs on the success path**, not only the error path. In `format`/`catalog_add` the pattern is:
  ```
  pat, err = parse(msg)
  if err != "" { string_release(err); return ... }
  string_release(err)          // <-- this must exist on the fall-through success path
  ```
  Make sure the fall-through `string_release(err)` is present in **every** caller. Same for `err_sel` from `to_int` (line ~451) and any other `_, err = ...` tuple where the err half is an owned `""`.
- Also: internal parser fns that produce an intermediate `err`/`b_err` and then discard it (e.g. `parse_block`'s `err`/`b_err` locals, lines ~282-283 and the plural/select branch loop) must release the discarded owned string before overwriting or returning.

### Task 2 — verify `plural_category`'s return ownership (line ~462, 472)
`category = plural.plural_category(locale, count)`. Determine whether `std/plural/module.ae::plural_category` returns an **owned** string or a **borrowed literal**:
- Read `std/plural/module.ae`. Its per-locale fns `return "one"` / `"other"` etc. — **string literals**. `plural_category_decimal` returns `result` which is bound to one of those literals (or the default `"other"`). String literals in Aether are NOT owned heap allocations you should free.
- Therefore `string_release(category)` at line 472 is very likely a **no-op at best** (literal → not magic, or magic-static) and possibly wrong. Confirm by testing: try removing the `string_release(category)` and re-measuring. If the count is unchanged or lower, the release was pointless (remove it). If it goes up, keep it. **Let valgrind decide** — do not assume.
- (This is the one place the answer is genuinely "measure it." Everything else follows the table above.)

### Task 3 — the formatter temporaries (leak class 2, ~20 blocks, the 35-byte swarm)
In `format_nodes` (lines ~418–544), find every owned temporary and release it after use:
- `count_str` (line ~436-439): if it comes from `map_get_raw` it's **borrowed** (don't free); if from a literal `"0"` it's not owned. But `to_int(count_str)` (line 440) — check whether `to_int` returns an owned err string that leaks.
- `sel_val_str = string_substring(...)` (line ~450) — **owned**, release after `to_int` uses it, on every loop iteration (this is inside a `while`, so it leaks once per iteration if not freed).
- `err_sel` from `to_int` (line ~451) — owned `""` on success, release it.
- `branch_str = format_nodes(...)` (lines ~494, ~533) — this is a `finish()` result (plain char*). It is appended then must be freed. Because `string_release` is a no-op on it (Trap B), the correct fix is to let its slot be freed by the tracker — the cleanest reliable way is to **not hold it in a bare local that outlives its use**. Options, in order of preference:
  1. Append and immediately reassign the slot: after `strbuilder.append(sb, branch_str)`, set `branch_str = ""` so the tracker frees the plain char* on reassignment — **BUT verify with valgrind** (Trap C says this isn't always reliable; if it doesn't drop the leak, use option 2).
  2. If option 1 doesn't clear it, add a plain-`free` extern for char* and call it, OR restructure so `format_nodes` appends child output directly into the parent `sb` (pass `sb` down) instead of returning a finished string per branch — this eliminates the per-branch `finish()` allocation entirely and is the most robust fix. This is a slightly larger refactor but kills the whole 35-byte swarm at the source. **Recommended.**
- `sel_val` (line ~500) from `map_get_raw` — **borrowed**, don't free.

### Task 4 — the `[missing: id]` concat leak (catalog_format, lines ~602-610)
`string_concat("[missing: ", string_concat(id, "]"))` — the **inner** `string_concat(id, "]")` produces an owned temporary that the outer concat copies but never frees. Fix by binding the inner to a local, using it, and releasing it:
```
inner = string_concat(id, "]")
res = string_concat("[missing: ", inner)
string_release(inner)
return res
```
(Only matters on missing-id / null-cat paths, but the test exercises missing-id, so it counts.)

### Task 5 — the parse-side substring/concat leaks (parse_block etc.)
`arg_name`/`type_name` (lines ~227, ~258) are `string_trim(string_substring(...))` — **owned**. They are either:
- transferred into `n.text` (line ~236 `n.text = arg_name`) → ownership moves to the Node, freed by `node_free`; do NOT also free `arg_name` — but do NOT leave a dangling second owner either. After `n.text = arg_name`, the slot `arg_name` still names the same owned string; make sure you don't double-free it later (`node_free` will free `n.text`). Don't add `string_release(arg_name)` after assigning it into `n.text`.
- OR discarded on an error path (lines ~254, ~260-261 set them to `""`) → must actually free the owned substring before discarding. Replace `arg_name = ""` (as-a-free) with `string_release(arg_name)` on the error/discard paths, then don't reference it again.
- `err_msg = string_concat(...)` (line ~269) is **returned** as the owned err → caller frees it; correct as-is (just make sure the caller frees it — task 1).

Check `node_free` / `pattern_free` / `pattern_nodes_free` (lines ~59–101) free `n.text` and each `Branch.selector`/`Branch.pattern` recursively, so field-owned strings are reclaimed. If a `Node.text` was set from a borrowed/literal source in one kind and an owned source in another, that inconsistency will show up as either a leak (owned, not freed) or a double-free (literal freed) — verify all four `kind`s set `text` to an owned string (or empty) consistently.

---

## Method (do this — it's how you avoid the regressions I hit)

Work **one leak class at a time**, smallest first, and **re-measure after each change** with a clean build:

1. Fix class 1 (`err`/`""` frees). Clean-build. `valgrind`. Confirm the 1-byte swarm dropped and `All PASS` still holds. If a change *raises* the count, revert it and reconsider ownership — a rise means you freed something borrowed or double-freed.
2. Fix class 2 (formatter temporaries; prefer the "pass `sb` down" refactor for `branch_str`). Clean-build. `valgrind`. `All PASS`.
3. Fix class 4/5 (concat + parse-side). Clean-build. `valgrind`. `All PASS`.
4. Iterate until `definitely lost: 0` AND `indirectly lost: 0`.

A change is only kept if it (a) does not break `All PASS`, and (b) strictly lowers or holds `definitely lost + indirectly lost`. If unsure whether something is owned, **test both ways and let valgrind adjudicate** (this is the reliable oracle here — the type system won't tell you).

---

## Proof of correctness (paste all of this into your final report)

```
# clean build (MANDATORY before measuring — stale cache lies)
rm -rf ~/.aether/cache && rm -f build/test_*

# 1. functional: must print "All PASS"
./build/ae run tests/regression/test_message.ae

# 2. leak proof: must show 0 definitely + 0 indirectly lost
./build/ae build tests/regression/test_message.ae -o /tmp/msg
valgrind --leak-check=full --num-callers=8 /tmp/msg 2>&1 | \
  grep -E "definitely lost|indirectly lost|ERROR SUMMARY|All PASS"

# 3. fmt gate: must produce NO changes (CI enforces this)
./build/ae fmt std/message/module.ae tests/regression/test_message.ae
git diff --stat            # expect: only your intended edits, fmt made nothing extra
```

Success criteria:
- `All PASS`
- `definitely lost: 0 bytes in 0 blocks`
- `indirectly lost: 0 bytes in 0 blocks`
- `ERROR SUMMARY: 0 errors from 0 contexts`
- `.ae fmt` changes nothing (idempotent formatting)
- **No public API change** — `format`, `format_pattern`, `parse`, `catalog_new/add/format/free` keep their signatures. This is a memory-discipline fix only.

## Constraints
- Do **not** commit and do **not** open a PR. Leave edits in-situ on `feat/i18n-message-phase3`.
- Do **not** weaken `tests/regression/test_message.ae` to hide a leak. Adding a legitimately-needed free to the test harness is fine; deleting assertions is not.
- Preserve Jules's authorship framing — this is a follow-up leak fix on his work, not a rewrite.
- Keep the change minimal and idiomatic; match the surrounding style. The `pass-sb-down` refactor in Task 3 is the one place a small structural change is encouraged because it removes an entire allocation class.

## Reference files (read these for the ownership facts)
- `std/string/aether_string.c` — `string_free`/`string_release` (Trap A), `string_concat`, `string_substring` return magic AetherStrings.
- `std/strbuilder/aether_strbuilder.c::aether_strbuilder_finish` — returns plain char* (Trap B); `aether_strbuilder_append` copies its arg.
- `std/plural/module.ae` — `plural_category` returns literals (Task 2).
- `std/language/module.ae` — `parse` returns owned `(canonical, err)` tuple; Phases 1/2 (`std/language`, `std/plural`) are already-merged, leak-clean references for the same idioms.
