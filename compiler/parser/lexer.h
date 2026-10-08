#ifndef LEXER_H
#define LEXER_H

#include "tokens.h"

// Lexer state snapshot for re-entrant lexing (used by string interpolation parser)
typedef struct {
    const char* source;
    int source_length;
    int current_pos;
    int current_line;
    int current_column;
} LexerState;

// Lexer functions
void lexer_init(const char* src);
void lexer_save(LexerState* out);
void lexer_restore(const LexerState* in);
Token* next_token(void);

// Internal lexer functions (exposed for testing)
char peek(void);
char advance(void);
void skip_whitespace(void);
int skip_comment(void);
Token* read_string(void);
// Decode the escape at p (p[0] is the backslash); returns the bytes it spans (#2512).
int lexer_decode_escape(const char* p, char* out);
Token* read_number(void);
Token* read_identifier(void);
Token* read_raw_identifier(void);

#endif // LEXER_H

