// Aether source formatter.
//
// Design: a whitespace-only, comment-preserving reformatter. It scans the
// source into a stream of significant tokens and comments (preserving the
// exact text of every token, string literal, and comment), then re-emits that
// stream with canonical layout: 4-space structural indentation, normalized
// inter-token spacing, at most one blank line between constructs, no trailing
// whitespace, and a single final newline. User line breaks are preserved (the
// formatter re-indents and re-spaces but does not reflow expressions).
//
// Safety invariant: the significant-token sequence is never reordered, dropped,
// or fused, and string/comment bytes are copied verbatim. Since the lexer
// already tokenizes `a-b` the same as `a - b`, re-spacing between separated
// tokens cannot change parsing; the one hazard, fusing two tokens that would
// re-lex as one (`a b` -> `ab`, `<` `<` -> `<<`), is prevented by would_merge().
// The formatter therefore preserves program semantics by construction.

#include "ae_fmt.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

// ------------------------------------------------------------ growable buffer
typedef struct { char* data; size_t len, cap; } Buf;
static int buf_init(Buf* b){ b->cap=256; b->len=0; b->data=(char*)malloc(b->cap); if(!b->data) return 0; b->data[0]=0; return 1; }
static int buf_reserve(Buf* b, size_t extra){
    if (b->len+extra+1 <= b->cap) return 1;
    size_t nc=b->cap ? b->cap : 256;
    while (nc < b->len+extra+1) nc*=2;
    char* nd=(char*)realloc(b->data,nc); if(!nd) return 0;
    b->data=nd; b->cap=nc; return 1;
}
static void buf_putc(Buf* b, char c){ if(!buf_reserve(b,1)) return; b->data[b->len++]=c; b->data[b->len]=0; }
static void buf_putn(Buf* b, const char* s, size_t n){ if(!buf_reserve(b,n)) return; memcpy(b->data+b->len,s,n); b->len+=n; b->data[b->len]=0; }
static void buf_puts(Buf* b, const char* s){ buf_putn(b,s,strlen(s)); }
static void buf_spaces(Buf* b, int n){ for(int i=0;i<n && buf_reserve(b,1);i++){ b->data[b->len++]=' '; b->data[b->len]=0; } }

// ------------------------------------------------------------------- lexemes
typedef enum { LX_TOKEN, LX_LINE_COMMENT, LX_BLOCK_COMMENT } LxKind;
typedef struct {
    LxKind kind;
    char* text;         // exact source slice, NUL-terminated (owned)
    int nl_before;      // '\n' count between the previous lexeme and this one
    int space_before;   // whitespace present before it on the same line (nl==0)
} Lexeme;

typedef struct { Lexeme* v; size_t n, cap; } LxVec;
static int lxvec_push(LxVec* a, Lexeme x){
    if (a->n==a->cap){ size_t nc=a->cap? a->cap*2:128; Lexeme* nv=(Lexeme*)realloc(a->v,nc*sizeof(Lexeme)); if(!nv) return 0; a->v=nv; a->cap=nc; }
    a->v[a->n++]=x; return 1;
}
static void lxvec_free(LxVec* a){ for(size_t i=0;i<a->n;i++) free(a->v[i].text); free(a->v); }

static char* slice(const char* s, size_t a, size_t b){
    size_t n=b-a; char* r=(char*)malloc(n+1); if(!r) return NULL; memcpy(r,s+a,n); r[n]=0; return r;
}

static int is_ident_start(int c){ return isalpha(c) || c=='_'; }
static int is_ident_char(int c){ return isalnum(c) || c=='_'; }
static int is_word_char(int c){ return isalnum(c) || c=='_'; }

// Multi-char operators, longest first. Single-char punctuation handled below.
static const char* OPS3[] = {"<<=", ">>=", "..=", "..<", "...", NULL};
static const char* OPS2[] = {"->","=>","==","!=","<=",">=","&&","||","++","--",
                             "<<",">>","+=","-=","*=","/=","%=","&=","|=","^=",
                             "??","?.","..", NULL};
static int op_len(const char* s){
    for (int k=0; OPS3[k]; k++) if (strncmp(s,OPS3[k],3)==0) return 3;
    for (int k=0; OPS2[k]; k++) if (strncmp(s,OPS2[k],2)==0) return 2;
    if (s[0] && strchr("+-*/%=<>&^~|!?:;,.()[]{}@", s[0])) return 1;
    return 0;
}

