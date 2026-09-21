# std.math

Floating-point maths and integer helpers.

The float functions mirror C's `math.h` — `sqrt`, `pow`, the trigonometric
and logarithmic families — over Aether's `float` (a double). The integer
helpers carry an explicit suffix (`abs_int`, `min_int`) because Aether does
not overload on argument type.

```aether,run
import std.math

main() {
    println("sqrt(16)  = ${math.sqrt(16.0)}")
    println("abs_int(-3) = ${math.abs_int(-3)}")
}
```
```output
sqrt(16)  = 4
abs_int(-3) = 3
```

Float formatting here follows the C locale, so `4` prints as `4` on every
platform. Rendering a number for a person to read — with grouping and a
locale-appropriate decimal separator — is `std.number`'s job.

## Exports

`sqrt`, `pow`, `exp`, `log`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `atan2`, `floor`, `ceil`, `round`, `lrint`, `deg_to_rad`, `rad_to_deg`;
`abs_int`, `abs_float`, `min_int`, `max_int`, `min_float`, `max_float`,
`clamp_int`, `clamp_float`; `random_seed`, `random_int`, `random_float`; and
the constants `pi`, `tau`, `e`.

Single precision, for `f32` code (#2151): `sqrt_f32`, `abs_f32`, `min_f32`,
`max_f32`, `clamp_f32`, `sin_f32`, `cos_f32`, `tan_f32`, `atan2_f32`,
`floor_f32`, `ceil_f32`, `pow_f32`, `exp_f32` — libm's `f` functions, an
`f32` in and an `f32` out, so `math.sqrt_f32(dot(v, v))` in an `f32`
pipeline is one float `sqrt`. The double functions take an `f32` too, but
widen it and return `float`.

`random_*` is a plain PRNG for simulation and sampling — not a CSPRNG. Use
`std.cryptography.random_bytes` for anything security-bearing.

`round` and `lrint` both round to nearest but differ in two ways that matter:

| | returns | halfway cases |
|---|---|---|
| `round(x)` | float | away from zero — `round(0.5)` is 1.0 |
| `lrint(x)` | long | to even — `lrint(0.5)` is 0, `lrint(1.5)` is 2 |

Use `lrint` when you want an integer out, rather than `round(x) as int`: it
saves the cast, and `as int` truncates, so `round` composed with a cast is only
correct because `round` has already moved the value to a whole number.

`lrint` also exists so callers do not declare `extern lrint` themselves. An
Aether extern cannot spell libm's prototype: an Aether long return emits C
int64_t and an int return emits C int, and C's own long type is neither — so
such a declaration collides with the one in math.h. The collision is invisible
on Linux, where int64_t and long are the same type, and a hard error on
macOS/iOS, where int64_t is long long.
