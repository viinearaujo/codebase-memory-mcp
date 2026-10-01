/*
 * pass_uipath.c — CALLS edges for UiPath coded-workflow and XAML invokes.
 *
 * Gated on a project.json that contains expressionLanguage. Other repos are
 * left unchanged. .local/ is not read and its nodes are dropped: that tree
 * holds generated runners and selectors.
 *
 * workflows member names follow Studio, not a guess:
 *   - Docs: "If workflows with the same name exist in different folders, the
 *     folder structure prefixes one to avoid confusion."
 *     https://docs.uipath.com/studio/standalone/latest/user-guide/using-the-workflows-object
 *   - Generated WorkflowRunnerService methods (signatures only; nothing from
 *     those files is stored in the graph) use the sanitized file stem when
 *     that stem is unique, including files that live in a folder:
 *       Framework\InitAllSettings.xaml              -> InitAllSettings
 *       Workflows\Main Workflows\Prepare Period Folder.xaml
 *                                                   -> Prepare_Period_Folder
 *       Workflows\File Workflows\Json - Serialize Results.xaml
 *                                                   -> Json___Serialize_Results
 *       rentaLocales\StartRentaLocales.xaml        -> StartRentaLocales
 *     https://github.com/it-at-m/uipath-rpa_dtj/blob/13b1dce9f4e3cf473fa3e6486cbb1938f3fb78e4/Prod/rpa002_DTJ_Dispatcher/.local/.codedworkflows/WorkflowRunnerService.cs
 *     https://github.com/andresporras3423/scrapingRealState/blob/4465cada682ebf715101484733521832d68949eb/.local/.codedworkflows/WorkflowRunnerService.cs
 *   - Sanitization, measured on those runners: every character outside
 *     [A-Za-z0-9_] becomes '_', and underscores are not collapsed. A leading
 *     digit is prefixed with '_' so the result is a C# identifier.
 *   - On a shared sanitized stem the member is that same sanitization of the
 *     project-relative path without its extension, so folder separators become
 *     underscores (Invoices/Parse.cs -> Invoices_Parse). A root file keeps
 *     the bare stem because it has no folder to prefix. No public runner in
 *     the sample contained a duplicate stem; the prefix shape is the doc's
 *     rule applied with the identifier alphabet the runners already use.
 *
 * trace_path follows CALLS. A Function node is a callable endpoint
 * (pick_resolved_node ranks Function/Method above File/Module), so each .xaml
 * file gets a synthetic Function whose qualified name and file path are the
 * project-relative path. get_code_snippet opens that path.
 */
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/hash_table.h"
#include "foundation/log.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct uip_entry {
    char *path;
    char *member;
    char *target_qn;
    int64_t target_id;
    bool xaml;
} uip_entry;

struct cbm_uipath_index {
    uip_entry *entries;
    int count;
    int cap;
    CBMHashTable *by_path;
    CBMHashTable *by_member;
    CBMHashTable *coded_methods;
    char **method_qns;
    int method_count;
    int method_cap;
};

static char *uip_dup(const char *text) {
    size_t n;
    char *copy;
    if (!text) {
        return NULL;
    }
    n = strlen(text);
    copy = (char *)malloc(n + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, text, n + 1);
    return copy;
}

void cbm_uipath_index_free(cbm_pipeline_ctx_t *ctx) {
    struct cbm_uipath_index *index;
    int i;
    if (!ctx || !ctx->uipath_index) {
        return;
    }
    index = ctx->uipath_index;
    ctx->uipath_index = NULL;
    cbm_ht_free(index->by_path);
    cbm_ht_free(index->by_member);
    cbm_ht_free(index->coded_methods);
    for (i = 0; i < index->count; i++) {
        free(index->entries[i].path);
        free(index->entries[i].member);
        free(index->entries[i].target_qn);
    }
    free(index->entries);
    for (i = 0; i < index->method_count; i++) {
        free(index->method_qns[i]);
    }
    free(index->method_qns);
    free(index);
}

