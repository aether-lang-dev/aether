/* std.unicode — Unicode operations over the vendored utf8proc.
 * All string returns are managed AetherStrings (string_new_with_length);
 * the caller owns them. See aether_unicode.c. */
#ifndef AETHER_UNICODE_H
#define AETHER_UNICODE_H

void* aether_unicode_nfc(const char* input);
void* aether_unicode_nfd(const char* input);
void* aether_unicode_fold_accents(const char* input);
void* aether_unicode_casefold(const char* input);
int aether_unicode_grapheme_len(const char* input);
void* aether_unicode_grapheme_substring(const char* input, int start, int count);

#endif /* AETHER_UNICODE_H */