// Scan a "..." literal starting at s[i]=='"'. Returns index just past the
// closing quote, or 0 if unterminated. This mirrors the compiler lexer's
// read_string exactly: the string ends at the first unescaped '"' seen
// outside any ${...} interpolation, and `\<c>` is a two-character escape.
// Inside a ${...} span (tracked via interp_depth, matching '{'/'}' nesting),
// a '"' or '\"' opens a NESTED string literal rather than ending the outer
// one (#1237: `${id("hi")}` and `${id(\"hi\")}` are both valid Aether).
static size_t scan_quoted(const char* s, size_t i){
    i++; // opening quote
    int interp_depth=0;
    while (s[i]){
        char c=s[i];
        if (c=='$' && s[i+1]=='{'){ interp_depth++; i+=2; continue; }
        if (interp_depth>0 && c=='{'){ interp_depth++; i++; continue; }
        if (interp_depth>0 && c=='}'){ interp_depth--; i++; continue; }
        if (interp_depth>0 && (c=='"' || (c=='\\' && s[i+1]=='"'))){
            if (c=='\\') i++; // drop escaping backslash (opening)
            i++; // opening "
            for(;;){
                if (!s[i]) return 0;
                if (s[i]=='"'){ i++; break; }
                if (s[i]=='\\' && s[i+1]=='"'){ i+=2; break; }
                if (s[i]=='\\'){ if(!s[i+1]) return 0; i+=2; continue; }
                i++;
            }
            continue;
        }
        if (c=='\\'){ if(!s[i+1]) return 0; i+=2; continue; }
        if (c=='"') return i+1;
        i++;
    }
    return 0;
}

// Scan a `<<MARKER ... MARKER` heredoc starting at s[i]=='<' (with s[i+1]=='<'
// and s[i+2] an identifier start). Returns the index just past the closing
// marker, or 0 if unterminated. This mirrors the lexer's close detection: a
// line closes the heredoc only when, after optional leading whitespace at or
// below the shallowest body indent, it is exactly the marker followed by a line
// ending or EOF. The whole span is preserved verbatim by the formatter because
// the body's indentation is semantically significant (the `<<~` auto-dedent).
static size_t scan_heredoc(const char* s, size_t i){
    size_t j=i+2;
    char marker[256]; int mlen=0;
    while ((isalnum((unsigned char)s[j])||s[j]=='_') && mlen<255) marker[mlen++]=s[j++];
    marker[mlen]='\0';
    while (s[j] && s[j]!='\n') j++;          // skip rest of the opening line
    if (s[j]=='\n') j++;
    int min_indent=INT_MAX;
    while (s[j]){
        size_t ws=0; while (s[j+ws]==' '||s[j+ws]=='\t') ws++;
        size_t mpos=j+ws;
        int is_marker=0;
        if (strncmp(s+mpos, marker, (size_t)mlen)==0){
            char after=s[mpos+mlen];
            if (after=='\0'||after=='\n'||(after=='\r'&&s[mpos+mlen+1]=='\n')) is_marker=1;
        }
        if (is_marker && (int)ws<=min_indent) return mpos+mlen;
        int blank=(s[mpos]=='\0'||s[mpos]=='\n'||(s[mpos]=='\r'&&s[mpos+1]=='\n'));
        if (!blank && (int)ws<min_indent) min_indent=(int)ws;
        while (s[j] && s[j]!='\n') j++;
        if (s[j]=='\n') j++; else return 0;  // EOF mid-line, no closing marker
    }
    return 0;
}