static bool uip_norm_rel(char *dst, size_t dst_n, const char *rel) {
    size_t w = 0;
    if (!dst || dst_n < 2 || !rel) {
        return false;
    }
    for (const char *p = rel; *p && w + 1 < dst_n; p++) {
        char c = (*p == '\\') ? '/' : *p;
        if (c == '/' && w > 0 && dst[w - 1] == '/') {
            continue;
        }
        dst[w++] = c;
    }
    dst[w] = '\0';
    if (w >= 2 && dst[0] == '.' && dst[1] == '/') {
        memmove(dst, dst + 2, w - 1);
    }
    return dst[0] != '\0';
}

static bool uip_is_local_cache(const char *rel) {
    char norm[CBM_SZ_1K];
    size_t n;
    if (!uip_norm_rel(norm, sizeof(norm), rel)) {
        return false;
    }
    if (strcmp(norm, ".local") == 0 || strncmp(norm, ".local/", 7) == 0) {
        return true;
    }
    if (strstr(norm, "/.local/") != NULL) {
        return true;
    }
    n = strlen(norm);
    return n >= 7 && strcmp(norm + n - 7, "/.local") == 0;
}

static const char *uip_basename(const char *rel) {
    const char *base = rel ? rel : "";
    for (const char *p = base; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

static bool uip_ext_is(const char *path, const char *ext) {
    size_t n;
    size_t e;
    if (!path || !ext) {
        return false;
    }
    n = strlen(path);
    e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        char a = path[n - e + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool uip_file_has_key(const char *path, const char *key) {
    FILE *file;
    char buf[4096 + 64];
    size_t carry = 0;
    size_t seen = 0;
    size_t key_len;
    const size_t cap = 2u * 1024u * 1024u;
    if (!path || !key || !key[0]) {
        return false;
    }
    key_len = strlen(key);
    file = cbm_fopen(path, "rb");
    if (!file) {
        return false;
    }
    for (;;) {
        size_t n = fread(buf + carry, 1, 4096, file);
        size_t total = carry + n;
        buf[total] = '\0';
        if (strstr(buf, key) != NULL) {
            (void)fclose(file);
            return true;
        }
        seen += n;
        if (n == 0 || seen >= cap) {
            break;
        }
        if (total > key_len) {
            memmove(buf, buf + total - key_len, key_len);
            carry = key_len;
        } else {
            carry = total;
        }
    }
    (void)fclose(file);
    return false;
}

static bool uip_project_gated(const cbm_file_info_t *files, int file_count) {
    for (int i = 0; i < file_count; i++) {
        const char *rel = files[i].rel_path;
        if (!rel || uip_is_local_cache(rel)) {
            continue;
        }
        if (strcmp(uip_basename(rel), "project.json") != 0) {
            continue;
        }
        if (uip_file_has_key(files[i].path, "expressionLanguage")) {
            return true;
        }
    }
    return false;
}

static bool uip_sanitize_ident(char *dst, size_t dst_n, const char *src) {
    size_t w = 0;
    if (!dst || dst_n < 3 || !src || !src[0]) {
        return false;
    }
    if (src[0] >= '0' && src[0] <= '9') {
        dst[w++] = '_';
    }
    for (const char *p = src; *p && w + 1 < dst_n; p++) {
        char c = *p;
        int ok =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        dst[w++] = ok ? c : '_';
    }
    dst[w] = '\0';
    return w > 0;
}

static void uip_stem(char *dst, size_t dst_n, const char *norm) {
    const char *base = uip_basename(norm);
    char *dot;
    snprintf(dst, dst_n, "%s", base);
    dot = strrchr(dst, '.');
    if (dot && (uip_ext_is(dst, ".cs") || uip_ext_is(dst, ".xaml"))) {
        *dot = '\0';
    }
}

static void uip_path_without_ext(char *dst, size_t dst_n, const char *norm) {
    char *dot;
    char *slash;
    snprintf(dst, dst_n, "%s", norm);
    dot = strrchr(dst, '.');
    slash = strrchr(dst, '/');
    if (dot && (!slash || dot > slash)) {
        *dot = '\0';
    }
}

static bool uip_base_is_coded_workflow(const char *base) {
    const char *leaf;
    if (!base || !base[0]) {
        return false;
    }
    leaf = strrchr(base, '.');
    leaf = (leaf && leaf[1]) ? leaf + 1 : base;
    return strcmp(leaf, "CodedWorkflow") == 0;
}

static bool uip_decorator_is_entry(const char *text) {
    char name[64];
    size_t i = 0;
    const char *p;
    if (!text) {
        return false;
    }
    p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '[') {
        p++;
    }
    while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '_') &&
           i + 1 < sizeof(name)) {
        name[i++] = *p++;
    }
    name[i] = '\0';
    if (i > 9 && strcmp(name + i - 9, "Attribute") == 0) {
        name[i - 9] = '\0';
    }
    return strcmp(name, "Workflow") == 0 || strcmp(name, "TestCase") == 0;
}

static bool uip_def_is_entry(const CBMDefinition *def) {
    if (!def || !def->decorators) {
        return false;
    }
    for (int i = 0; def->decorators[i]; i++) {
        if (uip_decorator_is_entry(def->decorators[i])) {
            return true;
        }
    }
    return false;
}

static bool uip_push_method(struct cbm_uipath_index *index, const char *qn) {
    char *copy;
    if (!index || !qn || !qn[0]) {
        return false;
    }
    if (cbm_ht_get(index->coded_methods, qn)) {
        return true;
    }
    copy = uip_dup(qn);
    if (!copy) {
        return false;
    }
    if (index->method_count == index->method_cap) {
        int cap = index->method_cap ? index->method_cap * 2 : 16;
        char **grown = (char **)realloc(index->method_qns, (size_t)cap * sizeof(char *));
        if (!grown) {
            free(copy);
            return false;
        }
        index->method_qns = grown;
        index->method_cap = cap;
    }
    index->method_qns[index->method_count++] = copy;
    cbm_ht_set(index->coded_methods, copy, (void *)(uintptr_t)1);
    return true;
}

static bool uip_add_entry(struct cbm_uipath_index *index, const char *path, const char *qn,
                          int64_t id, bool xaml) {
    uip_entry *entry;
    if (!index || !path || !qn || id <= 0) {
        return false;
    }
    if (index->count == index->cap) {
        int cap = index->cap ? index->cap * 2 : 16;
        uip_entry *grown = (uip_entry *)realloc(index->entries, (size_t)cap * sizeof(uip_entry));
        if (!grown) {
            return false;
        }
        index->entries = grown;
        index->cap = cap;
    }
    entry = &index->entries[index->count];
    memset(entry, 0, sizeof(*entry));
    entry->path = uip_dup(path);
    entry->target_qn = uip_dup(qn);
    entry->target_id = id;
    entry->xaml = xaml;
    if (!entry->path || !entry->target_qn) {
        free(entry->path);
        free(entry->target_qn);
        return false;
    }
    index->count++;
    return true;
}

static int uip_count_lines(const char *text, size_t n) {
    int lines = 1;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\n') {
            lines++;
        }
    }
    if (n > 0 && text[n - 1] == '\n' && lines > 1) {
        lines--;
    }
    return lines < 1 ? 1 : lines;
}

