# HTTP handlers with per-server / per-route state

Server handlers registered via `http.server_get` (and friends:
`server_post`, `server_put`, `server_delete`, `server_add_route`)
receive three arguments:

```aether
handler(req: ptr, res: ptr, ud: ptr)
```

`ud` is the **user_data** slot, a `ptr` that the route remembers and
passes back to the handler on every invocation. The same slot exists
on the C side (`HttpHandler`'s third arg) and is plumbed end-to-end:
whatever you pass as `user_data` to `server_get` is what the handler
receives as `ud`.

This is the in-language way to give handlers access to per-server or
per-route state, a database handle, a config struct, an auth scheme,
a connection pool. No process-globals, no C shim.

## The pattern

Declare a struct, `malloc`+init it, and pass the resulting `ptr` as
the `user_data` argument. The handler casts back via
`ud as *YourStruct`.

```aether
import std.http
extern malloc(n: int) -> ptr

struct AppCtx {
    db: ptr            // sqlite handle, redis client, etc.
    data_dir: string
    request_count: int
}

handle_get(req: ptr, res: ptr, ud: ptr) {
    ctx = ud as *AppCtx
    http.response_set_status(res, 200)
    dir = ctx.data_dir
    http.response_set_body(res, "data_dir=${dir}")
}

main() {
    ctx = malloc(64) as *AppCtx
    ctx.db = open_database()
    ctx.data_dir = "/var/lib/myapp"
    ctx.request_count = 0

    server = http.server_create(8080)
    http.server_get(server, "/info", handle_get, ctx)
    // ...start server...
}
```

## Per-route isolation

Each route has its own `user_data` slot. A server can host routes
with distinct contexts without any coordination:

```aether
ctx_v1 = malloc(64) as *ApiCtx
ctx_v1.version = 1

ctx_v2 = malloc(64) as *ApiCtx
ctx_v2.version = 2

http.server_get(server, "/v1/*", handle_v1, ctx_v1)
http.server_get(server, "/v2/*", handle_v2, ctx_v2)
```

The handler invoked for `/v1/...` sees `ctx_v1`; the handler invoked
for `/v2/...` sees `ctx_v2`. Same handler function with different
`ud` is allowed too, the slot is set per route, not per function.

## Lifetime

The struct `ctx` must outlive every request the route serves. In
practice that's the lifetime of the server (the whole process), which
is what the `malloc` above gives you, no GC, the bytes stay live
until you `free` them. If you stop the server and start a new one
mid-process, free the old context after `server_stop` returns.

A common shortcut for app-wide state: allocate it once in `main()`
and never free it. The OS reclaims the pages on process exit.

## Mutating fields from the handler

Fields can be written too, `ctx.request_count = ctx.request_count + 1`
inside a handler bumps a process-wide counter. The pointer is shared,
so any handler with the same `ud` sees the same struct.

This is **not** atomic, if multiple handlers on different actors
(or multiple actors registered to the same `ud`) might race, funnel
the writes through a single owning actor, or use a per-actor
accumulator that you merge at shutdown.

## Passing `0`

`http.server_get(server, "/health", handle_health, 0)` continues to
work, the handler receives `ud == null`. Opting into `user_data` is
purely additive; existing code that threads `0` keeps working with no
changes.

## Why this matters

Before this contract was explicitly documented, port authors concluded the
`user_data` slot was unusable from Aether and reached for a C shim of
process statics to hold things like a DB handle. The slot was always
wired, what was missing was the worked example. That's what this doc
and the test at `tests/integration/http_handler_user_data/` provide.
(Module-level mutable `var` is now also available for app-wide state,
but the per-route `user_data` slot remains the right
tool when state is scoped to a route rather than the whole process.)

## See also

- `tests/integration/http_handler_user_data/server.ae` the working
  regression test for the per-route + null-ud cases.
- `std/mem/module.ae` `mem.ptr_to_long` / `mem.long_to_ptr` if you
  need to round-trip a pointer through an Aether `long` (rare; the
  direct `ptr` shape used above is preferable when you can use it).