static Lexeme* scan(const char* s, size_t* out_n, const char** err){
    LxVec v = {0,0,0};
    size_t i=0; int pending_nl=0, pending_sp=0;
    while (s[i]){
        char c=s[i];
        if (c=='\n'){ pending_nl++; pending_sp=0; i++; continue; }
        if (c==' '||c=='\t'||c=='\r'){ pending_sp=1; i++; continue; }

        Lexeme lx; lx.nl_before=pending_nl; lx.space_before=pending_sp;
        pending_nl=0; pending_sp=0;

        if (c=='/' && s[i+1]=='/'){
            size_t st=i; while(s[i] && s[i]!='\n') i++;
            size_t e=i; while(e>st && (s[e-1]==' '||s[e-1]=='\t'||s[e-1]=='\r')) e--;
            lx.kind=LX_LINE_COMMENT; lx.text=slice(s,st,e);
            if(!lx.text||!lxvec_push(&v,lx)){ if(err)*err="out of memory"; lxvec_free(&v); return NULL; }
            continue;
        }
        if (c=='/' && s[i+1]=='*'){
            size_t st=i; i+=2; int closed=0;
            while (s[i]){ if(s[i]=='*'&&s[i+1]=='/'){ i+=2; closed=1; break; } i++; }
            if(!closed){ if(err)*err="unterminated block comment"; lxvec_free(&v); return NULL; }
            lx.kind=LX_BLOCK_COMMENT; lx.text=slice(s,st,i);
            if(!lx.text||!lxvec_push(&v,lx)){ if(err)*err="out of memory"; lxvec_free(&v); return NULL; }
            continue;
        }
        if (c=='"'){
            size_t e=scan_quoted(s,i);
            if(!e){ if(err)*err="unterminated string literal"; lxvec_free(&v); return NULL; }
            lx.kind=LX_TOKEN; lx.text=slice(s,i,e); i=e;
            if(!lx.text||!lxvec_push(&v,lx)){ if(err)*err="out of memory"; lxvec_free(&v); return NULL; }
            continue;
        }
        // Backtick raw identifier `name` (#867): lets a reserved keyword be used
        // as an ordinary name. Preserved verbatim, including the backticks.
        if (c=='`'){
            size_t j=i+1;
            while (isalnum((unsigned char)s[j])||s[j]=='_') j++;
            if (s[j]=='`' && j>i+1){
                j++;
                lx.kind=LX_TOKEN; lx.text=slice(s,i,j); i=j;
                if(!lx.text||!lxvec_push(&v,lx)){ if(err)*err="out of memory"; lxvec_free(&v); return NULL; }
                continue;
            }
            // Not a valid raw identifier; fall through to emit '`' as a lone token.
        }
        // Heredoc `<<MARKER ... MARKER` (the lexer treats `<<` + identifier as a
        // heredoc; `<<`, `<<=`, `<<2` stay operators). Captured whole and verbatim.
        if (c=='<' && s[i+1]=='<' && (is_ident_start((unsigned char)s[i+2]))){
            size_t e=scan_heredoc(s,i);
            if(!e){ if(err)*err="unterminated heredoc"; lxvec_free(&v); return NULL; }
            lx.kind=LX_TOKEN; lx.text=slice(s,i,e); i=e;
            if(!lx.text||!lxvec_push(&v,lx)){ if(err)*err="out of memory"; lxvec_free(&v); return NULL; }
            continue;
        }
        if (is_ident_start((unsigned char)c)){
            size_t st=i; while(is_ident_char((unsigned char)s[i])) i++;
            lx.kind=LX_TOKEN; lx.text=slice(s,st,i);
            if(!lx.text||!lxvec_push(&v,lx)){ if(err)*err="out of memory"; lxvec_free(&v); return NULL; }
            continue;
        }
        if (isdigit((unsigned char)c) || (c=='.' && isdigit((unsigned char)s[i+1]))){
            size_t st=i;
            if (c=='0' && (s[i+1]=='x'||s[i+1]=='X')){
                i+=2; while(isalnum((unsigned char)s[i])||s[i]=='_') i++;
            } else {
                int seen_dot=0;
                while (s[i]){
                    if (isdigit((unsigned char)s[i]) || s[i]=='_'){ i++; continue; }
                    if (s[i]=='.' && !seen_dot && isdigit((unsigned char)s[i+1])){ seen_dot=1; i++; continue; }
                    if ((s[i]=='e'||s[i]=='E') &&
                        (isdigit((unsigned char)s[i+1]) ||
                         ((s[i+1]=='+'||s[i+1]=='-') && isdigit((unsigned char)s[i+2])))){
                        i++; if(s[i]=='+'||s[i]=='-') i++; continue;
                    }
                    break;
                }
                // numeric type suffix (e.g. 5i64), if any: keep it fused to the number
                while (is_ident_char((unsigned char)s[i])) i++;
            }
            lx.kind=LX_TOKEN; lx.text=slice(s,st,i);
            if(!lx.text||!lxvec_push(&v,lx)){ if(err)*err="out of memory"; lxvec_free(&v); return NULL; }
            continue;
        }
        int ol=op_len(s+i);
        if (ol>0){
            lx.kind=LX_TOKEN; lx.text=slice(s,i,i+ol); i+=ol;
            if(!lx.text||!lxvec_push(&v,lx)){ if(err)*err="out of memory"; lxvec_free(&v); return NULL; }
            continue;
        }
        // Unknown byte: preserve as a single-char token rather than drop it.
        lx.kind=LX_TOKEN; lx.text=slice(s,i,i+1); i++;
        if(!lx.text||!lxvec_push(&v,lx)){ if(err)*err="out of memory"; lxvec_free(&v); return NULL; }
    }
    *out_n=v.n; return v.v;
}