static char *uip_read_file(const char *path, size_t *out_len) {
    FILE *file;
    long size;
    char *buf;
    size_t nread;
    if (out_len) {
        *out_len = 0;
    }
    file = cbm_fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || size > 8 * 1024 * 1024) {
        (void)fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        (void)fclose(file);
        return NULL;
    }
    nread = fread(buf, 1, (size_t)size, file);
    (void)fclose(file);
    buf[nread] = '\0';
    if (out_len) {
        *out_len = nread;
    }
    return buf;
}

static bool uip_index_xaml(cbm_pipeline_ctx_t *ctx, struct cbm_uipath_index *index, const char *rel,
                           const char *abs_path) {
    char *text;
    size_t len = 0;
    int lines;
    int end_line;
    char stem[CBM_SZ_256];
    int64_t id;
    char *file_qn;
    const cbm_gbuf_node_t *file_node;
    text = uip_read_file(abs_path, &len);
    lines = text ? uip_count_lines(text, len) : 1;
    free(text);
    end_line = lines < 2 ? 2 : lines;
    uip_stem(stem, sizeof(stem), rel);
    /* Qualified name is the project-relative path so CALLS and
     * get_code_snippet can land on this file. Name is the file stem. */
    id = cbm_gbuf_upsert_node(ctx->gbuf, "Function", stem[0] ? stem : rel, rel, rel, 1, end_line,
                              "{}");
    if (id <= 0) {
        return false;
    }
    file_qn = cbm_pipeline_fqn_compute(ctx->project_name, rel, "__file__");
    file_node = file_qn ? cbm_gbuf_find_by_qn(ctx->gbuf, file_qn) : NULL;
    if (file_node) {
        cbm_gbuf_insert_edge(ctx->gbuf, file_node->id, id, "DEFINES", "{}");
    }
    free(file_qn);
    return uip_add_entry(index, rel, rel, id, true);
}

