# std.schema — a projection-first roadmap (Ash, without the framework)

## Where this comes from

`std.schema` borrowed its declarative `record { field { validation } }` shape
from Ash (alongside Zod/Pydantic/io-ts for the parse-contract and coercion). But
that shape is Ash's *thinnest outer ring*. Ash is a declarative
resource/domain framework whose real substance is everything behind the
resource: relationships, actions, lifecycle hooks, calculations/aggregates, a
pluggable data layer with a query DSL and SAT-solver-backed authorization
policies — and, crucially, **projections**: one resource declaration generates
JSON:API, GraphQL, OpenAPI, admin UIs, and typed code interfaces.

The question this doc answers: *how much of Ash should Aether pursue, and where
is the line?*

## The thesis worth stealing

Ash's superpower is: **a resource is a single declarative source of truth that
you project into many surfaces.** Validation, persistence, JSON:API, GraphQL,
OpenAPI, admin, and authorization all *derive* from one declaration.

Aether has already started down exactly this path, perhaps without naming it:

| Ash concept | Aether today |
|---|---|
| resource declaration | `std.schema` `record { field { rules } }` |
| a projection | `std.schema.to_json_schema()` (draft-07) |
| the web projection | `contrib/tinyweb/schema_api` — validation + `openapi_route` (OpenAPI 3.0) |

## The strategic line

Ash's depth is also its weight. Aether is a systems language that compiles to
readable C with **no GC and a manual-memory model** — not an Elixir/OTP
application framework. So the line is:

- **Lean hard into projection** (pure, compile-time, "one declaration → many
  artifacts"). This is Ash's most distinctive idea AND the one that fits Aether
  best: projections are just codegen over a schema value — no runtime, no
  persistence, no GC pressure. It's the same muscle the compiler already has.
- **Add modeling depth only where it feeds projection** (relationships make
  richer JSON Schema / OpenAPI / GraphQL; that's the justification).
- **Stay out of the framework/runtime tiers.** Data-layer-as-framework,
  a policy/SAT-solver, sagas, PubSub, multitenancy — that's Elixir-ecosystem
  weight that doesn't map to a systems language. `contrib/*` bridges (e.g.
  `sqlite`) remain the honest place for persistence, consumed explicitly.

## Tiers

### Tier A — projection-first (the plan)

Each is a pure function `schema -> string`, no runtime, mirroring
`to_json_schema()`. Build order roughly by value/effort:

1. **`to_openapi()`** — promote the FastAPI-style aggregate currently living in
   `tinyweb/schema_api` into a first-class `std.schema` projection (paths +
   `components.schemas`). The web layer becomes a thin caller.
2. **`to_typescript()`** — emit a TypeScript `interface`/`type` for a resource.
   Instant client-side type-safety for anyone consuming an Aether JSON API;
   very high "wow", trivial to keep pure.
3. **Relationships** (`belongs_to`/`has_many`) — the one modeling addition that
   pays for itself: it turns a schema into a *domain model* and makes every
   projection richer (`$ref` links in JSON Schema/OpenAPI, nested TS types).
   Depends on nested records (issue #1446). This is the gateway to A4/A5.
4. **`to_graphql_sdl()`** — emit GraphQL SDL types from a resource (+ its
   relationships). Aether's answer to AshGraphql, as pure codegen.
5. **Form/scaffold projection** — emit an HTML form (or a tinyweb form handler)
   from a schema. The "admin UI from a declaration" idea, kept to static output.
   The dynamic counterpart — a *live*, validating form that pushes per-field
   errors over a WebSocket — is Tier B of the sibling
   `docs/liveview-lite-roadmap.md`: the same `record`, projected onto the
   LiveView surface instead of a static page.

A shared spine: a small internal schema-introspection surface (iterate fields,
their types, rules, relationships) that every `to_*` projection walks — so
adding a projection is writing one walker, not re-reading internals.

### Tier B — modeling depth (only with a concrete consumer)

- **Actions** — named typed operations (beyond raw CRUD); `schema_api` already
  half-expresses this with `create/show/index`. Light, but only worth it when a
  real API needs custom action arguments distinct from stored fields.
- **Lifecycle hooks** (`before/after`) — declarative interception. Maps onto
  closures + the tinyweb filter chain. Useful, modest.
- **Calculations** (derived fields) — `full_name = first <> last`. Pure and
  demoable; feeds projections (a computed field in the schema output).

### Tier C — explicitly out of scope

Data-layer-as-framework + `Ash.Query` DSL + SAT-solver authorization policies +
Reactor/sagas + notifiers/PubSub + multitenancy + code-generators/Igniter. This
is where Ash becomes "huge"; it's OTP/Elixir-ecosystem weight and a persistence
runtime Aether deliberately doesn't have. Persistence stays in `contrib/`
bridges, consumed explicitly — not welded into `std.schema`.

## Why projection-first is the right first bet for Aether

- **It's pure.** No GC, no persistence, no runtime — just codegen over a schema
  value. Exactly Aether's wheelhouse and leak-free by construction (build the
  string with `strbuilder`; see the `to_json_schema` precedent, and note the
  `std.json` builder-nesting leak #1447 that motivated string-building over a
  DOM).
- **It's HTTP-agnostic.** `std.schema` stays a pure stdlib module; the web
  projection lives in `contrib/tinyweb`. An SMTP/IMAP/CLI/config consumer reuses
  the same declaration and the same projections.
- **It compounds.** Every projection added multiplies the value of every schema
  already written — the Ash flywheel, without adopting the framework.

## Concrete next steps (if pursued)

1. Land the nested-records/relationships work (#1446) — the modeling prerequisite.
2. Extract a `std.schema` introspection spine + move `to_openapi` into `std`.
3. Add `to_typescript()` (cheap, high-value) and `to_graphql_sdl()`.
4. Revisit Tier B (actions/hooks/calculations) only when a `schema_api` consumer
   asks for it.

Credits: the resource-and-projection framing is Ash's (MIT, © 2019 ash
contributors). Aether deliberately takes the *idea* (one declaration, many pure
projections) and not the framework/runtime.
