#ifndef AETHER_CODEGEN_LAYOUT_H
#define AETHER_CODEGEN_LAYOUT_H

/* `aetherc --emit=layout` (#3775): the size, alignment, field offsets and
 * padding of every struct, message and actor a file declares, as the C that
 * codegen just generated lays them out, with the field order that would be
 * tighter and whether `ae pad --fix` may rewrite the source to it. Drives
 * `ae pad` (tools/ae_pad.c). */

#include <stdio.h>
#include "../ast.h"
#include "../analysis/type_layout.h"
#include "codegen.h"

typedef enum {
    LAYOUT_REPORT_TEXT,     /* one block per type, then a summary line */
    LAYOUT_REPORT_JSON,     /* the same as one JSON object */
    LAYOUT_REPORT_PLAN,     /* `fix` lines for `ae pad --fix` */
    LAYOUT_REPORT_ASSERTS   /* C `_Static_assert`s of every number, for `ae pad --verify` */
} LayoutReportFormat;

/* Writes the report of what `path` declares to `out`. `gen` is the code
 * generator that generated the program: it knows which actor state fields
 * it widened to pointers and which functions an --emit=lib build exports.
 * `target_name` names the table in the report. Returns 0. */
int codegen_layout_report(CodeGenerator* gen, ASTNode* program, const char* path,
                          const LayoutTarget* tgt, const char* target_name,
                          LayoutReportFormat format, FILE* out);

#endif