static bool uip_class_is_coded(const CBMDefinition *def) {
    if (!def || !def->label || strcmp(def->label, "Class") != 0 || !def->base_classes) {
        return false;
    }
    for (int i = 0; def->base_classes[i]; i++) {
        if (uip_base_is_coded_workflow(def->base_classes[i])) {
            return true;
        }
    }
    return false;
}

static bool uip_parent_is_coded(const char **classes, int class_count, const char *parent) {
    if (!parent) {
        return false;
    }
    for (int i = 0; i < class_count; i++) {
        if (classes[i] && strcmp(classes[i], parent) == 0) {
            return true;
        }
    }
    return false;
}

static bool uip_index_cs(cbm_pipeline_ctx_t *ctx, struct cbm_uipath_index *index,
                         const CBMFileResult *result, const char *rel) {
    const char *coded[32];
    int coded_count = 0;
    const CBMDefinition *entry = NULL;
    int entry_count = 0;
    const cbm_gbuf_node_t *node;
    if (!result) {
        return true;
    }
    for (int i = 0; i < result->defs.count && coded_count < 32; i++) {
        const CBMDefinition *def = &result->defs.items[i];
        if (uip_class_is_coded(def) && def->qualified_name) {
            coded[coded_count++] = def->qualified_name;
        }
    }
    for (int i = 0; i < result->defs.count; i++) {
        const CBMDefinition *def = &result->defs.items[i];
        if (!def->label || strcmp(def->label, "Method") != 0 || !def->qualified_name) {
            continue;
        }
        if (!uip_parent_is_coded(coded, coded_count, def->parent_class)) {
            continue;
        }
        if (!uip_push_method(index, def->qualified_name)) {
            return false;
        }
        if (uip_def_is_entry(def)) {
            entry_count++;
            if (entry_count == 1) {
                entry = def;
            }
        }
    }
    if (entry_count != 1 || !entry) {
        return true;
    }
    node = cbm_gbuf_find_by_qn(ctx->gbuf, entry->qualified_name);
    if (!node) {
        return true;
    }
    return uip_add_entry(index, rel, entry->qualified_name, node->id, false);
}

static void uip_assign_members(struct cbm_uipath_index *index) {
    bool *drop;
    for (int i = 0; i < index->count; i++) {
        char stem[CBM_SZ_256];
        char raw[CBM_SZ_512];
        char member[CBM_SZ_512];
        int stem_count = 0;
        uip_stem(stem, sizeof(stem), index->entries[i].path);
        for (int j = 0; j < index->count; j++) {
            char other[CBM_SZ_256];
            uip_stem(other, sizeof(other), index->entries[j].path);
            if (strcmp(stem, other) == 0) {
                stem_count++;
            }
        }
        if (stem_count == 1) {
            snprintf(raw, sizeof(raw), "%s", stem);
        } else {
            uip_path_without_ext(raw, sizeof(raw), index->entries[i].path);
        }
        if (!uip_sanitize_ident(member, sizeof(member), raw)) {
            continue;
        }
        index->entries[i].member = uip_dup(member);
    }
    drop = (bool *)calloc((size_t)index->count, sizeof(bool));
    if (!drop) {
        return;
    }
    for (int i = 0; i < index->count; i++) {
        if (!index->entries[i].member) {
            continue;
        }
        for (int j = i + 1; j < index->count; j++) {
            if (!index->entries[j].member) {
                continue;
            }
            if (strcmp(index->entries[i].member, index->entries[j].member) == 0) {
                drop[i] = true;
                drop[j] = true;
            }
        }
    }
    for (int i = 0; i < index->count; i++) {
        if (!drop[i]) {
            continue;
        }
        free(index->entries[i].member);
        index->entries[i].member = NULL;
    }
    free(drop);
}

