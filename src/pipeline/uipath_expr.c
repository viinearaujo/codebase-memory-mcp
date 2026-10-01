#include "pipeline/uipath.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int eq_name(const char *a, const char *b, int cs) {
    if (!a || !b) {
        return 0;
    }
    if (cs) {
        return strcmp(a, b) == 0;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int is_ident_start(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_ident(int c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int stoplist(const char *s) {
    static const char *stop[] = {
        "String",     "Convert", "CInt",     "CStr",      "CBool",    "CDbl",
        "CDate",      "CDec",    "CLng",     "Path",      "Nothing",  "True",
        "False",      "Not",     "And",      "Or",        "AndAlso",  "OrElse",
        "If",         "Then",    "Else",     "New",       "DirectCast", "CType",
        "GetType",    "Int32",   "Object",   "Exception", "Now",      "DateTime",
        "Environment","File",    "Directory","Regex",     "ToString", "ToLower",
        "Trim",       "Length",  "Count",    "Item",      "Add",      "Contains",
        "SelectToken","Value",   "Parse",    "Equals",   "IsNothing","Nothing",
        "StringBuilder", "JObject", "JToken", "JsonConvert", "BusinessRuleException",
        "MessageBox", "First",   "Where",    "Select",   "ToList",   "ReadAllText",
        "GetAsset",   "GetCredential", "AddQueueItem", "GetTransactionItem", NULL};
    for (int i = 0; stop[i]; i++) {
        if (strcmp(s, stop[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int type_is_config(const char *type) {
    if (!type || !type[0]) {
        return 0;
    }
    return strstr(type, "Dictionary") != NULL || strstr(type, "JObject") != NULL ||
           strstr(type, "JToken") != NULL;
}

static int name_is_config(const char *name) {
    return name && (strcmp(name, "in_Config") == 0 || strcmp(name, "io_Config") == 0 ||
                    strcmp(name, "out_Config") == 0 || strcmp(name, "Config") == 0 ||
                    strcmp(name, "cfg") == 0 || strcmp(name, "config") == 0);
}

static void push_unique(char dest[][160], int *n, int cap, const char *s) {
    if (!s || !s[0] || *n >= cap) {
        return;
    }
    for (int i = 0; i < *n; i++) {
        if (strcmp(dest[i], s) == 0) {
            return;
        }
    }
    snprintf(dest[*n], 160, "%s", s);
    (*n)++;
}

static void push80(char dest[][80], int *n, int cap, const char *s) {
    if (!s || !s[0] || *n >= cap) {
        return;
    }
    for (int i = 0; i < *n; i++) {
        if (strcmp(dest[i], s) == 0) {
            return;
        }
    }
    snprintf(dest[*n], 80, "%s", s);
    (*n)++;
}

static void push96(char dest[][96], int *n, int cap, const char *s) {
    if (!s || !s[0] || *n >= cap) {
        return;
    }
    for (int i = 0; i < *n; i++) {
        if (strcmp(dest[i], s) == 0) {
            return;
        }
    }
    snprintf(dest[*n], 96, "%s", s);
    (*n)++;
}

static const char *lookup_type(const char *name, int cs, const char *const *names,
                               const char *const *types, int n) {
    for (int i = 0; i < n; i++) {
        if (names[i] && eq_name(names[i], name, cs)) {
            return types ? types[i] : NULL;
        }
    }
    return NULL;
}

static int known_var(const char *name, int cs, const char *const *names, int n) {
    for (int i = 0; i < n; i++) {
        if (names[i] && eq_name(names[i], name, cs)) {
            return 1;
        }
    }
    return 0;
}

void uipath_expr_analyze(const char *expr, int lang_cs, const char *const *var_names,
                         const char *const *var_types, int nvars, const char *const *arg_names,
                         const char *const *arg_types, int nargs, int is_write_site,
                         uipath_expr_facts *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!expr) {
        return;
    }
    const char *p = expr;
    while (*p == ' ' || *p == '\t' || *p == '[') {
        p++;
    }
    int first_ident = 1;
    char lambda[8][64];
    int nlamb = 0;
    while (*p) {
        if (*p == '"') {
            p++;
            if (lang_cs && p[-1] == '"' && p[-2] == '@') {
                /* already inside; handled below as normal */
            }
            while (*p && *p != '"') {
                if (*p == '"' && p[1] == '"') {
                    p += 2;
                    continue;
                }
                if (lang_cs && *p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }
                p++;
            }
            if (*p == '"') {
                p++;
            }
            continue;
        }
        if (*p == '\'') {
            p++;
            while (*p && *p != '\n' && *p != '\'') {
                p++;
            }
            if (*p == '\'') {
                p++;
            }
            continue;
        }
        if (!is_ident_start((unsigned char)*p)) {
            p++;
            continue;
        }
        const char *start = p;
        while (is_ident((unsigned char)*p)) {
            p++;
        }
        char ident[96];
        size_t ilen = (size_t)(p - start);
        if (ilen >= sizeof(ident)) {
            ilen = sizeof(ident) - 1;
        }
        memcpy(ident, start, ilen);
        ident[ilen] = '\0';
        int is_lambda = 0;
        for (int i = 0; i < nlamb; i++) {
            if (strcmp(lambda[i], ident) == 0) {
                is_lambda = 1;
            }
        }
        const char *ahead = p;
        while (*ahead == ' ') {
            ahead++;
        }
        if (!lang_cs && (strcmp(ident, "Function") == 0 || strcmp(ident, "Sub") == 0) &&
            *ahead == '(') {
            const char *q = ahead + 1;
            while (*q == ' ') {
                q++;
            }
            if (is_ident_start((unsigned char)*q) && nlamb < 8) {
                size_t k = 0;
                while (is_ident((unsigned char)*q) && k + 1 < sizeof(lambda[0])) {
                    lambda[nlamb][k++] = *q++;
                }
                lambda[nlamb][k] = '\0';
                nlamb++;
            }
            continue;
        }
        if (lang_cs && *ahead == '=' && ahead[1] == '>') {
            if (nlamb < 8) {
                snprintf(lambda[nlamb], sizeof(lambda[0]), "%s", ident);
                nlamb++;
            }
            continue;
        }
        if (is_lambda || stoplist(ident)) {
            first_ident = 0;
            continue;
        }
        /* Indexer: ident("k") or ident["k"] or ident.Item("k"). */
        int indexed = 0;
        const char *q = ahead;
        if (strcmp(ident, "Item") != 0 && *q == '.') {
            const char *m = q + 1;
            if (strncmp(m, "Item", 4) == 0 && !is_ident((unsigned char)m[4])) {
                q = m + 4;
                while (*q == ' ') {
                    q++;
                }
            }
        }
        char key[160];
        key[0] = '\0';
        if (*q == '(' || *q == '[') {
            const char *r = q + 1;
            while (*r == ' ') {
                r++;
            }
            if (*r == '"') {
                r++;
                size_t k = 0;
                while (*r && *r != '"' && k + 1 < sizeof(key)) {
                    if (*r == '"' && r[1] == '"') {
                        key[k++] = '"';
                        r += 2;
                        continue;
                    }
                    key[k++] = *r++;
                }
                key[k] = '\0';
                indexed = key[0] != '\0';
            }
        }
        const char *vtype = lookup_type(ident, lang_cs, var_names, var_types, nvars);
        if (!vtype) {
            vtype = lookup_type(ident, lang_cs, arg_names, arg_types, nargs);
        }
        int config = type_is_config(vtype) || name_is_config(ident);
        if (indexed && config) {
            char use[160];
            snprintf(use, sizeof(use), "%s", key);
            /* Chained ("Section")("Key") or ["Section"]["Key"]. */
            const char *after = q + 1;
            while (*after && *after != ')' && *after != ']') {
                after++;
            }
            if (*after == ')' || *after == ']') {
                after++;
            }
            while (*after == ' ') {
                after++;
            }
            if (*after == '(' || *after == '[') {
                const char *r = after + 1;
                while (*r == ' ') {
                    r++;
                }
                if (*r == '"') {
                    r++;
                    char key2[160];
                    size_t k = 0;
                    while (*r && *r != '"' && k + 1 < sizeof(key2)) {
                        key2[k++] = *r++;
                    }
                    key2[k] = '\0';
                    if (key2[0] && (strcmp(key, "Settings") == 0 || strcmp(key, "Constants") == 0 ||
                                    strcmp(key, "Assets") == 0)) {
                        snprintf(use, sizeof(use), "%s", key2);
                    } else if (key2[0]) {
                        snprintf(use, sizeof(use), "%s.%s", key, key2);
                    }
                }
            }
            push_unique(out->config_keys, &out->nconfig, 8, use);
        } else if (strcmp(ident, "SelectToken") == 0 && indexed) {
            const char *dot = strrchr(key, '.');
            if (dot && (strncmp(key, "Settings.", 9) == 0 || strncmp(key, "Constants.", 10) == 0 ||
                        strncmp(key, "Assets.", 7) == 0)) {
                push_unique(out->config_keys, &out->nconfig, 8, dot + 1);
            } else {
                push_unique(out->config_keys, &out->nconfig, 8, key);
            }
        } else if (known_var(ident, lang_cs, var_names, nvars) ||
                   known_var(ident, lang_cs, arg_names, nargs)) {
            if (is_write_site && first_ident) {
                push80(out->writes, &out->nwrites, 8, ident);
            } else {
                push80(out->reads, &out->nreads, 12, ident);
            }
        } else if (*ahead == '(') {
            push96(out->calls, &out->ncalls, 8, ident);
        } else if (*ahead == '.' && ident[0] >= 'A' && ident[0] <= 'Z') {
            push96(out->types, &out->ntypes, 8, ident);
        }
        first_ident = 0;
    }
}