// ------------------------------------------------------------- classification
static int eq(const char* s, const char* t){ return strcmp(s,t)==0; }
static int is_open(const char* t){ return eq(t,"(")||eq(t,"[")||eq(t,"{"); }
static int is_close(const char* t){ return eq(t,")")||eq(t,"]")||eq(t,"}"); }
static int is_word_tok(const char* t){ return is_word_char((unsigned char)t[0]) || t[0]=='"' || t[0]=='`'; }

static int in_set(const char* t, const char* const* set){ for(int k=0; set[k]; k++) if(eq(t,set[k])) return 1; return 0; }

static const char* KEYWORDS[] = {
    "actor","main","func","fn","let","var","if","else","for","while","switch",
    "case","default","break","continue","return","defer","builder","match","when",
    "receive","send","spawn_actor","spawn","make","self","state","struct","union",
    "import","as","export","exports","module","message","reply","extern","null",
    "const","in","after","callback","hide","seal","except","try","catch","panic",
    "requires","ensures","true","false","and","or","not","isolate","consume", NULL
};
static const char* VALUE_KEYWORDS[] = { "true","false","null","self", NULL };
static const char* STMT_KW_BEFORE_PAREN[] = { "if","while","for","switch","match","when","return","catch","in", NULL };
static const char* CALL_KW_BEFORE_PAREN[] = { "spawn","make","isolate","consume", NULL };

static int is_keyword(const char* t){ return in_set(t, KEYWORDS); }
static int is_value_end(const char* t){
    if (is_close(t)) return 1;
    if (in_set(t, VALUE_KEYWORDS)) return 1;
    if (is_word_tok(t) && !is_keyword(t)) return 1;   // identifiers, numbers, strings
    return 0;
}

// Desired spaces (0/1) before `cur`, given the previous two emitted tokens.
static int spaces_before(const char* prev, const char* prevprev, const char* cur, int cur_had_space){
    if (eq(cur,",") || eq(cur,";")) return 0;
    if (eq(cur,".") || eq(cur,"?.")) return 0;
    if (eq(prev,".") || eq(prev,"?.")) return 0;
    if (eq(cur,":")) return 0;
    if (eq(prev,":")) return 1;
    if (eq(prev,"(") || eq(prev,"[")) return 0;
    if (eq(cur,")") || eq(cur,"]")) return 0;
    if (eq(prev,"{")) return eq(cur,"}") ? 0 : 1;
    if (eq(cur,"}")) return 1;
    if (eq(prev,",") || eq(prev,";")) return 1;
    if (eq(prev,"@")) return 0;
    // `!` (send / not / unwrap) and `?` (optional) are context-ambiguous but
    // purely cosmetic; preserve the author's spacing so we never guess wrong.
    if (eq(cur,"!") || eq(prev,"!")) return cur_had_space ? 1 : 0;
    if (eq(cur,"?") || eq(prev,"?")) return cur_had_space ? 1 : 0;
    // datastar#5: `++`/`--` bind tight to their operand. A POSTFIX increment
    // follows a value (`i++`), and a PREFIX one precedes it (`++i`); either
    // way the operator and its operand are written together, which is how
    // every hand-written Aether source in this tree spells it. Without this
    // the formatter rendered `for (i = 0; i < n; i++)` as `... n; i ++)`,
    // disagreeing with the code it exists to normalise.
    if (eq(cur,"++") || eq(cur,"--")) {
        // `i++` / `x[0]++` -- no space when it follows a value.
        if (prev && is_value_end(prev)) return 0;
        return 1;                       // `n = ++i` keeps the space before it
    }
    if (eq(prev,"++") || eq(prev,"--")) {
        // Prefix form: `++i` binds to what follows. After a postfix use the
        // next token starts something new (`i++; j`), and the `;`/`,` rules
        // above have already returned for those.
        if (prevprev == NULL || !is_value_end(prevprev)) return 0;
        return 1;
    }
    // No space after a unary +/-/~/*/& (prev token was the operator and the
    // token before it does not end a value, so it is prefix, not infix).
    if ((eq(prev,"-")||eq(prev,"+")||eq(prev,"~")||eq(prev,"*")||eq(prev,"&")) &&
        (prevprev==NULL || !is_value_end(prevprev)))
        return 0;
    if (eq(cur,"(")){
        if (in_set(prev, CALL_KW_BEFORE_PAREN)) return 0;
        if (in_set(prev, STMT_KW_BEFORE_PAREN)) return 1;
        if (is_word_tok(prev) || is_close(prev)) return 0;   // call foo( / chain )(
        return 1;
    }
    if (eq(cur,"[")){
        if (is_word_tok(prev) || is_close(prev)) return 0;   // index a[
        return 1;
    }
    if (eq(cur,"{")) return 1;                               // space before a brace
    // Ranges are written tight: 0..5, x..=y.
    if (eq(cur,"..")||eq(cur,"..=")||eq(cur,"..<")||eq(cur,"...")) return 0;
    if (eq(prev,"..")||eq(prev,"..=")||eq(prev,"..<")||eq(prev,"...")) return 0;
    return 1;   // default: one space (around operators, between words)
}

