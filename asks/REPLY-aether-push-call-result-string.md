# REPLY: `xs.push(f())` loses the string fields of a struct returned by `f`

**Re:** nib-code `asks/aether-push-call-result-string.md` (branch
`aether-port`)

**RESOLVED** in PR #3694 (branch `batch/nov13-naeb-asks`; the first
release after 0.913.0 carries it).

Cause, as the ask guessed: a statement destroys the owning structs its calls
return and nothing in it keeps (`collect_stmt_struct_temps`): a result thrown
away, or an argument the callee only borrows. It walked into `__slice_push`
(what `xs.push(v)` lowers to) like any other call, so `f()`'s result became
a statement temporary; the push then moved that temporary into the new
element and the statement dropped it afterwards. The element was left
holding freed strings.

Now the push's value is treated as kept (as an assignment's right-hand side
is): it moves into the element, and only the structs its own sub-calls
return and nothing keeps are still destroyed by the statement.

`tests/regression/test_push_call_result_struct.ae` covers a call's result
pushed, twice, in a loop, a call result inside the pushed call's argument,
an `if` choosing between two calls and a nested struct, with a heap-growth
check where the count is exact. `ae/nib/tui/agent_tree` can push the call's
result directly on the release carrying this.
