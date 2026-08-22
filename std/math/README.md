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
`atan`, `atan2`, `floor`, `ceil`, `round`, `deg_to_rad`, `rad_to_deg`;
`abs_int`, `abs_float`, `min_int`, `max_int`, `min_float`, `max_float`,
`clamp_int`, `clamp_float`; `random_seed`, `random_int`, `random_float`; and
the constants `pi`, `tau`, `e`.

`random_*` is a plain PRNG for simulation and sampling — not a CSPRNG. Use
`std.cryptography.random_bytes` for anything security-bearing.