// Would emitting `a` then `b` with no space between them re-lex as a different
// token boundary (word fusion, or two operators forming a longer one)?
static int would_merge(const char* a, const char* b){
    if (!a[0] || !b[0]) return 0;
    char la=a[strlen(a)-1], fb=b[0];
    if (is_word_char((unsigned char)la) && is_word_char((unsigned char)fb)) return 1;
    char two[3]={la,fb,0};
    for (int k=0; OPS2[k]; k++) if (eq(OPS2[k],two)) return 1;
    return 0;
}

static char* layout(Lexeme* lex, size_t n){
    Buf out; if(!buf_init(&out)) return NULL;
    if (n==0){ return out.data; }   // empty / whitespace-only input -> empty output
    int depth=0;
    const char* prev=NULL, *prevprev=NULL;   // previous emitted TOKEN texts
    int line_started=0;                       // any code emitted on the current line
    int after_inline_comment=0;               // last emitted was an inline block comment

    for (size_t k=0;k<n;k++){
        Lexeme* L=&lex[k];
        int first=(k==0);
        int is_closer = (L->kind==LX_TOKEN) &&
                        (eq(L->text,"}")||eq(L->text,")")||eq(L->text,"]"));

        int emit_nl;
        if (first) emit_nl=0;
        else if (L->nl_before==0) emit_nl=0;
        else {
            emit_nl = (L->nl_before>=2) ? 2 : 1;
            if (prev && eq(prev,"{") && !(L->kind==LX_TOKEN && eq(L->text,"}"))) emit_nl=1;
            if (L->kind==LX_TOKEN && eq(L->text,"}")) emit_nl=1;
            if (prev && (eq(prev,"(")||eq(prev,"["))) emit_nl=1;
            if (L->kind==LX_TOKEN && (eq(L->text,")")||eq(L->text,"]"))) emit_nl=1;
        }

        if (emit_nl>0){
            for (int q=0;q<emit_nl;q++) buf_putc(&out,'\n');
            int ind=depth - (is_closer?1:0); if(ind<0) ind=0;
            buf_spaces(&out, ind*4);
            line_started=0; after_inline_comment=0;
        } else if (!first){
            int sp;
            if (L->kind!=LX_TOKEN){
                sp = line_started ? 1 : 0;                 // inline comment
            } else if (after_inline_comment){
                sp = 1;                                     // token after inline block comment
            } else if (prev){
                sp = spaces_before(prev, prevprev, L->text, L->space_before);
                if (sp==0 && would_merge(prev, L->text)) sp=1;
            } else sp=0;
            buf_spaces(&out, sp);
        }

        buf_puts(&out, L->text);

        if (L->kind==LX_TOKEN){
            if (is_open(L->text)) depth++;
            else if (is_close(L->text)){ depth--; if(depth<0) depth=0; }
            prevprev=prev; prev=L->text;
            after_inline_comment=0;
        } else {
            after_inline_comment = (L->kind==LX_BLOCK_COMMENT && emit_nl==0);
        }
        line_started=1;
    }
    buf_putc(&out,'\n');   // exactly one trailing newline
    return out.data;
}

char* ae_format_source(const char* src, const char** err){
    if (err) *err=NULL;
    if (!src){ if(err)*err="null source"; return NULL; }
    size_t n=0;
    Lexeme* lex=scan(src,&n,err);
    if (!lex && n==0 && err && *err) return NULL;   // scan error
    char* out = layout(lex ? lex : (Lexeme*)"", lex? n : 0);
    if (lex){ for(size_t i=0;i<n;i++) free(lex[i].text); free(lex); }
    if (!out && err && !*err) *err="out of memory";
    return out;
}

char* ae_format_source_changed(const char* src, int* changed, const char** err){
    char* out=ae_format_source(src,err);
    if (!out){ if(changed)*changed=0; return NULL; }
    if (changed) *changed = (src && strcmp(src,out)!=0) ? 1 : 0;
    return out;
}
