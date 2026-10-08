#include "toml_parser.h"
#include <ctype.h>

static char* trim(char* str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static char* remove_quotes(char* str) {
    if (!str) return str;
    size_t len = strlen(str);
    if (len >= 2 && ((str[0] == '"' && str[len-1] == '"') ||
                     (str[0] == '\'' && str[len-1] == '\''))) {
        str[len-1] = '\0';
        return str + 1;
    }
    return str;
}

/* The value of a `key = value` line, without the `# comment` TOML allows
 * after any value: a quoted string's contents (a basic string's escapes
 * left as written, as this reader always has), or the bare text cut at its
 * first `#` outside quotes. A `#` inside quotes is part of the value.
 * Before, `cflags = "-O2"  # tuned` read as `"-O2"  # tuned`, and the C
 * compiler was handed `#` and `tuned` as files. */
static char* parse_value(char* v) {
    v = trim(v);
    if (*v == '"' || *v == '\'') {
        char q = *v;
        char* p = v + 1;
        while (*p && *p != q) {
            if (q == '"' && *p == '\\' && p[1]) p++;
            p++;
        }
        if (*p == q) {
            char* rest = trim(p + 1);
            if (*rest == '\0' || *rest == '#') {
                *p = '\0';
                return v + 1;
            }
        }
        return remove_quotes(v);
    }
    char q = 0;
    for (char* p = v; *p; p++) {
        if (q) {
            if (q == '"' && *p == '\\' && p[1]) p++;
            else if (*p == q) q = 0;
        } else if (*p == '"' || *p == '\'') {
            q = *p;
        } else if (*p == '#') {
            *p = '\0';
            break;
        }
    }
    return trim(v);
}

/* Reads one line of any length into *buf, grown as needed. 1 for a line,
 * 0 at the end of the file, -1 when out of memory. A fixed 512-byte
 * buffer used to split a longer line: its value was cut, and the rest was
 * read as a line of its own (#2535). */
static int read_line(FILE* f, char** buf, size_t* cap) {
    size_t len = 0;
    for (;;) {
        if (len + 1 >= *cap) {
            size_t ncap = *cap ? *cap * 2 : 512;
            char* nb = realloc(*buf, ncap);
            if (!nb) return -1;
            *buf = nb;
            *cap = ncap;
        }
        if (!fgets(*buf + len, (int)(*cap - len), f)) return len > 0;
        len += strlen(*buf + len);
        if (len > 0 && (*buf)[len - 1] == '\n') return 1;
        if (feof(f)) return 1;
    }
}

/* NULL when the file cannot be opened, and when memory runs out partway:
 * a document missing its later lines would read as one without them. */
TomlDocument* toml_parse_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return NULL;

    TomlDocument* doc = calloc(1, sizeof(TomlDocument));
    if (!doc) { fclose(f); return NULL; }
    int section_capacity = 16;
    doc->sections = calloc(section_capacity, sizeof(TomlSection));
    if (!doc->sections) { fclose(f); free(doc); return NULL; }
    doc->section_count = 0;

    char* line = NULL;
    size_t line_cap = 0;
    TomlSection* current_section = NULL;
    int entry_capacity = 0;  // per-section capacity
    int oom = 0;

    int got;
    while ((got = read_line(f, &line, &line_cap)) > 0) {
        char* trimmed = trim(line);

        // Skip empty lines and comments
        if (trimmed[0] == '\0' || trimmed[0] == '#') continue;

        // Section header [section]
        if (trimmed[0] == '[') {
            char* end = strchr(trimmed, ']');
            if (end) {
                *end = '\0';
                char* section_name = trim(trimmed + 1);

                // Grow sections array if needed
                if (doc->section_count >= section_capacity) {
                    int new_cap = section_capacity * 2;
                    TomlSection* new_secs = realloc(doc->sections, (size_t)new_cap * sizeof(TomlSection));
                    if (!new_secs) { oom = 1; break; }
                    doc->sections = new_secs;
                    section_capacity = new_cap;
                }

                // Create new section
                current_section = &doc->sections[doc->section_count++];
                entry_capacity = 32;
                current_section->name = strdup(section_name);
                current_section->entries = calloc(entry_capacity, sizeof(TomlKeyValue));
                current_section->entry_count = 0;
                if (!current_section->name || !current_section->entries) { oom = 1; break; }
            }
            continue;
        }

        // Key = value
        char* eq = strchr(trimmed, '=');
        if (eq && current_section) {
            *eq = '\0';
            char* key = trim(trimmed);
            char* value = parse_value(eq + 1);

            // Grow entries array if needed
            if (current_section->entry_count >= entry_capacity) {
                int new_cap = entry_capacity * 2;
                TomlKeyValue* new_entries = realloc(current_section->entries, (size_t)new_cap * sizeof(TomlKeyValue));
                if (!new_entries) { oom = 1; break; }
                current_section->entries = new_entries;
                entry_capacity = new_cap;
            }

            TomlKeyValue* entry = &current_section->entries[current_section->entry_count++];
            entry->key = strdup(key);
            entry->value = strdup(value);
            if (!entry->key || !entry->value) { oom = 1; break; }
        }
    }

    free(line);
    fclose(f);
    if (oom || got < 0) {
        toml_free_document(doc);
        return NULL;
    }
    return doc;
}

const char* toml_get_value(TomlDocument* doc, const char* section, const char* key) {
    if (!doc) return NULL;

    for (int i = 0; i < doc->section_count; i++) {
        if (doc->sections[i].name && strcmp(doc->sections[i].name, section) == 0) {
            for (int j = 0; j < doc->sections[i].entry_count; j++) {
                if (doc->sections[i].entries[j].key &&
                    strcmp(doc->sections[i].entries[j].key, key) == 0) {
                    return doc->sections[i].entries[j].value;
                }
            }
        }
    }
    return NULL;
}

TomlKeyValue* toml_get_section_entries(TomlDocument* doc, const char* section, int* count) {
    if (!doc || !count) return NULL;
    *count = 0;

    for (int i = 0; i < doc->section_count; i++) {
        if (doc->sections[i].name && strcmp(doc->sections[i].name, section) == 0) {
            *count = doc->sections[i].entry_count;
            return doc->sections[i].entries;
        }
    }
    return NULL;
}

void toml_free_document(TomlDocument* doc) {
    if (!doc) return;

    for (int i = 0; i < doc->section_count; i++) {
        free(doc->sections[i].name);
        for (int j = 0; j < doc->sections[i].entry_count; j++) {
            free(doc->sections[i].entries[j].key);
            free(doc->sections[i].entries[j].value);
        }
        free(doc->sections[i].entries);
    }
    free(doc->sections);
    free(doc);
}