static bool uip_bind_lookups(struct cbm_uipath_index *index) {
    index->by_path = cbm_ht_create(64);
    index->by_member = cbm_ht_create(64);
    if (!index->by_path || !index->by_member) {
        return false;
    }
    for (int i = 0; i < index->count; i++) {
        if (!cbm_ht_get(index->by_path, index->entries[i].path)) {
            cbm_ht_set(index->by_path, index->entries[i].path, &index->entries[i]);
        }
        if (index->entries[i].member && !cbm_ht_get(index->by_member, index->entries[i].member)) {
            cbm_ht_set(index->by_member, index->entries[i].member, &index->entries[i]);
        }
    }
    return true;
}

static const char *uip_workflows_member(const char *callee) {
    const char *dot;
    const char *member;
    size_t recv_len;
    if (!callee) {
        return NULL;
    }
    dot = strrchr(callee, '.');
    if (!dot || dot == callee || !dot[1]) {
        return NULL;
    }
    member = dot + 1;
    for (const char *p = member; *p; p++) {
        int ok =
            (p == member && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')) ||
            (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '_';
        if (!ok) {
            return NULL;
        }
    }
    recv_len = (size_t)(dot - callee);
    if (recv_len == 9 && strncmp(callee, "workflows", 9) == 0) {
        return member;
    }
    if (recv_len > 10 && callee[recv_len - 10] == '.' &&
        strncmp(callee + recv_len - 9, "workflows", 9) == 0) {
        return member;
    }
    return NULL;
}

static bool uip_is_runworkflow(const char *callee) {
    const char *dot;
    const char *name;
    size_t recv_len;
    const char *marker = "WorkflowInvocationService";
    size_t marker_len;
    if (!callee || !callee[0]) {
        return false;
    }
    dot = strrchr(callee, '.');
    name = dot ? dot + 1 : callee;
    if (strcmp(name, "RunWorkflow") != 0) {
        return false;
    }
    if (!dot) {
        return true;
    }
    recv_len = (size_t)(dot - callee);
    marker_len = strlen(marker);
    if (recv_len < marker_len) {
        return false;
    }
    if (strncmp(callee + recv_len - marker_len, marker, marker_len) != 0) {
        return false;
    }
    return recv_len == marker_len || callee[recv_len - marker_len - 1] == '.';
}

static bool uip_workflow_ext(const char *path) {
    return uip_ext_is(path, ".cs") || uip_ext_is(path, ".xaml");
}

static bool uip_normalize_literal_path(char *dst, size_t dst_n, const char *raw) {
    char tmp[CBM_SZ_1K];
    size_t w = 0;
    const char *p;
    if (!raw || !dst || dst_n < 2) {
        return false;
    }
    p = raw;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '@') {
        p++;
    }
    if (*p == '"' || *p == '\'') {
        char quote = *p++;
        const char *end = strrchr(p, quote);
        if (!end || end == p) {
            return false;
        }
        if ((size_t)(end - p) >= sizeof(tmp)) {
            return false;
        }
        memcpy(tmp, p, (size_t)(end - p));
        tmp[end - p] = '\0';
        p = tmp;
    }
    for (; *p && w + 1 < dst_n; p++) {
        char c = (*p == '\\') ? '/' : *p;
        if (c == '/' && w > 0 && dst[w - 1] == '/') {
            continue;
        }
        dst[w++] = c;
    }
    dst[w] = '\0';
    if (w == 0 || dst[0] == '/' || strchr(dst, ':') != NULL) {
        return false;
    }
    if (strcmp(dst, "..") == 0 || strncmp(dst, "../", 3) == 0 || strstr(dst, "/../") != NULL) {
        return false;
    }
    if (uip_is_local_cache(dst) || !uip_workflow_ext(dst)) {
        return false;
    }
    return true;
}

static bool uip_expr_is_string_literal(const char *expr) {
    if (!expr) {
        return true;
    }
    return strchr(expr, '"') != NULL || strchr(expr, '\'') != NULL;
}

static bool uip_call_literal_path(const CBMCall *call, char *dst, size_t dst_n) {
    if (!call || !call->first_string_arg || !call->first_string_arg[0]) {
        return false;
    }
    if (call->arg_count > 0 && call->args && call->args[0].expr &&
        !uip_expr_is_string_literal(call->args[0].expr)) {
        return false;
    }
    return uip_normalize_literal_path(dst, dst_n, call->first_string_arg);
}

bool cbm_uipath_suppress_generic_call(const cbm_pipeline_ctx_t *ctx, const CBMCall *call) {
    char path[CBM_SZ_1K];
    if (!ctx || !ctx->uipath_index || !ctx->uipath_index->coded_methods || !call ||
        !call->callee_name || !call->enclosing_func_qn) {
        return false;
    }
    if (!cbm_ht_get(ctx->uipath_index->coded_methods, call->enclosing_func_qn)) {
        return false;
    }
    if (uip_workflows_member(call->callee_name) != NULL) {
        return true;
    }
    return uip_is_runworkflow(call->callee_name) && uip_call_literal_path(call, path, sizeof(path));
}

static bool uip_json_escape(char *dst, size_t dst_n, const char *src) {
    size_t w = 0;
    if (!dst || dst_n < 2) {
        return false;
    }
    for (const char *p = src ? src : ""; *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (w + 2 >= dst_n) {
                return false;
            }
            dst[w++] = '\\';
        }
        if ((unsigned char)*p < 0x20) {
            return false;
        }
        if (w + 1 >= dst_n) {
            return false;
        }
        dst[w++] = *p;
    }
    dst[w] = '\0';
    return true;
}

