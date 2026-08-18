# DSL without macros, why Aether doesn't need `macro_rules!`

A short comparison between Aether's trailing-block DSL story (see
[`closures-and-builder-dsl.md`](closures-and-builder-dsl.md)) and the Rust
macro tower (`macro_rules!` → proc-macros → `syn`/`quote`) that Rust DSLs
have to climb. The point isn't a beauty contest; it's that the two designs
hit different ceilings for different reasons, and being honest about both
is useful when choosing or evaluating either.

## The shape under discussion

A nested, declarative-feeling DSL, the GUI / SwiftUI / kotlinx.html / Sinatra
shape:

```aether,fragment
frame("App") {
    panel("Controls") {
        button("OK")
        button("Cancel")
    }
}
```

In Rust this would typically be a function-like macro:

```rust
gui! {
    frame("App") {
        panel("Controls") {
            button("OK");
            button("Cancel");
        }
    }
}
```

Both produce the same surface. The machinery underneath is what differs.

## Three-way comparisons

The GUI shape above is the easy case, flat structural nesting. The
interesting differences show up when the DSL has to carry *behaviour*,
*types*, or domain semantics the language was never designed for. Three
worked examples below; in each one the alternatives picked different
points on the trade.

### Test specs (RSpec / Jest / Aether)

Test DSLs are interesting because they have to capture *mutable state
across* `before` / `it` / `after`, *and* the assertion body needs to read
like prose. Different language families pay for that in different ways.

**RSpec (Ruby).** The DSL is trailing blocks all the way down. State
crossing `before`/`it` lives in instance variables; the closure is an
implicit `self` bound to a context object the framework builds.

```ruby
describe "Account" do
  before { @account = Account.new(balance: 100) }
  it "deducts on withdrawal" do
    @account.withdraw(30)
    expect(@account.balance).to eq(70)
  end
end
```

**Jest (JS).** Same shape, no instance variables. State crossing
`beforeEach`/`it` lives in `let` bindings the closures capture by
reference. The cost is each callback being an explicit `() => { ... }`.

```js
describe("Account", () => {
  let account;
  beforeEach(() => { account = new Account({ balance: 100 }); });
  it("deducts on withdrawal", () => {
    account.withdraw(30);
    expect(account.balance).toBe(70);
  });
});
```

**Rust** never seriously attempts this shape. `#[test]` is
function-attribute-based; `rstest`, `test_case` etc. add fixtures and
parameterised tests but don't try the nested `describe`/`it` form,
because `macro_rules!` would have to invent its own scope-and-capture
story across nested blocks. The honest answer in the Rust ecosystem is
"flat `#[test]` functions, fixtures via traits or proc-macros."

