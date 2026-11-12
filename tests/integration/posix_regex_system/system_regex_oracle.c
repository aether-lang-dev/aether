/*
 * The host's own regcomp/regexec over std/posix/regex/tests/conformance.txt,
 * one result per case in that file's expected-column notation. The other
 * half, posix_regex_cases.ae, prints the same for std.posix.regex, and
 * test_posix_regex_system.sh compares them.
 *
 * Runs in a UTF-8 locale (the first of C.UTF-8, en_US.UTF-8 the host has),
 * since std.posix.regex is UTF-8 only.
 */
#include <locale.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void unescape(const char *in, char *out) {
    if (strcmp(in, "\"\"") == 0) { *out = '\0'; return; }
    while (*in) {
        if (in[0] == '\\' && in[1] == 'n') { *out++ = '\n'; in += 2; }
        else if (in[0] == '\\' && in[1] == 't') { *out++ = '\t'; in += 2; }
        else if (in[0] == '\\' && in[1] == '\\') { *out++ = '\\'; in += 2; }
        else *out++ = *in++;
    }
    *out = '\0';
}

int main(int argc, char **argv) {
    const char *locales[] = { "C.UTF-8", "en_US.UTF-8", "C.utf8", NULL };
    int i;
    for (i = 0; locales[i]; i++)
        if (setlocale(LC_ALL, locales[i]) != NULL) break;
    if (!locales[i]) { fprintf(stderr, "no UTF-8 locale\n"); return 2; }
    FILE *f = fopen(argc > 1 ? argv[1] : "std/posix/regex/tests/conformance.txt", "r");
    if (!f) { perror("conformance.txt"); return 2; }
    char line[4096];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;
        char *flags = strtok(line, "\t");
        char *pat = strtok(NULL, "\t");
        char *subj = strtok(NULL, "\t");
        if (!flags || !pat || !subj) { fprintf(stderr, "line %d: bad case\n", lineno); return 2; }
        char p[4096], s[4096];
        unescape(pat, p);
        unescape(subj, s);
        int cf = strchr(flags, 'E') ? REG_EXTENDED : 0;
        if (strchr(flags, 'i')) cf |= REG_ICASE;
        if (strchr(flags, 'n')) cf |= REG_NEWLINE;
        int ef = 0;
        if (strchr(flags, '^')) ef |= REG_NOTBOL;
        if (strchr(flags, '$')) ef |= REG_NOTEOL;
        regex_t re;
        printf("%d\t", lineno);
        if (regcomp(&re, p, cf) != 0) { printf("ERROR\n"); continue; }
        regmatch_t m[10];
        size_t nm = re.re_nsub + 1 < 10 ? re.re_nsub + 1 : 10;
        int rc = regexec(&re, s, nm, m, ef);
        if (rc == REG_NOMATCH) printf("NOMATCH\n");
        else if (rc != 0) printf("EXEC %d\n", rc);
        else {
            for (size_t k = 0; k < nm; k++) {
                if (m[k].rm_so == -1) printf("(?,?)");
                else printf("(%ld,%ld)", (long)m[k].rm_so, (long)m[k].rm_eo);
            }
            printf("\n");
        }
        regfree(&re);
    }
    fclose(f);
    return 0;
}