static void uip_emit(cbm_gbuf_t *gbuf, int64_t source, int64_t target, const char *path, int line) {
    char escaped[CBM_SZ_512];
    char props[CBM_SZ_1K];
    if (!gbuf || source <= 0 || target <= 0 || source == target || line < 1) {
        return;
    }
    if (!uip_json_escape(escaped, sizeof(escaped), path ? path : "")) {
        return;
    }
    snprintf(props, sizeof(props), "{\"strategy\":\"uipath_invoke\",\"path\":\"%s\",\"line\":%d}",
             escaped, line);
    cbm_gbuf_insert_edge(gbuf, source, target, "CALLS", props);
}

static const uip_entry *uip_find_path(const struct cbm_uipath_index *index, const char *path) {
    if (!index || !index->by_path || !path) {
        return NULL;
    }
    return (const uip_entry *)cbm_ht_get(index->by_path, path);
}

static const uip_entry *uip_find_member(const struct cbm_uipath_index *index, const char *member) {
    if (!index || !index->by_member || !member) {
        return NULL;
    }
    return (const uip_entry *)cbm_ht_get(index->by_member, member);
}

static void uip_emit_cs_calls(cbm_pipeline_ctx_t *ctx, const CBMFileResult *result) {
    if (!result || !ctx->uipath_index) {
        return;
    }
    for (int i = 0; i < result->calls.count; i++) {
        const CBMCall *call = &result->calls.items[i];
        const cbm_gbuf_node_t *source;
        const char *member;
        char path[CBM_SZ_1K];
        const uip_entry *target = NULL;
        if (!call->callee_name || !call->enclosing_func_qn) {
            continue;
        }
        if (!cbm_ht_get(ctx->uipath_index->coded_methods, call->enclosing_func_qn)) {
            continue;
        }
        member = uip_workflows_member(call->callee_name);
        if (member) {
            target = uip_find_member(ctx->uipath_index, member);
        } else if (uip_is_runworkflow(call->callee_name) &&
                   uip_call_literal_path(call, path, sizeof(path))) {
            target = uip_find_path(ctx->uipath_index, path);
        }
        if (!target) {
            continue;
        }
        source = cbm_gbuf_find_by_qn(ctx->gbuf, call->enclosing_func_qn);
        if (!source) {
            continue;
        }
        uip_emit(ctx->gbuf, source->id, target->target_id, target->path,
                 call->start_line > 0 ? call->start_line : 1);
    }
}