**Aether.** Trailing blocks for structure, `callback` blocks (real
closures with implicit capture, [`closures-and-builder-dsl.md`
§Callback blocks](closures-and-builder-dsl.md#callback-blocks)) for the
actual test bodies. Shared state across `before`/`it` lives in `ref`
cells the closures capture by pointer:

```aether,fragment
describe("Account") {
    account = ref(0)
    before callback { ref_set(account, account_new(100)) }
    it("deducts on withdrawal") callback {
        a = ref_get(account)
        account_withdraw(a, 30)
        expect_eq(account_balance(a), 70)
    }
}
```

The structural block (`describe { ... }`) and the closure-capturing
block (`it(...) callback { ... }`) are *different forms* of trailing
block, that distinction is what makes the DSL legible. The structural
block runs inline at registration time; the `callback` block is a real
closure stored in the test registry and invoked later. Both fall out of
the language; there's no DSL parser to write.

### SQL query builders (Diesel / jOOQ / Aether)

The interesting axis here is **type checking against the schema**, not
nesting. Each ecosystem picked a different way to get there.

**Diesel (Rust).** Proc-macros generate a typed schema module from the
DB at compile time. The query is composed via methods, but the *types*
that make `users.filter(...).select(...)` type-check are macro output.

```rust
use schema::users::dsl::*;
let actives = users
    .filter(active.eq(true))
    .order(created_at.desc())
    .limit(10)
    .load::<User>(&mut conn)?;
```

Strong correctness guarantees, expensive build-time machinery, and
schema changes require regenerating the macro output.

**jOOQ (Java).** A code-generation step builds a typed DSL from the live
schema; queries compose via a fluent builder. No macros, Java doesn't
have them, so the code-gen step is an external tool.

```java
List<UsersRecord> actives = ctx
    .selectFrom(USERS)
    .where(USERS.ACTIVE.eq(true))
    .orderBy(USERS.CREATED_AT.desc())
    .limit(10)
    .fetch();
```

Same idea as Diesel without the macro layer: the typed bindings come
from an external generator that runs against the schema.

**Aether.** Builder-flavour trailing block. The setters fill a config
object; the function executes with the filled config; the typed bindings
come from the same `--emit=lib` C-ABI shape Aether uses for everything
else.

```aether,fragment
import std.sql

actives = sql.select(users) {
    where(eq(users.active, true))
    order_by(desc(users.created_at))
    limit(10)
}
```

The seam this exposes is real: Aether's type system today doesn't have a
way to bind the schema's column types into the DSL without code-gen
either. The honest comparison is "Aether avoids the macro tower but
needs the same code-gen step Java needs for the *typed* layer."  The
ergonomic improvement over Java is in the call site, the trailing
block reads as nested clauses rather than a method chain, but the
upstream type-binding work is the same shape. A future Aether feature
that lets `--emit=ast` consume a schema and emit typed bindings would
close that gap.

### A domain that resists flat description: interactive 3D scenes

The earlier examples have a flat serialised form that captures most of
what matters, HTML for a GUI, SQL for a query. Interactive 3D doesn't.
A glTF file describes the *renderable state* of a scene; it deliberately
discards the *parametric*, *behavioural*, and *constraint* layers that
make a scene *interactive*. That's not a bug in glTF, it's the format
doing its job, which is "snapshot for renderers", but it means there's
no canonical source form a DSL can mirror.

**Concretely**, in an interactive scene the author wants to say:

- This door is the child of the room, but its open angle is a closure
  over `player_distance` and `interact_pressed`.
- This particle emitter's rate is `clamp(60, 10, distance_to_camera / 0.5)`,
  re-evaluated per frame.
- This NPC's chase target is whichever of `[player, decoy_1, decoy_2]`
  is closest, with a 0.5s hysteresis.

glTF can encode none of those without an `extras` field carrying a
script blob. The KHR_animation_pointer extension and the various
behaviour-graph extensions (`KHR_interactivity`, vendor-specific ones)
are explicit admissions that the serialised form lost something
essential and needs a programming language smuggled back in.

**Three actual alternatives:**

**Unity prefabs (C# `MonoBehaviour`).** The scene is a tree of
GameObjects with components attached, edited in a visual editor and
serialised to YAML. Behaviours are C# scripts referenced by name; the
authoring happens in the editor, not in code, and the serialised form
is the editor's output, not the source of truth.

```yaml
Door:
  components:
    - Transform: { ... }
    - DoorScript: { open_angle_curve: ..., trigger_radius: 2.0 }
```

Power, but the "source" is the editor; reading the YAML doesn't tell
you what the door *does*.

**Three.js (JS fluent code).** The scene is built programmatically:

```js
const door = new THREE.Mesh(doorGeo, doorMat);
room.add(door);
clock.addEventListener('tick', () => {
  if (distance(player, door) < 2 && input.E) {
    door.rotation.y = lerp(door.rotation.y, openAngle, 0.1);
  }
});
```

No DSL surface at all, it's just a JS codebase. Behaviour is first-
class (closures over scene state), but structure (parent-child, layout,
constraints) is scattered across `.add()` calls and there's no nesting
to read.

**Aether.** Trailing blocks for structure, real closures for behaviour,
both first-class:

```aether,fragment
import contrib.scene as s

room = s.room("Library") {
    door = s.door("east_exit") {
        position(4.0, 0.0, 0.0)
        open_angle = ref(0.0)
        s.on_frame() callback {
            if s.distance(s.player(), door) < 2.0 && s.input("E") {
                ref_set(open_angle,
                        s.lerp(ref_get(open_angle), 90.0, 0.1))
            }
            s.set_rotation(door, 0.0, ref_get(open_angle), 0.0)
        }
    }
    s.npc("librarian") {
        s.chase_target() callback {
            return s.nearest(s.player(), [s.decoy(1), s.decoy(2)])
        }
    }
}
```

Three things this gets that the serialised forms structurally can't:

1. **Structure and behaviour at the same level.** The closure that
   updates `open_angle` is written next to the `door` declaration that
   owns it. No "go look in the script file with the matching name."
2. **Real expressions in parameter slots.** `s.chase_target()` takes a
   `callback`; the body is just Aether code, `if`, function calls,
   `for`, all of it. No expression mini-language to parse or escape.
3. **No loss of fidelity round-tripping to a renderer.** You can still
   *export* glTF from this scene, but the source of truth is the
   Aether code, not a serialised snapshot. The closures don't survive
   the export because they're not renderable state, that's the
   point.

The Rust equivalent of this would need a behaviour-tree crate's macros
(`bevy`'s ECS macros, for example), and the macros pay for everything
they buy by being their own learning curve. Aether's trailing-block
mechanism is the same one that handles GUIs and tests and queries, one
construct, applied across domains.

## Why Rust needs a macro layer

Rust the language doesn't have first-class trailing blocks that execute in
the caller's scope. A call like `panel("Controls") { button("OK") }` isn't
valid Rust, `{ ... }` after a call site is a separate expression, not an
argument to `panel`. So a nested DSL has to be *parsed by user code*.

The standard escalation:

1. **`macro_rules!`**, declarative, pattern-match over token trees.
   Works for `vec![1, 2, 3]`, `hashmap! { "k" => "v" }`. Hits a wall on
   arbitrary nesting: error messages on typos inside the block become
   "expected `something`, got `something else`" pointing into expanded
   code, with no real source context. The Little Book of Rust Macros
   covers TT-munchers and the recursion patterns; the part that gets quiet
   is exactly where production DSLs care most.

2. **Proc-macros + `syn`/`quote`**, write a parser in Rust, over a
   `TokenStream`, using the same crates rust-analyzer uses internally.
   This is real work: hand-built recursive descent, span tracking for
   each token so errors point somewhere useful, and re-emitting code as
   another `TokenStream`. `syn` exists because this is hard enough that
   nobody should write it from scratch.

The pattern people hit: the declarative-macro docs feel thin exactly at
the point where the real problem starts. That's not a gap in the docs, it's
the tool running out of road. `macro_rules!` is a pattern-matching DSL over
token trees, and arbitrary nesting with good diagnostics requires writing a
parser, which is what `syn` is.

## What Aether does instead

The trailing block isn't a macro construct, it's part of the language
grammar. `panel("Controls") { button("OK") }` parses as "call `panel`,
then run this block." The compiler injects `_aether_ctx_push(panel_result)`
before the block and `_aether_ctx_pop()` after; function calls inside the
block whose first param is `_ctx: ptr` get `_aether_ctx_get()` auto-injected
as their first argument. The whole mechanism is in
[`closures-and-builder-dsl.md`](closures-and-builder-dsl.md#builder-context-stack).

What this means in practice:

- **No parser to write.** The DSL surface is "function calls with a
  trailing block." Nesting works because trailing blocks nest. There's no
  user-side recursion to hand-build.
- **Errors come from the regular typechecker.** A typo inside `panel { ... }`
  surfaces as "undefined `buton`" at the line you wrote it, with the same
  diagnostic quality as any other call site. The receiver-scoping rules
  ([`closures-and-builder-dsl.md` §DSL Block Receiver Scoping](closures-and-builder-dsl.md#dsl-block-receiver-scoping))
  even let bare-name calls inside a `bash.test(b) { ... }` block fall back
  through `bash_<name>` without a companion `import bash (script, jobs)`
  line.
- **Refactor safety.** Rename a setter and every call site updates,
  because the setter is a regular function, not a macro-synthesised
  identifier. rust-analyzer's rename across macro bodies is a separate
  feature with separate caveats; Aether's editor tooling sees the DSL
  body as ordinary code.
- **No second language.** Rust DSLs effectively live in a parallel
  syntactic universe (the macro body's grammar), and `syn` is a
  compatibility shim that makes that universe look like Rust. Aether
  DSL bodies *are* Aether, `for`, `if`, `${...}` interpolation,
  closures, ref cells, `defer` all work and behave identically to how
  they work outside the block.

## Where Aether hits its own ceiling

This is the honest part. Aether's trailing block is *imperative*, it
runs its statements in source order. A DSL where order shouldn't matter
(declarative attributes, a graph that wants topological resolution, a
spec where the user describes *what* and not *the order of operations*)
sits awkwardly.

The escape hatch is the builder variant, `builder ... with <factory>`
([`closures-and-builder-dsl.md` §Builder Functions](closures-and-builder-dsl.md#builder-functions-configure-then-execute)).
The block runs first against a config object filled via setter calls;
the function runs second with that config. This is the closest analogue
to a real declarative DSL and it works for most cases:

```aether,fragment
builder compile(src: string) {
    rel = ""
    if _builder != null {
        if map_has(_builder, "release") == 1 {
            rel = map_get(_builder, "release")
        }
    }
    println("compiling ${src} with release=${rel}")
}

// User writes, block fills config, function runs with it
compile("Main.java") {
    set_release("21")
    set_lint("all")
}
```

But there's still ordering: the setters fire in source order, so a
"genuinely order-independent" spec has to be reduced to "fill a bag of
fields, then resolve." If you need actual declarative semantics where
the runtime is allowed to reorder, you build the bag explicitly in the
block and post-process it after the call returns. That's a real seam.

Rust macros have the same problem, they only see token trees, so they
can reorder syntactically but can't reorder *semantically* without
emitting a second compile pass. Different ceiling, same general
neighbourhood.

## The trade summarised

| Concern | Rust (`macro_rules!` → `syn`) | Aether (trailing blocks + `_ctx`) |
|---|---|---|
| Surface for `panel { button { ... } }` | Macro-defined | Language-native |
| Who parses the body | User code (you, via `syn`) | The Aether compiler |
| Typo diagnostics | Macro-expansion error, often pointing into expanded code | Regular typechecker error at the typo's line |
| Editor refactor support | Limited inside macro bodies | Same as ordinary code |
| Cost to add a setter / nested form | New macro arm or new proc-macro logic | New function with `_ctx: ptr` first param |
| Order-independent DSLs | Hard (token trees only) | Hard (block is imperative; `builder...with` is the workaround) |
| Reach if the language doesn't bend | Macros bend it for you (powerful, expensive) | None, you're stuck with what trailing blocks express |

The trade is: Rust pays in tooling complexity to get unbounded syntactic
power. Aether pays in expressive ceiling to get diagnostics-for-free and
no second language.

## When this matters

If a Rust crate's README shows a deeply-nested DSL macro and the linked
issue tracker has a recurring "error messages inside `foo!` are awful"
thread, that's the macro-tower tax showing up. If an Aether project
needs a DSL whose semantics genuinely don't care about statement order,
that's the imperative-block tax showing up.

For the nested-builder shape both designs aim at, GUIs, HTML emission,
build descriptions, configuration objects, Aether reaches the surface
with less machinery. For DSLs whose body is *not* a sequence of effects
(constraint solvers, regex builders that want to be a pure tree, anything
the user thinks of as "data not code"), Rust macros still have more
reach, at the cost of all the parsing work.

---

Companion read: [`closures-and-builder-dsl.md`](closures-and-builder-dsl.md)
covers the implementation in detail, closure compilation, `_ctx`
injection, the builder context stack, ref cells for shared mutable state,
and the same-line rule that makes `result = build()` not greedily eat the
next-line block.
