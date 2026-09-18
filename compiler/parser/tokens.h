#ifndef TOKENS_H
#define TOKENS_H

typedef enum {
    // Keywords
    TOKEN_ACTOR,
    TOKEN_MAIN,
    TOKEN_FUNC,
    TOKEN_LET,
    TOKEN_VAR,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,
    TOKEN_WHILE,
    TOKEN_SWITCH,
    TOKEN_CASE,
    TOKEN_DEFAULT,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_RETURN,
    TOKEN_DEFER,
    TOKEN_BUILDER,          // 'builder' keyword for configure-then-execute functions
    TOKEN_MATCH,
    TOKEN_WHEN,
    TOKEN_RECEIVE,
    TOKEN_SEND,
    TOKEN_SPAWN_ACTOR,
    TOKEN_SPAWN,
    TOKEN_MAKE,
    TOKEN_SELF,
    TOKEN_STATE,
    TOKEN_STRUCT,
    TOKEN_UNION,
    TOKEN_IMPORT,
    TOKEN_AS,               // 'as' keyword for import aliasing
    TOKEN_EXPORT,
    TOKEN_EXPORTS,          // 'exports' — top-of-file public-API list
    TOKEN_MODULE,
    TOKEN_MESSAGE_KEYWORD,  // 'message' keyword for message definitions
    TOKEN_REPLY,            // 'reply' keyword for sending responses
    TOKEN_EXCLAIM,          // '!' operator for fire-and-forget
    TOKEN_QUESTION,         // '?' operator for ask pattern
    TOKEN_QUESTION_QUESTION,// '??' null-coalescing operator (#340)
    TOKEN_QUESTION_DOT,     // '?.' optional-chaining operator (#340)
    TOKEN_EXTERN,           // 'extern' keyword for C FFI
    TOKEN_NULL,             // 'null' keyword for null pointer literal
    TOKEN_CONST,            // 'const' keyword for top-level constants
    TOKEN_IN,               // 'in' keyword for range-based for loops
    TOKEN_AFTER,            // 'after' keyword for receive timeouts
    TOKEN_CALLBACK,         // 'callback' keyword for callback trailing blocks
    TOKEN_HIDE,             // 'hide' scope-level directive: blocks named outer bindings
    TOKEN_SEAL,             // 'seal' scope-level directive: blocks all but a whitelist
    TOKEN_EXCEPT,           // 'except' modifier following 'seal'
    TOKEN_TRY,              // 'try' — start of a try/catch block
    TOKEN_CATCH,            // 'catch' — catch clause binding the panic reason
    TOKEN_PANIC,            // 'panic' — unwind the current actor/try frame
    TOKEN_REQUIRES,         // 'requires' — function-level precondition (issue #348)
    TOKEN_ENSURES,          // 'ensures'  — function-level postcondition (issue #348)

    // Types
    TOKEN_INT,
    TOKEN_INT64,
    TOKEN_UINT64,
    TOKEN_DURATION,         // 'Duration' type — int64 nanoseconds
    TOKEN_FLOAT,
    TOKEN_BOOL,
    TOKEN_BYTE,             // 'byte' type — unsigned 8-bit (`unsigned char` in C)
    TOKEN_STRING,
    TOKEN_ACTOR_REF,
    TOKEN_MESSAGE,
    TOKEN_PTR,              // 'ptr' type for void* (C interop)
    
    // Literals
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING_LITERAL,
    TOKEN_INTERP_STRING,    // string literal containing ${expr} interpolation
    TOKEN_TRUE,
    TOKEN_FALSE,
    
    // Operators
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_MULTIPLY,
    TOKEN_DIVIDE,
    TOKEN_MODULO,
    TOKEN_ASSIGN,
    TOKEN_EQUALS,
    TOKEN_NOT_EQUALS,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,
    TOKEN_INCREMENT,
    TOKEN_DECREMENT,
    TOKEN_AMPERSAND,        // '&' bitwise AND
    TOKEN_CARET,            // '^' bitwise XOR
    TOKEN_TILDE,            // '~' bitwise NOT
    TOKEN_LSHIFT,           // '<<' left shift
    TOKEN_RSHIFT,           // '>>' right shift
    TOKEN_PLUS_ASSIGN,      // '+='
    TOKEN_MINUS_ASSIGN,     // '-='
    TOKEN_MULTIPLY_ASSIGN,  // '*='
    TOKEN_DIVIDE_ASSIGN,    // '/='
    TOKEN_MODULO_ASSIGN,    // '%='
    TOKEN_AND_ASSIGN,       // '&='
    TOKEN_OR_ASSIGN,        // '|='
    TOKEN_XOR_ASSIGN,       // '^='
    TOKEN_LSHIFT_ASSIGN,    // '<<='
    TOKEN_RSHIFT_ASSIGN,    // '>>='
    TOKEN_DOTDOT,           // '..' range operator (exclusive, as in `for i in 0..5`)
    TOKEN_DOTDOT_EQ,        // '..=' inclusive range in match/switch case labels (#1047)
    TOKEN_DOTDOT_LT,        // '..<' half-open range in match/switch case labels (#1047)
    TOKEN_DOTDOTDOT,        // '...' varargs marker (extern only, v1)

    // Delimiters
    TOKEN_LEFT_PAREN,
    TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE,
    TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_BRACKET,
    TOKEN_RIGHT_BRACKET,
    TOKEN_SEMICOLON,
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_COLON,
    TOKEN_ARROW,
    TOKEN_PIPE,
    TOKEN_AT,               // '@' attribute / annotation prefix

    // Special
    TOKEN_PRINT,
    TOKEN_EOF,
    TOKEN_ERROR
} AeTokenType;

typedef struct {
    AeTokenType type;
    char* value;
    int line;
    int column;
} Token;

// Lexer functions
void lexer_init(const char* src);
Token* next_token(void);
// Lex all of `src` into a heap array whose last entry is the TOKEN_EOF, or
// the TOKEN_ERROR that stopped the scan. The array grows with the input, so
// the only limit on a source file is memory; NULL means memory ran out.
// Release with free_tokens.
Token** lexer_tokenize(const char* src, int* out_count);

// Token functions
Token* create_token(AeTokenType type, const char* value, int line, int column);
void free_token(Token* token);
void free_tokens(Token** tokens, int count);
const char* token_type_to_string(AeTokenType type);

#endif