static void uip_decode_basic_entities(char *text) {
    char *r = text;
    char *w = text;
    if (!text) {
        return;
    }
    while (*r) {
        if (r[0] == '&') {
            if (strncmp(r, "&amp;", 5) == 0) {
                *w++ = '&';
                r += 5;
                continue;
            }
            if (strncmp(r, "&quot;", 6) == 0) {
                *w++ = '"';
                r += 6;
                continue;
            }
            if (strncmp(r, "&apos;", 6) == 0) {
                *w++ = '\'';
                r += 6;
                continue;
            }
            if (strncmp(r, "&lt;", 4) == 0) {
                *w++ = '<';
                r += 4;
                continue;
            }
            if (strncmp(r, "&gt;", 4) == 0) {
                *w++ = '>';
                r += 4;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static bool uip_name_eq(const char *start, const char *end, const char *expected) {
    size_t n = (size_t)(end - start);
    return n == strlen(expected) && strncmp(start, expected, n) == 0;
}

static void uip_scan_invokes(cbm_pipeline_ctx_t *ctx, const char *src, const char *file_rel);

static void uip_emit_xaml(cbm_pipeline_ctx_t *ctx, const char *rel, const char *abs_path) {
    char *text;
    size_t len = 0;
    text = uip_read_file(abs_path, &len);
    if (!text) {
        return;
    }
    uip_scan_invokes(ctx, text, rel);
    free(text);
    (void)len;
}

static void uip_scan_invokes(cbm_pipeline_ctx_t *ctx, const char *src, const char *file_rel) {
    const uip_entry *source_entry;
    const char *p;
    int line = 1;
    if (!src || !ctx->uipath_index) {
        return;
    }
    source_entry = uip_find_path(ctx->uipath_index, file_rel);
    if (!source_entry) {
        return;
    }
    p = src;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    while (*p) {
        const char *name;
        const char *name_end;
        const char *local;
        bool invoke;
        int tag_line;
        char file_name[CBM_SZ_1K];
        bool have_file;
        if (*p == '\n') {
            line++;
            p++;
            continue;
        }
        if (p[0] == '<' && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            p += 4;
            while (*p && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) {
                if (*p == '\n') {
                    line++;
                }
                p++;
            }
            if (*p) {
                p += 3;
            }
            continue;
        }
        if (p[0] == '<' && p[1] == '!' && p[2] == '[') {
            p += 3;
            while (*p && !(p[0] == ']' && p[1] == ']' && p[2] == '>')) {
                if (*p == '\n') {
                    line++;
                }
                p++;
            }
            if (*p) {
                p += 3;
            }
            continue;
        }
        if (*p != '<' || p[1] == '/' || p[1] == '!' || p[1] == '?') {
            p++;
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            if (*p == '\n') {
                line++;
            }
            p++;
        }
        name = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '/' &&
               *p != '>') {
            p++;
        }
        name_end = p;
        local = name;
        for (const char *c = name; c < name_end; c++) {
            if (*c == ':') {
                local = c + 1;
            }
        }
        invoke = uip_name_eq(local, name_end, "InvokeWorkflowFile");
        tag_line = line;
        have_file = false;
        file_name[0] = '\0';
        while (*p && *p != '>') {
            if (*p == '\n') {
                line++;
                p++;
                continue;
            }
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote) {
                    if (*p == '\n') {
                        line++;
                    }
                    p++;
                }
                if (*p == quote) {
                    p++;
                }
                continue;
            }
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_') {
                const char *an = p;
                const char *an_end;
                const char *alocal;
                p++;
                while (*p && *p != '=' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' &&
                       *p != '/' && *p != '>') {
                    p++;
                }
                an_end = p;
                alocal = an;
                for (const char *c = an; c < an_end; c++) {
                    if (*c == ':') {
                        alocal = c + 1;
                    }
                }
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
                    if (*p == '\n') {
                        line++;
                    }
                    p++;
                }
                if (*p == '=') {
                    p++;
                    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
                        if (*p == '\n') {
                            line++;
                        }
                        p++;
                    }
                    if (*p == '"' || *p == '\'') {
                        char q = *p++;
                        const char *vs = p;
                        while (*p && *p != q) {
                            if (*p == '\n') {
                                line++;
                            }
                            p++;
                        }
                        if (invoke && uip_name_eq(alocal, an_end, "WorkflowFileName")) {
                            size_t vn = (size_t)(p - vs);
                            if (vn >= sizeof(file_name)) {
                                vn = sizeof(file_name) - 1;
                            }
                            memcpy(file_name, vs, vn);
                            file_name[vn] = '\0';
                            uip_decode_basic_entities(file_name);
                            have_file = true;
                        }
                        if (*p == q) {
                            p++;
                        }
                        continue;
                    }
                }
                continue;
            }
            p++;
        }
        if (*p == '>') {
            p++;
        }
        if (invoke && have_file) {
            char target_path[CBM_SZ_1K];
            const uip_entry *target;
            if (uip_normalize_literal_path(target_path, sizeof(target_path), file_name) &&
                (target = uip_find_path(ctx->uipath_index, target_path)) != NULL) {
                uip_emit(ctx->gbuf, source_entry->target_id, target->target_id, target->path,
                         tag_line);
            }
        }
    }
}

