/* ducet.h — types for the generated DUCET table (ducet_data.c).
 *
 * The Default Unicode Collation Element Table maps a code point (or a
 * length-2 contraction) to a run of collation elements, each a
 * (primary, secondary, tertiary) weight triple (UTS #10). ducet_data.c is
 * generated from allkeys.txt by gen_ducet.py; see NOTICE for the Unicode
 * license covering the data. */
#ifndef AETHER_I18N_DUCET_H
#define AETHER_I18N_DUCET_H

#include <stdint.h>

typedef struct {
    uint16_t p; /* primary weight   */
    uint16_t s; /* secondary weight */
    uint16_t t; /* tertiary weight  */
} AetherCE;

typedef struct {
    uint32_t cp;      /* code point */
    uint32_t ce_off;  /* offset into aether_ducet_ce[] */
    uint16_t ce_len;  /* number of collation elements */
} AetherDucetSingle;

typedef struct {
    uint32_t cp0;     /* first code point of the contraction  */
    uint32_t cp1;     /* second code point of the contraction */
    uint32_t ce_off;  /* offset into aether_ducet_ce[] */
    uint16_t ce_len;  /* number of collation elements */
} AetherDucetContract;

extern const int aether_ducet_ce_count;
extern const AetherCE aether_ducet_ce[];

extern const int aether_ducet_single_count;
extern const AetherDucetSingle aether_ducet_single[];

extern const int aether_ducet_contract_count;
extern const AetherDucetContract aether_ducet_contract[];

#endif /* AETHER_I18N_DUCET_H */
