/* Compiles only when AETHER_CFLAGS_EXTRA reached this compile (it defines
 * AE_EXTRA_MARKER) and aether.toml's [build] cflags did too. */
#ifndef AE_EXTRA_MARKER
#error "AE_EXTRA_MARKER not defined: AETHER_CFLAGS_EXTRA was not applied"
#endif
#ifndef AE_TOML_MARKER
#error "AE_TOML_MARKER not defined: aether.toml [build] cflags were dropped"
#endif

int ae_cflags_extra_probe(void) { return AE_EXTRA_MARKER * AE_TOML_MARKER; }