static struct cbm_uipath_index *uip_index_new(void) {
    struct cbm_uipath_index *index = (struct cbm_uipath_index *)calloc(1, sizeof(*index));
    if (!index) {
        return NULL;
    }
    index->coded_methods = cbm_ht_create(64);
    if (!index->coded_methods) {
        free(index);
        return NULL;
    }
    return index;
}

int cbm_pipeline_pass_uipath(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                             int file_count) {
    struct cbm_uipath_index *index;
    char edges_buf[16];
    char entries_buf[16];
    int edges_before;
    int edges_after;
    if (!ctx || !ctx->gbuf || !files || file_count <= 0) {
        return 0;
    }
    if (ctx->cancelled && cbm_pipeline_check_cancel(ctx) != 0) {
        return CBM_NOT_FOUND;
    }
    cbm_uipath_index_free(ctx);
    if (!uip_project_gated(files, file_count)) {
        return 0;
    }
    for (int i = 0; i < file_count; i++) {
        if (files[i].rel_path && uip_is_local_cache(files[i].rel_path)) {
            cbm_gbuf_delete_by_file(ctx->gbuf, files[i].rel_path);
        }
    }
    index = uip_index_new();
    if (!index) {
        return 0;
    }
    for (int i = 0; i < file_count; i++) {
        char rel[CBM_SZ_1K];
        bool loaded = false;
        CBMFileResult *result;
        if (!files[i].rel_path || !uip_norm_rel(rel, sizeof(rel), files[i].rel_path)) {
            continue;
        }
        if (uip_is_local_cache(rel)) {
            continue;
        }
        if (uip_ext_is(rel, ".xaml")) {
            if (!uip_index_xaml(ctx, index, rel, files[i].path)) {
                ctx->uipath_index = index;
                cbm_uipath_index_free(ctx);
                return 0;
            }
            continue;
        }
        if (!uip_ext_is(rel, ".cs") || !ctx->result_cache) {
            continue;
        }
        result = cbm_pipeline_result_acquire(ctx, ctx->result_cache, i, NULL, &loaded);
        if (!uip_index_cs(ctx, index, result, rel)) {
            cbm_pipeline_result_release(result, loaded);
            ctx->uipath_index = index;
            cbm_uipath_index_free(ctx);
            return 0;
        }
        cbm_pipeline_result_release(result, loaded);
    }
    uip_assign_members(index);
    if (!uip_bind_lookups(index)) {
        ctx->uipath_index = index;
        cbm_uipath_index_free(ctx);
        return 0;
    }
    ctx->uipath_index = index;
    edges_before = cbm_gbuf_edge_count_by_type(ctx->gbuf, "CALLS");
    for (int i = 0; i < file_count; i++) {
        char rel[CBM_SZ_1K];
        bool loaded = false;
        CBMFileResult *result;
        if (!files[i].rel_path || !uip_norm_rel(rel, sizeof(rel), files[i].rel_path) ||
            uip_is_local_cache(rel)) {
            continue;
        }
        if (uip_ext_is(rel, ".xaml")) {
            uip_emit_xaml(ctx, rel, files[i].path);
            continue;
        }
        if (!uip_ext_is(rel, ".cs") || !ctx->result_cache) {
            continue;
        }
        result = cbm_pipeline_result_acquire(ctx, ctx->result_cache, i, NULL, &loaded);
        uip_emit_cs_calls(ctx, result);
        cbm_pipeline_result_release(result, loaded);
    }
    edges_after = cbm_gbuf_edge_count_by_type(ctx->gbuf, "CALLS");
    snprintf(edges_buf, sizeof(edges_buf), "%d", edges_after - edges_before);
    snprintf(entries_buf, sizeof(entries_buf), "%d", index->count);
    cbm_log_info("pass.done", "pass", "uipath", "edges", edges_buf, "entries", entries_buf);
    return 0;
}
