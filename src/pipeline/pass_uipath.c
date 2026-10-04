#include "pipeline/pipeline_internal.h"
#include "pipeline/uipath.h"

#include "foundation/log.h"
#include "foundation/sha256.h"
#include "yyjson/yyjson.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sqlite3.h"

static void jesc(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    if (!dst || !cap) {
        return;
    }
    if (!src) {
        src = "";
    }
    for (size_t i = 0; src[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c == '\t') {
            dst[o++] = '\\';
            dst[o++] = 't';
        } else if (c < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static char *read_cap(const char *path, size_t cap, size_t *out_len, int *truncated) {
    if (truncated) {
        *truncated = 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *buf = malloc(cap + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, cap + 1, f);
    fclose(f);
    if (n > cap) {
        n = cap;
        if (truncated) {
            *truncated = 1;
        }
    }
    buf[n] = '\0';
    if (out_len) {
        *out_len = n;
    }
    return buf;
}

static int is_dot_local(const char *rel) {
    return rel && (strncmp(rel, ".local/", 7) == 0 || strstr(rel, "/.local/") != NULL);
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void norm_slash(char *s) {
    for (; s && *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

static void lower_copy(const char *in, char *out, size_t cap) {
    size_t i = 0;
    for (; in && in[i] && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        out[i] = (char)tolower(c);
    }
    out[i] = '\0';
}

static void collapse_path(const char *in, char *out, size_t cap) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", in ? in : "");
    norm_slash(tmp);
    char *parts[64];
    int np = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok && np < 64; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, "") == 0 || strcmp(tok, ".") == 0) {
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            if (np > 0) {
                np--;
            }
            continue;
        }
        parts[np++] = tok;
    }
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < np; i++) {
        size_t n = strlen(parts[i]);
        if (o && o + 1 < cap) {
            out[o++] = '/';
        }
        if (o + n >= cap) {
            break;
        }
        memcpy(out + o, parts[i], n);
        o += n;
        out[o] = '\0';
    }
}

static void join_under_root(const char *root, const char *raw, char *out, size_t cap,
                            int *escaped) {
    char cleaned[768];
    snprintf(cleaned, sizeof(cleaned), "%s", raw ? raw : "");
    norm_slash(cleaned);
    while (cleaned[0] == ' ' || cleaned[0] == '[') {
        memmove(cleaned, cleaned + 1, strlen(cleaned));
    }
    size_t L = strlen(cleaned);
    while (L && (cleaned[L - 1] == ' ' || cleaned[L - 1] == ']')) {
        cleaned[--L] = '\0';
    }
    if (cleaned[0] == '/') {
        memmove(cleaned, cleaned + 1, strlen(cleaned));
    }
    char joined[1100];
    if (root && root[0]) {
        snprintf(joined, sizeof(joined), "%s/%s", root, cleaned);
    } else {
        snprintf(joined, sizeof(joined), "%s", cleaned);
    }
    collapse_path(joined, out, cap);
    if (escaped) {
        size_t rl = root ? strlen(root) : 0;
        *escaped = rl > 0 && !(strncmp(out, root, rl) == 0 && (out[rl] == '/' || out[rl] == '\0'));
    }
}

static void sanitize_member(const char *stem, char *out, size_t cap) {
    size_t o = 0;
    if (!stem) {
        stem = "";
    }
    if (stem[0] >= '0' && stem[0] <= '9' && o + 1 < cap) {
        out[o++] = '_';
    }
    for (size_t i = 0; stem[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)stem[i];
        if (isalnum(c) || c == '_') {
            out[o++] = (char)c;
        } else {
            out[o++] = '_';
        }
    }
    out[o] = '\0';
    if (!out[0]) {
        snprintf(out, cap, "_wf");
    }
}

static int64_t up_upsert(cbm_gbuf_t *gb, const char *label, const char *name, const char *qn,
                         const char *file, int sl, int el, const char *props) {
    if (!qn || !qn[0]) {
        return 0;
    }
    if (!name || !name[0]) {
        name = qn;
    }
    return cbm_gbuf_upsert_node(gb, label, name, qn, file ? file : "", sl, el,
                                props ? props : "{}");
}

bool cbm_uipath_suppress_generic_call(const cbm_pipeline_ctx_t *ctx, const char *callee_name,
                                      const char *first_string_arg) {
    if (!ctx || !ctx->uipath_active || !callee_name) {
        return false;
    }
    if (strncmp(callee_name, "workflows.", 10) == 0 && callee_name[10]) {
        return true;
    }
    const char *run = strstr(callee_name, "RunWorkflow");
    if (run && first_string_arg && first_string_arg[0]) {
        return true;
    }
    return false;
}

static int text_is_uipath_project(const char *text) {
    return text && (strstr(text, "expressionLanguage") || strstr(text, "studioVersion") ||
                    strstr(text, "UiPath."));
}

typedef struct {
    char root[512];
    char qn[576];
    char name[128];
    char expr[32];
    char main_path[512];
    char output[32];
    char framework[32];
    char deps[3000];
    char entries[2000];
    char privates[1500];
    char tests[1500];
    char target[32];
    char studio[48];
} up_proj;

static int proj_index(up_proj *ps, int n, const char *root) {
    for (int i = 0; i < n; i++) {
        if (strcmp(ps[i].root, root) == 0) {
            return i;
        }
    }
    return -1;
}

static void proj_add_list(char *dst, size_t cap, const char *item) {
    if (!item || !item[0]) {
        return;
    }
    size_t L = strlen(dst);
    size_t n = strlen(item);
    if (L && L + 1 < cap) {
        dst[L++] = '|';
        dst[L] = '\0';
    }
    if (L + n < cap) {
        memcpy(dst + L, item, n + 1);
    }
}

static int list_has(const char *list, const char *item) {
    if (!list || !item || !item[0]) {
        return 0;
    }
    const char *p = list;
    size_t n = strlen(item);
    while (*p) {
        if ((p == list || p[-1] == '|') && strncmp(p, item, n) == 0 &&
            (p[n] == '|' || p[n] == '\0')) {
            return 1;
        }
        p++;
    }
    return 0;
}

static void parse_project_json(cbm_gbuf_t *gb, up_proj *slot, const char *rel, const char *text) {
    const char *slash = strrchr(rel, '/');
    if (slash) {
        size_t n = (size_t)(slash - rel);
        if (n >= sizeof(slot->root)) {
            n = sizeof(slot->root) - 1;
        }
        memcpy(slot->root, rel, n);
        slot->root[n] = '\0';
    } else {
        slot->root[0] = '\0';
    }
    snprintf(slot->qn, sizeof(slot->qn), "uipath:%s", slot->root[0] ? slot->root : ".");
    snprintf(slot->expr, sizeof(slot->expr), "VisualBasic");
    snprintf(slot->output, sizeof(slot->output), "Process");
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *name = yyjson_obj_get(root, "name");
        if (yyjson_is_str(name)) {
            snprintf(slot->name, sizeof(slot->name), "%s", yyjson_get_str(name));
        }
        yyjson_val *expr = yyjson_obj_get(root, "expressionLanguage");
        if (yyjson_is_str(expr)) {
            snprintf(slot->expr, sizeof(slot->expr), "%s", yyjson_get_str(expr));
        }
        yyjson_val *studio = yyjson_obj_get(root, "studioVersion");
        if (yyjson_is_str(studio)) {
            snprintf(slot->studio, sizeof(slot->studio), "%s", yyjson_get_str(studio));
        }
        yyjson_val *tf = yyjson_obj_get(root, "targetFramework");
        if (yyjson_is_str(tf)) {
            snprintf(slot->target, sizeof(slot->target), "%s", yyjson_get_str(tf));
        }
        yyjson_val *mainv = yyjson_obj_get(root, "main");
        if (yyjson_is_str(mainv)) {
            join_under_root(slot->root, yyjson_get_str(mainv), slot->main_path,
                            sizeof(slot->main_path), NULL);
            proj_add_list(slot->entries, sizeof(slot->entries), slot->main_path);
        }
        yyjson_val *dopt = yyjson_obj_get(root, "designOptions");
        if (yyjson_is_obj(dopt)) {
            yyjson_val *ot = yyjson_obj_get(dopt, "outputType");
            if (yyjson_is_str(ot)) {
                snprintf(slot->output, sizeof(slot->output), "%s", yyjson_get_str(ot));
            }
        }
        yyjson_val *deps = yyjson_obj_get(root, "dependencies");
        if (yyjson_is_obj(deps)) {
            yyjson_obj_iter it;
            yyjson_obj_iter_init(deps, &it);
            yyjson_val *k;
            while ((k = yyjson_obj_iter_next(&it))) {
                yyjson_val *v = yyjson_obj_iter_get_val(k);
                const char *id = yyjson_get_str(k);
                const char *constraint = yyjson_is_str(v) ? yyjson_get_str(v) : "";
                char piece[192];
                snprintf(piece, sizeof(piece), "%s=%s", id ? id : "", constraint);
                proj_add_list(slot->deps, sizeof(slot->deps), piece);
                char pqn[200];
                snprintf(pqn, sizeof(pqn), "nuget:%s", id ? id : "pkg");
                char e_id[120], e_c[80];
                jesc(e_id, sizeof(e_id), id);
                jesc(e_c, sizeof(e_c), constraint);
                char props[320];
                snprintf(props, sizeof(props),
                         "{\"domain\":\"uipath\",\"ecosystem\":\"nuget\",\"constraint\":\"%s\","
                         "\"project_qn\":\"%s\",\"strategy\":\"uipath_pkg\"}",
                         e_c, slot->qn);
                up_upsert(gb, "Package", e_id[0] ? id : "pkg", pqn, rel, 1, 1, props);
            }
        }
        yyjson_val *eps = yyjson_obj_get(root, "entryPoints");
        if (yyjson_is_arr(eps)) {
            size_t idx, max;
            yyjson_val *el;
            yyjson_arr_foreach(eps, idx, max, el) {
                yyjson_val *fp = yyjson_is_obj(el) ? yyjson_obj_get(el, "filePath") : NULL;
                if (yyjson_is_str(fp)) {
                    char full[512];
                    join_under_root(slot->root, yyjson_get_str(fp), full, sizeof(full), NULL);
                    proj_add_list(slot->entries, sizeof(slot->entries), full);
                }
            }
        }
        yyjson_val *lib = yyjson_obj_get(root, "libraryOptions");
        if (yyjson_is_obj(lib)) {
            yyjson_val *pw = yyjson_obj_get(lib, "privateWorkflows");
            if (yyjson_is_arr(pw)) {
                size_t idx, max;
                yyjson_val *el;
                yyjson_arr_foreach(pw, idx, max, el) {
                    if (yyjson_is_str(el)) {
                        char full[512];
                        join_under_root(slot->root, yyjson_get_str(el), full, sizeof(full), NULL);
                        proj_add_list(slot->privates, sizeof(slot->privates), full);
                    }
                }
            }
        }
        yyjson_val *fic = yyjson_obj_get(root, "fileInfoCollection");
        if (yyjson_is_obj(fic)) {
            yyjson_obj_iter it;
            yyjson_obj_iter_init(fic, &it);
            yyjson_val *k;
            while ((k = yyjson_obj_iter_next(&it))) {
                yyjson_val *v = yyjson_obj_iter_get_val(k);
                int test = 0;
                if (yyjson_is_obj(v) &&
                    (yyjson_obj_get(v, "testCaseId") || yyjson_obj_get(v, "testCaseType"))) {
                    test = 1;
                }
                if (test && yyjson_is_str(k)) {
                    char full[512];
                    join_under_root(slot->root, yyjson_get_str(k), full, sizeof(full), NULL);
                    proj_add_list(slot->tests, sizeof(slot->tests), full);
                }
            }
        }
        yyjson_doc_free(doc);
    }
    if (!slot->name[0]) {
        snprintf(slot->name, sizeof(slot->name), "%s",
                 slot->root[0] ? base_name(slot->root) : "UiPath");
    }
    char e_name[160], e_expr[40], e_out[40], e_fw[40], e_main[200], e_deps[400], e_ent[400];
    jesc(e_name, sizeof(e_name), slot->name);
    jesc(e_expr, sizeof(e_expr), slot->expr);
    jesc(e_out, sizeof(e_out), slot->output);
    jesc(e_fw, sizeof(e_fw), slot->framework);
    jesc(e_main, sizeof(e_main), slot->main_path);
    jesc(e_deps, sizeof(e_deps), slot->deps);
    jesc(e_ent, sizeof(e_ent), slot->entries);
    char props[1600];
    snprintf(props, sizeof(props),
             "{\"domain\":\"uipath\",\"name\":\"%s\",\"expression_language\":\"%s\","
             "\"output_type\":\"%s\",\"target_framework\":\"%s\",\"studio_version\":\"%s\","
             "\"main\":\"%s\",\"framework\":\"%s\",\"dependencies\":\"%s\",\"entries\":\"%s\","
             "\"strategy\":\"uipath_project\"}",
             e_name, e_expr, e_out, slot->target, slot->studio, e_main, e_fw, e_deps, e_ent);
    up_upsert(gb, "UiPathProject", slot->name, slot->qn, rel, 1, 1, props);
}

static up_proj *owning_proj(up_proj *ps, int n, const char *rel) {
    up_proj *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < n; i++) {
        size_t L = strlen(ps[i].root);
        if (L == 0) {
            if (!best) {
                best = &ps[i];
            }
            continue;
        }
        if (strncmp(rel, ps[i].root, L) == 0 && (rel[L] == '/' || rel[L] == '\0') &&
            L >= best_len) {
            best = &ps[i];
            best_len = L;
        }
    }
    return best;
}

typedef struct {
    cbm_gbuf_t *gb;
    const char *wf;
    const char *project_qn;
    const char *expr_proj;
    int is_entry;
    int is_test;
    int is_private;
    char outline[6000];
    size_t outline_n;
    char args_sig[1500];
    size_t args_n;
    char root_kind[48];
    char x_class[200];
    char annotation[240];
    char expr_lang[24];
    char skeleton[1500];
    int activity_count;
    int parse_status;
    int max_line;
    cbm_pipeline_t *pipeline;
} scan_ud;

static void append_text(char *dst, size_t cap, size_t *used, const char *s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    if (*used + n + 1 >= cap) {
        return;
    }
    memcpy(dst + *used, s, n + 1);
    *used += n;
}

static const char *norm_type_str(const char *in, char *buf, size_t cap) {
    if (!in) {
        in = "";
    }
    const char *open = strchr(in, '(');
    const char *close = open ? strchr(open, ')') : NULL;
    const char *body = in;
    if (open && close && close > open + 1) {
        size_t n = (size_t)(close - open - 1);
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(buf, open + 1, n);
        buf[n] = '\0';
        body = buf;
    }
    if (strncmp(body, "x:", 2) == 0) {
        body += 2;
    }
    if (strcmp(body, "String") == 0) {
        snprintf(buf, cap, "System.String");
        return buf;
    }
    if (strcmp(body, "Int32") == 0) {
        snprintf(buf, cap, "System.Int32");
        return buf;
    }
    if (strcmp(body, "Boolean") == 0) {
        snprintf(buf, cap, "System.Boolean");
        return buf;
    }
    if (strcmp(body, "Object") == 0) {
        snprintf(buf, cap, "System.Object");
        return buf;
    }
    if (body != buf) {
        snprintf(buf, cap, "%s", body);
    }
    return buf;
}

static void on_xaml_item(void *ud, const uipath_xaml_item *item) {
    scan_ud *s = ud;
    if (item->kind == UIP_XAML_META) {
        snprintf(s->root_kind, sizeof(s->root_kind), "%s", item->root_kind);
        snprintf(s->x_class, sizeof(s->x_class), "%s", item->x_class);
        if (item->annotation[0]) {
            snprintf(s->annotation, sizeof(s->annotation), "%s", item->annotation);
        }
        snprintf(s->expr_lang, sizeof(s->expr_lang), "%s", item->expr_lang);
        snprintf(s->skeleton, sizeof(s->skeleton), "%s", item->facts);
        s->activity_count = item->activity_count;
        s->parse_status = item->parse_status;
        return;
    }
    if (item->kind == UIP_XAML_ARGUMENT) {
        char qn[700];
        snprintf(qn, sizeof(qn), "%s#arg:%s", s->wf, item->name);
        char tb[128];
        const char *ty = norm_type_str(item->type_name, tb, sizeof(tb));
        char e1[160], e2[80], e3[140];
        jesc(e1, sizeof(e1), item->name);
        jesc(e2, sizeof(e2), item->direction);
        jesc(e3, sizeof(e3), ty);
        char props[640];
        snprintf(props, sizeof(props),
                 "{\"domain\":\"uipath\",\"direction\":\"%s\",\"type\":\"%s\",\"naming_ok\":%s,"
                 "\"strategy\":\"uipath_structure\",\"project_qn\":\"%s\"}",
                 e2, e3, item->naming_ok ? "true" : "false", s->project_qn ? s->project_qn : "");
        up_upsert(s->gb, "Argument", item->name, qn, s->wf, item->start_line, item->end_line,
                  props);
        char line[200];
        snprintf(line, sizeof(line), "%s %s:%s", item->direction, item->name, ty);
        if (s->args_n) {
            append_text(s->args_sig, sizeof(s->args_sig), &s->args_n, ", ");
        }
        append_text(s->args_sig, sizeof(s->args_sig), &s->args_n, line);
        return;
    }
    if (item->kind == UIP_XAML_VARIABLE) {
        char qn[760];
        snprintf(qn, sizeof(qn), "%s#var:%s:%s", s->wf, item->parent_id[0] ? item->parent_id : "_",
                 item->name);
        char tb[128];
        const char *ty = norm_type_str(item->type_name, tb, sizeof(tb));
        char e1[120], e2[140], e3[80];
        jesc(e1, sizeof(e1), item->name);
        jesc(e2, sizeof(e2), ty);
        jesc(e3, sizeof(e3), item->parent_id);
        char props[640];
        snprintf(props, sizeof(props),
                 "{\"domain\":\"uipath\",\"type\":\"%s\",\"scope_id_ref\":\"%s\","
                 "\"parent_id\":\"%s\",\"strategy\":\"uipath_structure\",\"project_qn\":\"%s\"}",
                 e2, e3, e3, s->project_qn ? s->project_qn : "");
        up_upsert(s->gb, "Variable", item->name[0] ? item->name : "var", qn, s->wf,
                  item->start_line, item->end_line, props);
        (void)e1;
        return;
    }
    if (item->kind == UIP_XAML_ACTIVITY && item->id_ref[0]) {
        char qn[800];
        snprintf(qn, sizeof(qn), "%s#%s", s->wf, item->id_ref);
        char e_name[180], e_type[120], e_id[100], e_clr[220], e_pkg[100], e_slot[50], e_parent[100],
            e_ann[240], e_bc[300], e_prot[100], e_facts[8192];
        jesc(e_name, sizeof(e_name), item->name);
        jesc(e_type, sizeof(e_type), item->type_name);
        jesc(e_id, sizeof(e_id), item->id_ref);
        jesc(e_clr, sizeof(e_clr), item->clr_type);
        jesc(e_pkg, sizeof(e_pkg), item->package);
        jesc(e_slot, sizeof(e_slot), item->slot);
        jesc(e_parent, sizeof(e_parent), item->parent_id);
        jesc(e_ann, sizeof(e_ann), item->annotation);
        jesc(e_bc, sizeof(e_bc), item->breadcrumb);
        jesc(e_prot, sizeof(e_prot), item->protected_by);
        jesc(e_facts, sizeof(e_facts), item->facts);
        char *props = malloc(16384);
        if (!props) {
            return;
        }
        snprintf(props, 16384,
                 "{\"domain\":\"uipath\",\"activity_type\":\"%s\",\"clr_type\":\"%s\","
                 "\"package\":\"%s\",\"id_ref\":\"%s\",\"id_source\":\"%s\",\"slot\":\"%s\","
                 "\"parent_id\":\"%s\",\"breadcrumb\":\"%s\",\"protected_by\":\"%s\","
                 "\"annotation\":\"%s\",\"ordinal\":%d,\"facts\":\"%s\","
                 "\"strategy\":\"uipath_structure\",\"project_qn\":\"%s\",\"docstring\":\"%s\"}",
                 e_type, e_clr, e_pkg, e_id, item->id_source, e_slot, e_parent, e_bc, e_prot, e_ann,
                 item->ordinal, e_facts, s->project_qn ? s->project_qn : "", e_ann);
        up_upsert(s->gb, "Activity", item->name[0] ? item->name : item->type_name, qn, s->wf,
                  item->start_line, item->end_line, props);
        if (item->end_line > s->max_line) {
            s->max_line = item->end_line;
        }
        free(props);
        char line[240];
        snprintf(line, sizeof(line), "%s %s\n", item->id_ref, item->name);
        append_text(s->outline, sizeof(s->outline), &s->outline_n, line);
    }
}

static void finish_workflow_node(scan_ud *s, const char *kind) {
    const char *status = "ok";
    if (s->parse_status == 1) {
        status = "partial";
    } else if (s->parse_status == 2) {
        status = "truncated";
    }
    char e_kind[24], e_root[48], e_ann[240], e_x[200], e_sk[400], e_doc[500], e_sig[400],
        e_expr[32];
    jesc(e_kind, sizeof(e_kind), kind);
    jesc(e_root, sizeof(e_root), s->root_kind);
    jesc(e_ann, sizeof(e_ann), s->annotation);
    jesc(e_x, sizeof(e_x), s->x_class);
    jesc(e_sk, sizeof(e_sk), s->skeleton);
    jesc(e_doc, sizeof(e_doc), s->outline);
    jesc(e_sig, sizeof(e_sig), s->args_sig);
    const char *el = s->expr_lang[0] ? s->expr_lang : (s->expr_proj ? s->expr_proj : "VisualBasic");
    if (strcmp(el, "cs") == 0) {
        el = "CSharp";
    } else if (strcmp(el, "vb") == 0) {
        el = "VisualBasic";
    }
    jesc(e_expr, sizeof(e_expr), el);
    char props[2200];
    snprintf(props, sizeof(props),
             "{\"domain\":\"uipath\",\"kind\":\"%s\",\"root_kind\":\"%s\",\"expr_lang\":\"%s\","
             "\"is_entry_point\":%s,\"is_test\":%s,\"is_private\":%s,\"annotation\":\"%s\","
             "\"x_class\":\"%s\",\"activity_count\":%d,\"parse_status\":\"%s\",\"skeleton\":\"%s\","
             "\"docstring\":\"%s\",\"signature\":\"%s\",\"project_qn\":\"%s\","
             "\"strategy\":\"uipath_structure\"}",
             e_kind, e_root, e_expr, s->is_entry ? "true" : "false", s->is_test ? "true" : "false",
             s->is_private ? "true" : "false", e_ann, e_x, s->activity_count, status, e_sk, e_doc,
             e_sig, s->project_qn ? s->project_qn : "");
    const char *nm = base_name(s->wf);
    int end_line = s->max_line > 0 ? s->max_line : 1;
    up_upsert(s->gb, "Workflow", nm, s->wf, s->wf, 1, end_line, props);
    if (s->parse_status == 2 && s->pipeline) {
        cbm_pipeline_add_file_error(s->pipeline, s->wf, "uipath workflow truncated",
                                    "parse_partial");
    }
}

static int line_at(const char *src, const char *hit) {
    int line = 1;
    for (const char *p = src; p && hit && p < hit; p++) {
        if (*p == '\n') {
            line++;
        }
    }
    return line;
}

static void add_coded_activity(cbm_gbuf_t *gb, const char *wf, const char *project_qn, int line,
                               const char *id, const char *type_name, const char *facts) {
    char qn[700];
    snprintf(qn, sizeof(qn), "%s#%s", wf, id);
    char e_t[80], e_id[80], e_f[800];
    jesc(e_t, sizeof(e_t), type_name);
    jesc(e_id, sizeof(e_id), id);
    jesc(e_f, sizeof(e_f), facts);
    char props[1200];
    snprintf(
        props, sizeof(props),
        "{\"domain\":\"uipath\",\"activity_type\":\"%s\",\"id_ref\":\"%s\",\"id_source\":\"idref\","
        "\"parent_id\":\"\",\"facts\":\"%s\",\"strategy\":\"uipath_structure\","
        "\"project_qn\":\"%s\",\"clr_type\":\"%s\"}",
        e_t, e_id, e_f, project_qn ? project_qn : "", e_t);
    up_upsert(gb, "Activity", type_name, qn, wf, line, line, props);
}

static void scan_coded(cbm_gbuf_t *gb, cbm_pipeline_t *pipeline, const char *rel, const char *src,
                       up_proj *proj) {
    (void)pipeline;
    int marked =
        strstr(src, "[Workflow]") || strstr(src, "[TestCase]") || strstr(src, "CodedWorkflow");
    if (!marked) {
        return;
    }
    scan_ud ud;
    memset(&ud, 0, sizeof(ud));
    ud.gb = gb;
    ud.wf = rel;
    ud.project_qn = proj ? proj->qn : "";
    ud.expr_proj = "CSharp";
    ud.is_test = strstr(src, "[TestCase]") != NULL || (proj && list_has(proj->tests, rel));
    ud.is_entry = proj && list_has(proj->entries, rel);
    ud.is_private = proj && list_has(proj->privates, rel);
    snprintf(ud.root_kind, sizeof(ud.root_kind), "Coded");
    snprintf(ud.expr_lang, sizeof(ud.expr_lang), "cs");
    const char *ex = strstr(src, "Execute(");
    if (ex) {
        const char *p = ex + 8;
        const char *end = strchr(p, ')');
        if (end && (size_t)(end - p) < 800) {
            char buf[800];
            memcpy(buf, p, (size_t)(end - p));
            buf[end - p] = '\0';
            char *save = NULL;
            for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                while (*tok == ' ') {
                    tok++;
                }
                char *name = strrchr(tok, ' ');
                if (!name || !name[1]) {
                    continue;
                }
                *name = '\0';
                name++;
                const char *dir = "In";
                if (strstr(tok, "OutArgument") && !strstr(tok, "InOut")) {
                    dir = "Out";
                } else if (strstr(tok, "InOutArgument")) {
                    dir = "InOut";
                }
                char qn[700];
                snprintf(qn, sizeof(qn), "%s#arg:%s", rel, name);
                char props[400];
                snprintf(
                    props, sizeof(props),
                    "{\"domain\":\"uipath\",\"direction\":\"%s\",\"type\":\"%s\","
                    "\"naming_ok\":true,\"strategy\":\"uipath_structure\",\"project_qn\":\"%s\"}",
                    dir, tok, ud.project_qn);
                up_upsert(gb, "Argument", name, qn, rel, line_at(src, ex), line_at(src, ex), props);
            }
        }
    }
    const char *p = src;
    while ((p = strstr(p, "workflows.")) != NULL) {
        const char *m = p + 10;
        char mem[96];
        size_t i = 0;
        while (isalnum((unsigned char)m[i]) || m[i] == '_') {
            if (i + 1 < sizeof(mem)) {
                mem[i] = m[i];
            }
            i++;
            if (i > 80) {
                break;
            }
        }
        mem[i < sizeof(mem) ? i : sizeof(mem) - 1] = '\0';
        if (mem[0] && m[i] == '(') {
            int line = line_at(src, p);
            char id[64];
            snprintf(id, sizeof(id), "workflows_%s_%d", mem, line);
            char facts[160];
            snprintf(facts, sizeof(facts), "member\t%s", mem);
            add_coded_activity(gb, rel, ud.project_qn, line, id, "workflows", facts);
            ud.activity_count++;
        }
        p += 10;
    }
    p = src;
    while ((p = strstr(p, "RunWorkflow")) != NULL) {
        const char *q = strchr(p, '(');
        const char *q2 = q ? strchr(q, '"') : NULL;
        if (q2) {
            q2++;
            char path[256];
            size_t i = 0;
            while (q2[i] && q2[i] != '"' && i + 1 < sizeof(path)) {
                path[i] = q2[i];
                i++;
            }
            path[i] = '\0';
            int line = line_at(src, p);
            char id[64];
            snprintf(id, sizeof(id), "RunWorkflow_%d", line);
            char facts[320];
            snprintf(facts, sizeof(facts), "invoke\t%s", path);
            add_coded_activity(gb, rel, ud.project_qn, line, id, "RunWorkflow", facts);
            ud.activity_count++;
        }
        p += 11;
    }
    static const char *ops[][3] = {{"GetAsset", "asset", "get"},
                                   {"GetCredential", "asset", "credential"},
                                   {"SetAsset", "asset", "set"},
                                   {"AddQueueItem", "queue", "enqueue"},
                                   {"GetTransactionItem", "queue", "dequeue"},
                                   {NULL, NULL, NULL}};
    for (int oi = 0; ops[oi][0]; oi++) {
        const char *h = src;
        while ((h = strstr(h, ops[oi][0])) != NULL) {
            const char *q = strchr(h, '"');
            if (!q || q - h > 80) {
                h += strlen(ops[oi][0]);
                continue;
            }
            q++;
            char arg[128];
            size_t i = 0;
            while (q[i] && q[i] != '"' && i + 1 < sizeof(arg)) {
                arg[i] = q[i];
                i++;
            }
            arg[i] = '\0';
            int line = line_at(src, h);
            char id[80];
            snprintf(id, sizeof(id), "%s_%d", ops[oi][0], line);
            char facts[240];
            snprintf(facts, sizeof(facts), "%s\t%s\t%s\tliteral", ops[oi][1], ops[oi][2], arg);
            add_coded_activity(gb, rel, ud.project_qn, line, id, ops[oi][0], facts);
            ud.activity_count++;
            h += strlen(ops[oi][0]);
        }
    }
    finish_workflow_node(&ud, ud.is_test ? "test_case" : "coded");
}

static void index_known_configs(cbm_gbuf_t *gb, const char *repo, up_proj *proj) {
    const char *rels[] = {"Data/Config.xlsx", "Config.xlsx", "Data/Config.json", "Config.json"};
    for (int i = 0; i < 4; i++) {
        char rel[700];
        if (proj->root[0]) {
            snprintf(rel, sizeof(rel), "%s/%s", proj->root, rels[i]);
        } else {
            snprintf(rel, sizeof(rel), "%s", rels[i]);
        }
        char abs[1100];
        snprintf(abs, sizeof(abs), "%s/%s", repo, rel);
        struct stat st;
        if (stat(abs, &st) == 0) {
            uipath_config_index_file(gb, abs, rel, proj->qn);
        }
    }
    const char *dirs[2];
    char d0[700], d1[700];
    if (proj->root[0]) {
        snprintf(d0, sizeof(d0), "%s/%s", repo, proj->root);
        snprintf(d1, sizeof(d1), "%s/%s/Data", repo, proj->root);
    } else {
        snprintf(d0, sizeof(d0), "%s", repo);
        snprintf(d1, sizeof(d1), "%s/Data", repo);
    }
    dirs[0] = d0;
    dirs[1] = d1;
    const char *prefix_rel[2];
    char r0[512], r1[512];
    snprintf(r0, sizeof(r0), "%s", proj->root);
    if (proj->root[0]) {
        snprintf(r1, sizeof(r1), "%s/Data", proj->root);
    } else {
        snprintf(r1, sizeof(r1), "Data");
    }
    prefix_rel[0] = r0;
    prefix_rel[1] = r1;
    for (int d = 0; d < 2; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) {
            continue;
        }
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            const char *nm = ent->d_name;
            size_t nl = strlen(nm);
            if (nl > 5 && strncmp(nm, "Config", 6) == 0 && strcmp(nm + nl - 5, ".json") == 0) {
                char rel[700];
                if (prefix_rel[d][0]) {
                    snprintf(rel, sizeof(rel), "%s/%s", prefix_rel[d], nm);
                } else {
                    snprintf(rel, sizeof(rel), "%s", nm);
                }
                char abs[1100];
                snprintf(abs, sizeof(abs), "%s/%s", repo, rel);
                uipath_config_index_file(gb, abs, rel, proj->qn);
            }
        }
        closedir(dir);
    }
}

typedef struct {
    int64_t id;
    char *qn;
    char *name;
    char *file;
    char *props;
    int line;
} up_n;

static void free_nodes(up_n *a, int n) {
    for (int i = 0; i < n; i++) {
        free(a[i].qn);
        free(a[i].name);
        free(a[i].file);
        free(a[i].props);
    }
    free(a);
}

static up_n *copy_label(cbm_gbuf_t *gb, const char *label, int *out_n) {
    *out_n = 0;
    const cbm_gbuf_node_t **nodes = NULL;
    int n = 0;
    if (cbm_gbuf_find_by_label(gb, label, &nodes, &n) != 0 || n <= 0) {
        return NULL;
    }
    up_n *a = calloc((size_t)n, sizeof(up_n));
    if (!a) {
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        a[i].id = nodes[i]->id;
        a[i].qn = strdup(nodes[i]->qualified_name ? nodes[i]->qualified_name : "");
        a[i].name = strdup(nodes[i]->name ? nodes[i]->name : "");
        a[i].file = strdup(nodes[i]->file_path ? nodes[i]->file_path : "");
        a[i].props = strdup(nodes[i]->properties_json ? nodes[i]->properties_json : "{}");
        a[i].line = nodes[i]->start_line;
    }
    *out_n = n;
    return a;
}

static yyjson_val *props_of(yyjson_doc **doc, const char *json) {
    *doc = yyjson_read(json ? json : "{}", strlen(json ? json : "{}"), 0);
    return *doc ? yyjson_doc_get_root(*doc) : NULL;
}

static const char *js(yyjson_val *o, const char *k) {
    yyjson_val *v = o ? yyjson_obj_get(o, k) : NULL;
    return v && yyjson_is_str(v) ? yyjson_get_str(v) : "";
}

static int js_bool(yyjson_val *o, const char *k) {
    yyjson_val *v = o ? yyjson_obj_get(o, k) : NULL;
    return v && yyjson_is_bool(v) && yyjson_get_bool(v);
}

typedef struct {
    int64_t src;
    int64_t dst;
    int sites;
    double conf;
    char how[24];
    int is_test;
} call_agg;

static void agg_call(call_agg **a, int *n, int *cap, int64_t s, int64_t d, double conf,
                     const char *how, int is_test) {
    for (int i = 0; i < *n; i++) {
        if ((*a)[i].src == s && (*a)[i].dst == d) {
            (*a)[i].sites++;
            if (conf > (*a)[i].conf) {
                (*a)[i].conf = conf;
                snprintf((*a)[i].how, sizeof((*a)[i].how), "%s", how);
            }
            return;
        }
    }
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 32;
        call_agg *g = realloc(*a, (size_t)nc * sizeof(call_agg));
        if (!g) {
            return;
        }
        *a = g;
        *cap = nc;
    }
    (*a)[*n].src = s;
    (*a)[*n].dst = d;
    (*a)[*n].sites = 1;
    (*a)[*n].conf = conf;
    snprintf((*a)[*n].how, sizeof((*a)[*n].how), "%s", how ? how : "literal");
    (*a)[*n].is_test = is_test;
    (*n)++;
}

static up_n *find_qn(up_n *a, int n, const char *qn) {
    for (int i = 0; i < n; i++) {
        if (a[i].qn && strcmp(a[i].qn, qn) == 0) {
            return &a[i];
        }
    }
    return NULL;
}

static up_n *find_wf(up_n *wfs, int n, const char *path, int ci) {
    if (!path || !path[0]) {
        return NULL;
    }
    if (!ci) {
        for (int i = 0; i < n; i++) {
            if (strcmp(wfs[i].qn, path) == 0 || strcmp(wfs[i].file, path) == 0) {
                return &wfs[i];
            }
        }
        return NULL;
    }
    char want[512];
    lower_copy(path, want, sizeof(want));
    for (int i = 0; i < n; i++) {
        char got[512];
        lower_copy(wfs[i].qn, got, sizeof(got));
        if (strcmp(got, want) == 0) {
            return &wfs[i];
        }
    }
    return NULL;
}

static up_n *find_member(up_n *wfs, int n, const char *project, const char *member) {
    for (int i = 0; i < n; i++) {
        yyjson_doc *d = NULL;
        yyjson_val *o = props_of(&d, wfs[i].props);
        const char *mem = js(o, "member");
        const char *pq = js(o, "project_qn");
        int ok =
            mem[0] && strcmp(mem, member) == 0 && (project[0] == '\0' || strcmp(pq, project) == 0);
        if (d) {
            yyjson_doc_free(d);
        }
        if (ok) {
            return &wfs[i];
        }
    }
    return NULL;
}

static void assign_members(up_n *wfs, int n) {
    for (int i = 0; i < n; i++) {
        const char *file = wfs[i].file[0] ? wfs[i].file : wfs[i].qn;
        const char *base = base_name(file);
        char stem[256];
        snprintf(stem, sizeof(stem), "%s", base);
        char *dot = strrchr(stem, '.');
        if (dot) {
            *dot = '\0';
        }
        char mem[256];
        sanitize_member(stem, mem, sizeof(mem));
        int dup = 0;
        for (int j = 0; j < n; j++) {
            if (i == j) {
                continue;
            }
            const char *b2 = base_name(wfs[j].file[0] ? wfs[j].file : wfs[j].qn);
            char s2[256];
            snprintf(s2, sizeof(s2), "%s", b2);
            char *d2 = strrchr(s2, '.');
            if (d2) {
                *d2 = '\0';
            }
            char m2[256];
            sanitize_member(s2, m2, sizeof(m2));
            if (strcmp(mem, m2) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            char pathstem[512];
            snprintf(pathstem, sizeof(pathstem), "%s", file);
            char *ext = strrchr(pathstem, '.');
            if (ext) {
                *ext = '\0';
            }
            sanitize_member(pathstem, mem, sizeof(mem));
        }
        char e[256];
        jesc(e, sizeof(e), mem);
        /* splice member into a copy of props by re-upsert */
        size_t L = strlen(wfs[i].props);
        char *props = malloc(L + 64 + strlen(e));
        if (!props) {
            continue;
        }
        if (L && wfs[i].props[L - 1] == '}') {
            snprintf(props, L + 64 + strlen(e), "%.*s,\"member\":\"%s\"}", (int)(L - 1),
                     wfs[i].props, e);
        } else {
            snprintf(props, L + 64 + strlen(e), "{\"member\":\"%s\"}", e);
        }
        free(wfs[i].props);
        wfs[i].props = props;
    }
}

static int64_t dyn_target(cbm_gbuf_t *gb, const char *project, const char *expr) {
    char sha[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(expr ? expr : "", expr ? strlen(expr) : 0, sha);
    sha[12] = '\0';
    char qn[400];
    snprintf(qn, sizeof(qn), "%s:dyn:%s", project && project[0] ? project : "uipath:.", sha);
    char e[300];
    jesc(e, sizeof(e), expr);
    char props[400];
    snprintf(props, sizeof(props),
             "{\"domain\":\"uipath\",\"expression\":\"%s\",\"strategy\":\"uipath_invoke\"}", e);
    return up_upsert(gb, "DynamicTarget", expr && expr[0] ? expr : "dynamic", qn, "", 0, 0, props);
}

static int file_loaded(up_n *files, int n, const char *path) {
    for (int i = 0; i < n; i++) {
        if (strcmp(files[i].qn, path) == 0 || strcmp(files[i].file, path) == 0) {
            yyjson_doc *d = NULL;
            yyjson_val *o = props_of(&d, files[i].props);
            int on = js_bool(o, "is_loaded");
            if (d) {
                yyjson_doc_free(d);
            }
            return on;
        }
    }
    return 0;
}

static void mark_loaded(cbm_gbuf_t *gb, up_n *files, int n, const char *path) {
    for (int i = 0; i < n; i++) {
        if (strcmp(files[i].file, path) != 0 && strcmp(files[i].qn, path) != 0) {
            continue;
        }
        yyjson_doc *d = NULL;
        yyjson_val *o = props_of(&d, files[i].props);
        if (!o) {
            if (d) {
                yyjson_doc_free(d);
            }
            continue;
        }
        /* rewrite is_loaded true by string replace */
        char *copy = strdup(files[i].props);
        if (copy) {
            char *hit = strstr(copy, "\"is_loaded\":false");
            if (hit) {
                /* "true " is the same width as "false", so the surrounding JSON stays valid. */
                memcpy(hit, "\"is_loaded\":true ", 17);
            }
            up_upsert(gb, "ConfigFile", files[i].name, files[i].qn, files[i].file, 1, 1, copy);
            free(files[i].props);
            files[i].props = copy;
        }
        if (d) {
            yyjson_doc_free(d);
        }
    }
}

static const char *config_value(up_n *keys, int nkeys, up_n *files, int nfiles, const char *project,
                                const char *lookup) {
    for (int i = 0; i < nkeys; i++) {
        yyjson_doc *d = NULL;
        yyjson_val *o = props_of(&d, keys[i].props);
        const char *lk = js(o, "lookup_key");
        const char *pq = js(o, "project_qn");
        const char *prev = js(o, "value_preview");
        int ok = strcmp(lk, lookup) == 0 && (project[0] == '\0' || strcmp(pq, project) == 0) &&
                 file_loaded(files, nfiles, keys[i].file);
        if (ok) {
            static char buf[128];
            snprintf(buf, sizeof(buf), "%s", prev);
            if (d) {
                yyjson_doc_free(d);
            }
            return buf;
        }
        if (d) {
            yyjson_doc_free(d);
        }
    }
    return NULL;
}

static void link_all(cbm_gbuf_t *gb) {
    int nwf = 0, nact = 0, narg = 0, nvar = 0, nkey = 0, nfile = 0, ncls = 0, nmeth = 0, nproj = 0;
    up_n *wfs = copy_label(gb, "Workflow", &nwf);
    up_n *acts = copy_label(gb, "Activity", &nact);
    up_n *args = copy_label(gb, "Argument", &narg);
    up_n *vars = copy_label(gb, "Variable", &nvar);
    up_n *keys = copy_label(gb, "ConfigKey", &nkey);
    up_n *files = copy_label(gb, "ConfigFile", &nfile);
    up_n *clss = copy_label(gb, "Class", &ncls);
    up_n *meths = copy_label(gb, "Method", &nmeth);
    up_n *projs = copy_label(gb, "UiPathProject", &nproj);
    assign_members(wfs, nwf);
    for (int i = 0; i < nwf; i++) {
        /* persist member onto the node */
        up_upsert(gb, "Workflow", wfs[i].name, wfs[i].qn, wfs[i].file, 1, 1, wfs[i].props);
        yyjson_doc *d = NULL;
        yyjson_val *o = props_of(&d, wfs[i].props);
        const char *pq = js(o, "project_qn");
        up_n *proj = NULL;
        for (int p = 0; p < nproj; p++) {
            if (strcmp(projs[p].qn, pq) == 0) {
                proj = &projs[p];
            }
        }
        if (proj) {
            char props[256];
            snprintf(props, sizeof(props),
                     "{\"strategy\":\"uipath_structure\",\"slot\":\"workflow\"}");
            cbm_gbuf_insert_edge(gb, proj->id, wfs[i].id, "CONTAINS", props);
        }
        if (d) {
            yyjson_doc_free(d);
        }
    }
    /* Load-site detection. */
    for (int i = 0; i < nact; i++) {
        yyjson_doc *d = NULL;
        yyjson_val *o = props_of(&d, acts[i].props);
        const char *facts = js(o, "facts");
        const char *pq = js(o, "project_qn");
        char *copy = facts[0] ? strdup(facts) : NULL;
        if (copy) {
            char *save = NULL;
            for (char *line = strtok_r(copy, "\n", &save); line;
                 line = strtok_r(NULL, "\n", &save)) {
                if (strncmp(line, "load\t", 5) != 0) {
                    continue;
                }
                const char *raw = line + 5;
                const char *root = "";
                if (strncmp(pq, "uipath:", 7) == 0) {
                    root = pq + 7;
                    if (strcmp(root, ".") == 0) {
                        root = "";
                    }
                }
                char resolved[512];
                join_under_root(root, raw, resolved, sizeof(resolved), NULL);
                /* also try the raw tail if the value is an expression containing a path */
                const char *hitj = strstr(raw, "Config.json");
                const char *hitx = strstr(raw, "Config.xlsx");
                const char *hit = hitj ? hitj : hitx;
                if (hit) {
                    const char *start = hit;
                    while (start > raw && start[-1] != '"' && start[-1] != ' ' &&
                           start[-1] != '[') {
                        start--;
                    }
                    char piece[256];
                    size_t n = 0;
                    if (hitj) {
                        n = (size_t)(hitj - start) + strlen("Config.json");
                    } else if (hitx) {
                        n = (size_t)(hitx - start) + strlen("Config.xlsx");
                    }
                    if (n >= sizeof(piece)) {
                        n = sizeof(piece) - 1;
                    }
                    memcpy(piece, start, n);
                    piece[n] = '\0';
                    join_under_root(root, piece, resolved, sizeof(resolved), NULL);
                }
                mark_loaded(gb, files, nfile, resolved);
            }
            free(copy);
        }
        if (d) {
            yyjson_doc_free(d);
        }
    }
    for (int p = 0; p < nproj; p++) {
        int count = 0;
        int only = -1;
        for (int i = 0; i < nfile; i++) {
            yyjson_doc *d = NULL;
            yyjson_val *o = props_of(&d, files[i].props);
            const char *pq = js(o, "project_qn");
            int mine = strcmp(pq, projs[p].qn) == 0;
            if (d) {
                yyjson_doc_free(d);
            }
            if (!mine) {
                continue;
            }
            count++;
            only = i;
        }
        int any = 0;
        for (int i = 0; i < nfile; i++) {
            yyjson_doc *d = NULL;
            yyjson_val *o = props_of(&d, files[i].props);
            if (strcmp(js(o, "project_qn"), projs[p].qn) == 0 && js_bool(o, "is_loaded")) {
                any = 1;
            }
            if (d) {
                yyjson_doc_free(d);
            }
        }
        if (!any && count == 1 && only >= 0) {
            mark_loaded(gb, files, nfile, files[only].file);
        }
    }

    call_agg *ag = NULL;
    int agn = 0, agc = 0;
    for (int i = 0; i < nact; i++) {
        yyjson_doc *d = NULL;
        yyjson_val *o = props_of(&d, acts[i].props);
        if (!o) {
            if (d) {
                yyjson_doc_free(d);
            }
            continue;
        }
        const char *facts = js(o, "facts");
        const char *pq = js(o, "project_qn");
        const char *parent_id = js(o, "parent_id");
        const char *clr = js(o, "clr_type");
        const char *atype = js(o, "activity_type");
        const char *root = "";
        if (strncmp(pq, "uipath:", 7) == 0) {
            root = strcmp(pq + 7, ".") == 0 ? "" : pq + 7;
        }
        up_n *wf = find_qn(wfs, nwf, acts[i].file);
        int64_t parent_node = wf ? wf->id : 0;
        if (parent_id[0]) {
            char pqn[800];
            snprintf(pqn, sizeof(pqn), "%s#%s", acts[i].file, parent_id);
            up_n *pn = find_qn(acts, nact, pqn);
            if (pn) {
                parent_node = pn->id;
            }
        }
        if (parent_node) {
            char ep[160];
            snprintf(ep, sizeof(ep),
                     "{\"strategy\":\"uipath_structure\",\"slot\":\"%s\",\"ordinal\":0}",
                     js(o, "slot"));
            cbm_gbuf_insert_edge(gb, parent_node, acts[i].id, "CONTAINS", ep);
        }
        if (clr[0] || atype[0]) {
            char tqn[300];
            snprintf(tqn, sizeof(tqn), "clr:%s", clr[0] ? clr : atype);
            char tp[300];
            snprintf(tp, sizeof(tp),
                     "{\"domain\":\"uipath\",\"package\":\"%s\",\"source\":\"package\","
                     "\"strategy\":\"uipath_type\"}",
                     js(o, "package"));
            int64_t tid = up_upsert(gb, "ActivityType", atype[0] ? atype : clr, tqn, "", 0, 0, tp);
            char ip[120];
            snprintf(ip, sizeof(ip), "{\"strategy\":\"uipath_type\"}");
            cbm_gbuf_insert_edge(gb, acts[i].id, tid, "INSTANCE_OF", ip);
            if (wf) {
                char up[80];
                snprintf(up, sizeof(up), "{\"strategy\":\"uipath_type\",\"count\":1}");
                cbm_gbuf_insert_edge(gb, wf->id, tid, "USES_ACTIVITY", up);
            }
        }
        if (!facts[0]) {
            yyjson_doc_free(d);
            continue;
        }
        char *copy = strdup(facts);
        if (!copy) {
            yyjson_doc_free(d);
            continue;
        }
        /* expression resolution needs vars/args of this workflow */
        const char *vnames[64];
        const char *vtypes[64];
        int nv = 0;
        const char *anames[64];
        const char *atypes[64];
        int na = 0;
        for (int v = 0; v < nvar && nv < 64; v++) {
            if (strcmp(vars[v].file, acts[i].file) != 0) {
                continue;
            }
            yyjson_doc *vd = NULL;
            yyjson_val *vo = props_of(&vd, vars[v].props);
            vnames[nv] = vars[v].name;
            vtypes[nv] = js(vo, "type");
            /* js() pointer dies with doc — copy */
            char *ts = strdup(vtypes[nv]);
            vtypes[nv] = ts ? ts : "";
            nv++;
            if (vd) {
                yyjson_doc_free(vd);
            }
        }
        for (int a = 0; a < narg && na < 64; a++) {
            if (strcmp(args[a].file, acts[i].file) != 0) {
                continue;
            }
            yyjson_doc *ad = NULL;
            yyjson_val *ao = props_of(&ad, args[a].props);
            anames[na] = args[a].name;
            char *ts = strdup(js(ao, "type"));
            atypes[na] = ts ? ts : "";
            na++;
            if (ad) {
                yyjson_doc_free(ad);
            }
        }
        char *save = NULL;
        for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char kind[32] = "";
            char f1[300] = "", f2[300] = "", f3[400] = "", f4[400] = "";
            sscanf(line, "%31[^\t]\t%299[^\t]\t%299[^\t]\t%399[^\t]\t%399[^\n]", kind, f1, f2, f3,
                   f4);
            int lang_cs = strcmp(f1, "cs") == 0;
            if (strcmp(kind, "invoke") == 0 || strcmp(kind, "invoke_expr") == 0) {
                const char *raw = f1;
                char folded[512];
                folded[0] = '\0';
                const char *how = "literal";
                double conf = 1.0;
                if (strcmp(kind, "invoke_expr") == 0 || strchr(raw, '(')) {
                    uipath_expr_facts ef;
                    uipath_expr_analyze(raw, 0, vnames, vtypes, nv, anames, atypes, na, 0, &ef);
                    if (ef.nconfig > 0) {
                        const char *val =
                            config_value(keys, nkey, files, nfile, pq, ef.config_keys[0]);
                        if (val && strstr(val, ".xaml")) {
                            snprintf(folded, sizeof(folded), "%s", val);
                            raw = folded;
                            how = "folded";
                            conf = 0.8;
                        }
                    }
                    if (!folded[0]) {
                        int64_t dyn = dyn_target(gb, pq, f1);
                        char ep[200];
                        snprintf(ep, sizeof(ep),
                                 "{\"strategy\":\"uipath_invoke\",\"resolution\":\"dynamic\","
                                 "\"confidence\":0.30,\"line\":%d,\"id_ref\":\"%s\"}",
                                 acts[i].line, js(o, "id_ref"));
                        cbm_gbuf_insert_edge(gb, acts[i].id, dyn, "INVOKES_WORKFLOW", ep);
                        continue;
                    }
                }
                int escaped = 0;
                char resolved[512];
                join_under_root(root, raw, resolved, sizeof(resolved), &escaped);
                up_n *target = find_wf(wfs, nwf, resolved, 0);
                if (!target) {
                    target = find_wf(wfs, nwf, resolved, 1);
                    if (target && strcmp(how, "literal") == 0) {
                        how = "literal_ci";
                        conf = 0.95;
                    }
                }
                if (!target) {
                    char raw_norm[512];
                    snprintf(raw_norm, sizeof(raw_norm), "%s", raw);
                    norm_slash(raw_norm);
                    if (strchr(raw_norm, '/') == NULL) {
                        const char *base = base_name(resolved);
                        int hits = 0;
                        up_n *cand = NULL;
                        for (int w = 0; w < nwf; w++) {
                            if (strcmp(base_name(wfs[w].qn), base) == 0) {
                                hits++;
                                cand = &wfs[w];
                            }
                        }
                        if (hits == 1 && strchr(raw, '(') == NULL) {
                            target = cand;
                            how = "pattern";
                            conf = 0.5;
                        }
                    }
                }
                if (target && escaped && conf > 0.7) {
                    conf = 0.7;
                }
                if (!target) {
                    int64_t dyn = dyn_target(gb, pq, raw);
                    char ep[220];
                    snprintf(ep, sizeof(ep),
                             "{\"strategy\":\"uipath_invoke\",\"resolution\":\"dynamic\","
                             "\"confidence\":0.30,\"line\":%d}",
                             acts[i].line);
                    cbm_gbuf_insert_edge(gb, acts[i].id, dyn, "INVOKES_WORKFLOW", ep);
                } else {
                    char ep[320];
                    snprintf(ep, sizeof(ep),
                             "{\"strategy\":\"uipath_invoke\",\"resolution\":\"%s\","
                             "\"confidence\":%.2f,\"line\":%d%s}",
                             how, conf, acts[i].line,
                             strcmp(how, "pattern") == 0 ? ",\"candidate\":true" : "");
                    cbm_gbuf_insert_edge(gb, acts[i].id, target->id, "INVOKES_WORKFLOW", ep);
                    int is_test = wf && strstr(wf->props, "\"is_test\":true") != NULL;
                    if (wf) {
                        agg_call(&ag, &agn, &agc, wf->id, target->id, conf, how, is_test);
                    }
                }
            } else if (strcmp(kind, "member") == 0) {
                up_n *target = find_member(wfs, nwf, pq, f1);
                if (!target) {
                    int64_t dyn = dyn_target(gb, pq, f1);
                    char ep[200];
                    snprintf(ep, sizeof(ep),
                             "{\"strategy\":\"uipath_invoke\",\"resolution\":\"dynamic\","
                             "\"confidence\":0.30,\"line\":%d}",
                             acts[i].line);
                    cbm_gbuf_insert_edge(gb, acts[i].id, dyn, "INVOKES_WORKFLOW", ep);
                } else if (wf) {
                    char ep[200];
                    snprintf(ep, sizeof(ep),
                             "{\"strategy\":\"uipath_invoke\",\"resolution\":\"literal\","
                             "\"confidence\":1.00,\"line\":%d}",
                             acts[i].line);
                    cbm_gbuf_insert_edge(gb, acts[i].id, target->id, "INVOKES_WORKFLOW", ep);
                    agg_call(&ag, &agn, &agc, wf->id, target->id, 1, "literal",
                             strstr(wf->props, "\"is_test\":true") != NULL);
                }
            } else if (strcmp(kind, "bind") == 0) {
                /* f1 dir, f2 name, f3 expr. The invoked workflow is not known on this
                 * fact line; argument binding is matched after the invoke edge. */
                (void)f1;
            } else if (strcmp(kind, "expr") == 0 || strcmp(kind, "write") == 0) {
                int cs = strcmp(f1, "cs") == 0;
                uipath_expr_facts ef;
                memset(&ef, 0, sizeof(ef));
                if (strcmp(kind, "write") == 0) {
                    /* write <lang> <to> <value> */
                    if (f2[0]) {
                        uipath_expr_analyze(f2, cs, vnames, vtypes, nv, anames, atypes, na, 1, &ef);
                    }
                    if (f3[0]) {
                        uipath_expr_facts rd;
                        uipath_expr_analyze(f3, cs, vnames, vtypes, nv, anames, atypes, na, 0, &rd);
                        for (int r = 0; r < rd.nreads && ef.nreads < 12; r++) {
                            snprintf(ef.reads[ef.nreads], sizeof(ef.reads[0]), "%s", rd.reads[r]);
                            ef.nreads++;
                        }
                        for (int c = 0; c < rd.nconfig && ef.nconfig < 8; c++) {
                            snprintf(ef.config_keys[ef.nconfig], sizeof(ef.config_keys[0]), "%s",
                                     rd.config_keys[c]);
                            ef.nconfig++;
                        }
                    }
                } else {
                    uipath_expr_analyze(f2, cs, vnames, vtypes, nv, anames, atypes, na, 0, &ef);
                }
                for (int r = 0; r < ef.nreads; r++) {
                    for (int v = 0; v < nvar; v++) {
                        if (strcmp(vars[v].file, acts[i].file) == 0 &&
                            strcasecmp(vars[v].name, ef.reads[r]) == 0) {
                            char ep[120];
                            snprintf(ep, sizeof(ep),
                                     "{\"strategy\":\"uipath_expr\",\"lang\":\"%s\",\"line\":%d}",
                                     cs ? "cs" : "vb", acts[i].line);
                            cbm_gbuf_insert_edge(gb, acts[i].id, vars[v].id, "READS", ep);
                        }
                    }
                    for (int a = 0; a < narg; a++) {
                        if (strcmp(args[a].file, acts[i].file) == 0 &&
                            strcasecmp(args[a].name, ef.reads[r]) == 0) {
                            char ep[120];
                            snprintf(ep, sizeof(ep),
                                     "{\"strategy\":\"uipath_expr\",\"lang\":\"%s\",\"line\":%d}",
                                     cs ? "cs" : "vb", acts[i].line);
                            cbm_gbuf_insert_edge(gb, acts[i].id, args[a].id, "READS", ep);
                        }
                    }
                }
                for (int r = 0; r < ef.nwrites; r++) {
                    for (int v = 0; v < nvar; v++) {
                        if (strcmp(vars[v].file, acts[i].file) == 0 &&
                            strcasecmp(vars[v].name, ef.writes[r]) == 0) {
                            char ep[120];
                            snprintf(ep, sizeof(ep), "{\"strategy\":\"uipath_expr\",\"line\":%d}",
                                     acts[i].line);
                            cbm_gbuf_insert_edge(gb, acts[i].id, vars[v].id, "WRITES", ep);
                        }
                    }
                    for (int a = 0; a < narg; a++) {
                        if (strcmp(args[a].file, acts[i].file) == 0 &&
                            strcasecmp(args[a].name, ef.writes[r]) == 0) {
                            char ep[120];
                            snprintf(ep, sizeof(ep), "{\"strategy\":\"uipath_expr\",\"line\":%d}",
                                     acts[i].line);
                            cbm_gbuf_insert_edge(gb, acts[i].id, args[a].id, "WRITES", ep);
                        }
                    }
                }
                for (int c = 0; c < ef.nconfig; c++) {
                    for (int k = 0; k < nkey; k++) {
                        yyjson_doc *kd = NULL;
                        yyjson_val *ko = props_of(&kd, keys[k].props);
                        int match = strcmp(js(ko, "lookup_key"), ef.config_keys[c]) == 0 &&
                                    strcmp(js(ko, "project_qn"), pq) == 0 &&
                                    file_loaded(files, nfile, keys[k].file);
                        if (match) {
                            char ep[160];
                            snprintf(
                                ep, sizeof(ep),
                                "{\"strategy\":\"uipath_config\",\"confidence\":1.00,\"line\":%d}",
                                acts[i].line);
                            cbm_gbuf_insert_edge(gb, acts[i].id, keys[k].id, "READS_CONFIG", ep);
                        }
                        if (kd) {
                            yyjson_doc_free(kd);
                        }
                    }
                }
                (void)lang_cs;
            } else if (strcmp(kind, "sel") == 0) {
                char sha[CBM_SHA256_HEX_LEN + 1];
                const char *text = f4[0] ? f4 : f3;
                cbm_sha256_hex(text, strlen(text), sha);
                sha[12] = '\0';
                char qn[200];
                snprintf(qn, sizeof(qn), "%s:sel:%s", pq[0] ? pq : "uipath:.", sha);
                char e[200];
                jesc(e, sizeof(e), f3);
                char props[400];
                snprintf(props, sizeof(props),
                         "{\"domain\":\"uipath\",\"kind\":\"%s\",\"risk_score\":%d,"
                         "\"risk_reasons\":\"%s\",\"strategy\":\"uipath_selector\"}",
                         f1, atoi(f2), e);
                int64_t sid = up_upsert(gb, "Selector", sha, qn, acts[i].file, acts[i].line,
                                        acts[i].line, props);
                char ep[120];
                snprintf(ep, sizeof(ep), "{\"strategy\":\"uipath_selector\",\"line\":%d}",
                         acts[i].line);
                cbm_gbuf_insert_edge(gb, acts[i].id, sid, "SELECTS_UI_ELEMENT", ep);
            } else if (strcmp(kind, "asset") == 0 || strcmp(kind, "queue") == 0) {
                const char *op = f1;
                const char *rname = f2;
                const char *via = f3[0] ? f3 : "literal";
                char literal[160];
                snprintf(literal, sizeof(literal), "%s", rname);
                if (rname[0] == '[' || strchr(rname, '(')) {
                    uipath_expr_facts ef;
                    uipath_expr_analyze(rname, 0, vnames, vtypes, nv, anames, atypes, na, 0, &ef);
                    via = "config";
                    if (ef.nconfig) {
                        snprintf(literal, sizeof(literal), "%s", ef.config_keys[0]);
                        const char *val =
                            config_value(keys, nkey, files, nfile, pq, ef.config_keys[0]);
                        if (val && val[0] && strcmp(val, "[redacted]") != 0) {
                            snprintf(literal, sizeof(literal), "%s", val);
                            via = "config";
                        }
                    }
                }
                char qn[400];
                const char *lab = strcmp(kind, "asset") == 0 ? "Asset" : "Queue";
                snprintf(qn, sizeof(qn), "%s:%s:%s", pq[0] ? pq : "uipath:.",
                         strcmp(kind, "asset") == 0 ? "asset" : "queue", literal);
                char props[240];
                snprintf(props, sizeof(props),
                         "{\"domain\":\"uipath\",\"strategy\":\"uipath_resource\",\"project_qn\":"
                         "\"%s\"}",
                         pq);
                int64_t id = up_upsert(gb, lab, literal, qn, "", 0, 0, props);
                char ep[200];
                snprintf(ep, sizeof(ep),
                         "{\"strategy\":\"uipath_resource\",\"op\":\"%s\",\"via\":\"%s\","
                         "\"confidence\":%.2f,\"line\":%d}",
                         op, via, strcmp(via, "literal") == 0 ? 1.0 : 0.8, acts[i].line);
                const char *et = strcmp(kind, "asset") == 0   ? "USES_ASSET"
                                 : strcmp(op, "dequeue") == 0 ? "DEQUEUES"
                                                              : "ENQUEUES";
                cbm_gbuf_insert_edge(gb, acts[i].id, id, et, ep);
            } else if (strcmp(kind, "res") == 0) {
                char qn[400];
                snprintf(qn, sizeof(qn), "%s:res:%s:%s", pq[0] ? pq : "uipath:.", f1, f2);
                char props[200];
                snprintf(props, sizeof(props),
                         "{\"domain\":\"uipath\",\"kind\":\"%s\",\"strategy\":\"uipath_resource\"}",
                         f1);
                int64_t id = up_upsert(gb, "OrchestratorResource", f2, qn, "", 0, 0, props);
                char ep[120];
                snprintf(ep, sizeof(ep), "{\"strategy\":\"uipath_resource\",\"line\":%d}",
                         acts[i].line);
                cbm_gbuf_insert_edge(gb, acts[i].id, id, "USES_RESOURCE", ep);
            }
        }
        /* Second walk for bindings once invokes are known would need the target.
         * Re-scan facts for bind lines and attach them to args of every invoked
         * workflow recorded as INVOKES from this activity — done below from edges
         * is harder. Handle bind lines against the literal invoke target in the
         * same facts blob. */
        char *copy2 = strdup(facts);
        if (copy2) {
            char invoked[512] = "";
            char *sv = NULL;
            for (char *line = strtok_r(copy2, "\n", &sv); line; line = strtok_r(NULL, "\n", &sv)) {
                if (strncmp(line, "invoke\t", 7) == 0 && strchr(line, '(') == NULL) {
                    int escaped = 0;
                    join_under_root(root, line + 7, invoked, sizeof(invoked), &escaped);
                    up_n *tw = find_wf(wfs, nwf, invoked, 0);
                    if (!tw) {
                        tw = find_wf(wfs, nwf, invoked, 1);
                    }
                    if (tw) {
                        snprintf(invoked, sizeof(invoked), "%s", tw->qn);
                    }
                }
            }
            free(copy2);
            if (invoked[0]) {
                char *copy3 = strdup(facts);
                char *sv3 = NULL;
                for (char *line = strtok_r(copy3, "\n", &sv3); line;
                     line = strtok_r(NULL, "\n", &sv3)) {
                    if (strncmp(line, "bind\t", 5) != 0) {
                        continue;
                    }
                    char dir[24] = "", nm[128] = "", expr[240] = "";
                    sscanf(line, "bind\t%23[^\t]\t%127[^\t]\t%239[^\n]", dir, nm, expr);
                    char aqn[700];
                    snprintf(aqn, sizeof(aqn), "%s#arg:%s", invoked, nm);
                    up_n *arg = find_qn(args, narg, aqn);
                    if (!arg) {
                        for (int a = 0; a < narg; a++) {
                            if (strcasecmp(args[a].name, nm) == 0 &&
                                strncmp(args[a].qn, invoked, strlen(invoked)) == 0) {
                                arg = &args[a];
                                break;
                            }
                        }
                    }
                    if (arg) {
                        const char *status = "ok";
                        yyjson_doc *ad = NULL;
                        yyjson_val *ao = props_of(&ad, arg->props);
                        const char *adir = js(ao, "direction");
                        if (adir[0] && dir[0] && strcmp(adir, dir) != 0) {
                            status = "direction_mismatch";
                        }
                        char ep[300];
                        char eexpr[200];
                        jesc(eexpr, sizeof(eexpr), expr);
                        snprintf(ep, sizeof(ep),
                                 "{\"strategy\":\"uipath_binding\",\"direction\":\"%s\","
                                 "\"binding\":\"%s\",\"status\":\"%s\",\"line\":%d}",
                                 dir, eexpr, status, acts[i].line);
                        cbm_gbuf_insert_edge(gb, acts[i].id, arg->id, "PASSES_ARGUMENT", ep);
                        if (ad) {
                            yyjson_doc_free(ad);
                        }
                    }
                }
                /* missing In args */
                size_t ilen = strlen(invoked);
                for (int a = 0; a < narg; a++) {
                    if (strncmp(args[a].qn, invoked, ilen) != 0) {
                        continue;
                    }
                    yyjson_doc *ad = NULL;
                    yyjson_val *ao = props_of(&ad, args[a].props);
                    if (strcmp(js(ao, "direction"), "In") != 0) {
                        if (ad) {
                            yyjson_doc_free(ad);
                        }
                        continue;
                    }
                    int bound = 0;
                    char *copy4 = strdup(facts);
                    char *sv4 = NULL;
                    for (char *line = copy4 ? strtok_r(copy4, "\n", &sv4) : NULL; line;
                         line = strtok_r(NULL, "\n", &sv4)) {
                        char nm[128] = "";
                        sscanf(line, "bind\t%*23[^\t]\t%127[^\t]", nm);
                        if (strcasecmp(nm, args[a].name) == 0) {
                            bound = 1;
                        }
                    }
                    free(copy4);
                    if (!bound) {
                        char ep[200];
                        snprintf(ep, sizeof(ep),
                                 "{\"strategy\":\"uipath_binding\",\"status\":\"missing\","
                                 "\"direction\":\"In\",\"line\":%d}",
                                 acts[i].line);
                        cbm_gbuf_insert_edge(gb, acts[i].id, args[a].id, "PASSES_ARGUMENT", ep);
                    }
                    if (ad) {
                        yyjson_doc_free(ad);
                    }
                }
                free(copy3);
            }
        }
        for (int v = 0; v < nv; v++) {
            free((void *)vtypes[v]);
        }
        for (int a = 0; a < na; a++) {
            free((void *)atypes[a]);
        }
        free(copy);
        yyjson_doc_free(d);
    }
    for (int i = 0; i < narg; i++) {
        up_n *wf = find_qn(wfs, nwf, args[i].file);
        if (wf) {
            char ep[80];
            snprintf(ep, sizeof(ep), "{\"strategy\":\"uipath_structure\"}");
            cbm_gbuf_insert_edge(gb, wf->id, args[i].id, "DEFINES", ep);
        }
    }
    for (int i = 0; i < nvar; i++) {
        yyjson_doc *vd = NULL;
        yyjson_val *vo = props_of(&vd, vars[i].props);
        const char *pid = js(vo, "parent_id");
        int64_t src = 0;
        if (pid[0]) {
            char pqn[800];
            snprintf(pqn, sizeof(pqn), "%s#%s", vars[i].file, pid);
            up_n *pn = find_qn(acts, nact, pqn);
            if (pn) {
                src = pn->id;
            }
        }
        if (!src) {
            up_n *wf = find_qn(wfs, nwf, vars[i].file);
            if (wf) {
                src = wf->id;
            }
        }
        if (src) {
            char ep[80];
            snprintf(ep, sizeof(ep), "{\"strategy\":\"uipath_structure\"}");
            cbm_gbuf_insert_edge(gb, src, vars[i].id, "DEFINES", ep);
        }
        if (vd) {
            yyjson_doc_free(vd);
        }
    }
    for (int i = 0; i < agn; i++) {
        char ep[200];
        snprintf(ep, sizeof(ep),
                 "{\"strategy\":\"uipath_invoke\",\"sites\":%d,\"confidence\":%.2f,"
                 "\"resolution\":\"%s\"}",
                 ag[i].sites, ag[i].conf, ag[i].how);
        cbm_gbuf_insert_edge(gb, ag[i].src, ag[i].dst, "CALLS", ep);
        if (ag[i].is_test) {
            char tp[80];
            snprintf(tp, sizeof(tp), "{\"strategy\":\"uipath_test\"}");
            cbm_gbuf_insert_edge(gb, ag[i].src, ag[i].dst, "TESTS", tp);
        }
    }
    free(ag);
    /* Packages, custom activities, config resolves, REFramework. */
    for (int p = 0; p < nproj; p++) {
        yyjson_doc *d = NULL;
        yyjson_val *o = props_of(&d, projs[p].props);
        const char *deps = js(o, "dependencies");
        char *copy = deps[0] ? strdup(deps) : NULL;
        if (copy) {
            char *sv = NULL;
            for (char *tok = strtok_r(copy, "|", &sv); tok; tok = strtok_r(NULL, "|", &sv)) {
                char *eq = strchr(tok, '=');
                if (eq) {
                    *eq = '\0';
                }
                char pqn[200];
                snprintf(pqn, sizeof(pqn), "nuget:%s", tok);
                up_n *pkg = NULL;
                /* id was stored as name; find by qn via upsert return */
                int64_t id = up_upsert(gb, "Package", tok, pqn, projs[p].file, 1, 1,
                                       "{\"domain\":\"uipath\",\"ecosystem\":\"nuget\","
                                       "\"strategy\":\"uipath_pkg\"}");
                char ep[80];
                snprintf(ep, sizeof(ep), "{\"strategy\":\"uipath_pkg\"}");
                cbm_gbuf_insert_edge(gb, projs[p].id, id, "DEPENDS_ON", ep);
                (void)pkg;
            }
            free(copy);
        }
        int re = 0;
        for (int w = 0; w < nwf; w++) {
            if (strstr(wfs[w].qn, "InitAllSettings.xaml")) {
                re = 1;
            }
        }
        for (int a = 0; a < nact && !re; a++) {
            if (strcmp(acts[a].name, "Get Transaction Data") == 0 ||
                strcmp(acts[a].name, "Process Transaction") == 0) {
                re = 1;
            }
        }
        if (re) {
            char *np = malloc(strlen(projs[p].props) + 32);
            if (np) {
                size_t L = strlen(projs[p].props);
                if (L && projs[p].props[L - 1] == '}') {
                    snprintf(np, L + 32, "%.*s,\"framework\":\"REFramework\"}", (int)(L - 1),
                             projs[p].props);
                } else {
                    snprintf(np, L + 32, "{\"framework\":\"REFramework\"}");
                }
                up_upsert(gb, "UiPathProject", projs[p].name, projs[p].qn, projs[p].file, 1, 1, np);
                free(np);
            }
        }
        if (d) {
            yyjson_doc_free(d);
        }
    }
    for (int k = 0; k < nkey; k++) {
        yyjson_doc *d = NULL;
        yyjson_val *o = props_of(&d, keys[k].props);
        const char *asset = js(o, "asset_name");
        const char *folder = js(o, "asset_folder");
        const char *pq = js(o, "project_qn");
        if (asset[0] && file_loaded(files, nfile, keys[k].file)) {
            char qn[400];
            snprintf(qn, sizeof(qn), "%s:asset:%s%s%s", pq[0] ? pq : "uipath:.",
                     folder[0] ? folder : "", folder[0] ? "/" : "", asset);
            char props[160];
            snprintf(props, sizeof(props),
                     "{\"domain\":\"uipath\",\"strategy\":\"uipath_resource\"}");
            int64_t id = up_upsert(gb, "Asset", asset, qn, "", 0, 0, props);
            char ep[80];
            snprintf(ep, sizeof(ep), "{\"strategy\":\"uipath_config\"}");
            cbm_gbuf_insert_edge(gb, keys[k].id, id, "RESOLVES_TO", ep);
        }
        if (d) {
            yyjson_doc_free(d);
        }
    }
    for (int c = 0; c < ncls; c++) {
        if (!clss[c].props ||
            (!strstr(clss[c].props, "CodeActivity") && !strstr(clss[c].props, "NativeActivity") &&
             !strstr(clss[c].props, "AsyncCodeActivity"))) {
            continue;
        }
        for (int a = 0; a < nact; a++) {
            yyjson_doc *d = NULL;
            yyjson_val *o = props_of(&d, acts[a].props);
            const char *clr = js(o, "clr_type");
            const char *aty = js(o, "activity_type");
            int match =
                (clr[0] && strstr(clr, clss[c].name)) || (aty[0] && strcmp(aty, clss[c].name) == 0);
            if (match) {
                char tqn[300];
                snprintf(tqn, sizeof(tqn), "clr:%s", clr[0] ? clr : aty);
                int64_t tid =
                    up_upsert(gb, "ActivityType", aty[0] ? aty : clss[c].name, tqn, "", 0, 0,
                              "{\"domain\":\"uipath\",\"source\":\"in_repo\","
                              "\"strategy\":\"uipath_type\"}");
                char ep[80];
                snprintf(ep, sizeof(ep), "{\"strategy\":\"uipath_impl\"}");
                cbm_gbuf_insert_edge(gb, tid, clss[c].id, "IMPLEMENTED_BY", ep);
            }
            if (d) {
                yyjson_doc_free(d);
            }
        }
    }
    for (int w = 0; w < nwf; w++) {
        if (!strstr(wfs[w].props, "\"kind\":\"coded\"") &&
            !strstr(wfs[w].props, "\"kind\":\"test_case\"")) {
            continue;
        }
        for (int m = 0; m < nmeth; m++) {
            if (strcmp(meths[m].file, wfs[w].file) == 0 && strcmp(meths[m].name, "Execute") == 0) {
                char ep[80];
                snprintf(ep, sizeof(ep), "{\"strategy\":\"uipath_impl\"}");
                cbm_gbuf_insert_edge(gb, wfs[w].id, meths[m].id, "IMPLEMENTED_BY", ep);
            }
        }
    }
    (void)meths;
    uipath_emit_similarity(gb);
    free_nodes(wfs, nwf);
    free_nodes(acts, nact);
    free_nodes(args, narg);
    free_nodes(vars, nvar);
    free_nodes(keys, nkey);
    free_nodes(files, nfile);
    free_nodes(clss, ncls);
    free_nodes(meths, nmeth);
    free_nodes(projs, nproj);
}

static void scan_xaml_file(cbm_pipeline_ctx_t *ctx, const char *rel, const char *abs,
                           up_proj *proj) {
    int trunc = 0;
    size_t len = 0;
    char *src = read_cap(abs, 32 * 1024 * 1024, &len, &trunc);
    if (!src) {
        return;
    }
    if (!strstr(src, "schemas.microsoft.com/netfx/2009/xaml/activities")) {
        free(src);
        return;
    }
    scan_ud ud;
    memset(&ud, 0, sizeof(ud));
    ud.gb = ctx->gbuf;
    ud.wf = rel;
    ud.project_qn = proj ? proj->qn : "";
    ud.expr_proj = proj ? proj->expr : "VisualBasic";
    ud.pipeline = ctx->pipeline;
    ud.is_entry = proj && list_has(proj->entries, rel);
    ud.is_test = (proj && list_has(proj->tests, rel)) || strstr(rel, "/Tests/") != NULL ||
                 strstr(rel, "/Test/") != NULL;
    ud.is_private = proj && list_has(proj->privates, rel);
    if (trunc) {
        ud.parse_status = 2;
    }
    /* placeholder so the QN exists even if the scanner emits nothing */
    up_upsert(ctx->gbuf, "Workflow", base_name(rel), rel, rel, 1, 1,
              "{\"domain\":\"uipath\",\"kind\":\"xaml\",\"strategy\":\"uipath_structure\"}");
    uipath_xaml_scan(src, len, on_xaml_item, &ud);
    if (trunc) {
        ud.parse_status = 2;
    }
    finish_workflow_node(&ud, ud.is_test ? "test_case" : "xaml");
    free(src);
}

bool cbm_uipath_config_binaries_dirty(const char *repo, cbm_store_t *store, const char *project) {
    if (!repo || !store || !project) {
        return false;
    }
    sqlite3 *db = cbm_store_get_db(store);
    if (!db) {
        return false;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(
            db, "SELECT qualified_name FROM nodes WHERE project=?1 AND label='UiPathProject'", -1,
            &st, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
    int any = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        any = 1;
        const char *qn = (const char *)sqlite3_column_text(st, 0);
        const char *root = qn && strncmp(qn, "uipath:", 7) == 0 ? qn + 7 : "";
        if (strcmp(root, ".") == 0) {
            root = "";
        }
        const char *rels[] = {"Data/Config.xlsx", "Config.xlsx"};
        for (int i = 0; i < 2; i++) {
            char relp[700];
            if (root[0]) {
                snprintf(relp, sizeof(relp), "%s/%s", root, rels[i]);
            } else {
                snprintf(relp, sizeof(relp), "%s", rels[i]);
            }
            char abs[1100];
            snprintf(abs, sizeof(abs), "%s/%s", repo, relp);
            struct stat fst;
            int exists = stat(abs, &fst) == 0;
            sqlite3_stmt *ns = NULL;
            if (sqlite3_prepare_v2(
                    db,
                    "SELECT properties FROM nodes WHERE project=?1 AND label='ConfigFile' "
                    "AND file_path=?2",
                    -1, &ns, NULL) != SQLITE_OK) {
                continue;
            }
            sqlite3_bind_text(ns, 1, project, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ns, 2, relp, -1, SQLITE_TRANSIENT);
            int found = sqlite3_step(ns) == SQLITE_ROW;
            if (exists && !found) {
                sqlite3_finalize(ns);
                sqlite3_finalize(st);
                return true;
            }
            if (exists && found) {
                const char *props = (const char *)sqlite3_column_text(ns, 0);
                yyjson_doc *doc = props ? yyjson_read(props, strlen(props), 0) : NULL;
                yyjson_val *o = doc ? yyjson_doc_get_root(doc) : NULL;
                yyjson_val *mt = o ? yyjson_obj_get(o, "mtime") : NULL;
                yyjson_val *sz = o ? yyjson_obj_get(o, "size") : NULL;
                long om = mt && yyjson_is_int(mt) ? (long)yyjson_get_sint(mt) : -1;
                long os = sz && yyjson_is_int(sz) ? (long)yyjson_get_sint(sz) : -1;
                if (doc) {
                    yyjson_doc_free(doc);
                }
                if (om != (long)fst.st_mtime || os != (long)fst.st_size) {
                    sqlite3_finalize(ns);
                    sqlite3_finalize(st);
                    return true;
                }
            }
            sqlite3_finalize(ns);
        }
    }
    sqlite3_finalize(st);
    (void)any;
    return false;
}

int cbm_pipeline_pass_uipath(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                             int file_count) {
    if (!ctx || !ctx->gbuf) {
        return 0;
    }
    up_proj *projs = calloc(32, sizeof(up_proj));
    int np = 0;
    if (!projs) {
        return 0;
    }
    const cbm_gbuf_node_t **existing = NULL;
    int en = 0;
    if (cbm_gbuf_find_by_label(ctx->gbuf, "UiPathProject", &existing, &en) == 0) {
        for (int i = 0; i < en && np < 32; i++) {
            const char *qn = existing[i]->qualified_name ? existing[i]->qualified_name : "";
            if (strncmp(qn, "uipath:", 7) != 0) {
                continue;
            }
            const char *root = qn + 7;
            if (strcmp(root, ".") == 0) {
                root = "";
            }
            snprintf(projs[np].root, sizeof(projs[np].root), "%s", root);
            snprintf(projs[np].qn, sizeof(projs[np].qn), "%s", qn);
            snprintf(projs[np].name, sizeof(projs[np].name), "%s",
                     existing[i]->name ? existing[i]->name : "UiPath");
            yyjson_doc *d = NULL;
            yyjson_val *o = props_of(&d, existing[i]->properties_json);
            snprintf(projs[np].expr, sizeof(projs[np].expr), "%s",
                     js(o, "expression_language")[0] ? js(o, "expression_language")
                                                     : "VisualBasic");
            snprintf(projs[np].entries, sizeof(projs[np].entries), "%s", js(o, "entries"));
            snprintf(projs[np].main_path, sizeof(projs[np].main_path), "%s", js(o, "main"));
            if (projs[np].main_path[0]) {
                proj_add_list(projs[np].entries, sizeof(projs[np].entries), projs[np].main_path);
            }
            if (d) {
                yyjson_doc_free(d);
            }
            np++;
        }
    }
    for (int i = 0; i < file_count; i++) {
        const char *rel = files[i].rel_path;
        if (!rel || is_dot_local(rel)) {
            continue;
        }
        if (strcmp(base_name(rel), "project.json") != 0) {
            continue;
        }
        int trunc = 0;
        size_t len = 0;
        char *text = read_cap(files[i].path, 2 * 1024 * 1024, &len, &trunc);
        if (!text) {
            continue;
        }
        if (text_is_uipath_project(text) && np < 32) {
            const char *slash = strrchr(rel, '/');
            char root[512];
            if (slash) {
                size_t n = (size_t)(slash - rel);
                if (n >= sizeof(root)) {
                    n = sizeof(root) - 1;
                }
                memcpy(root, rel, n);
                root[n] = '\0';
            } else {
                root[0] = '\0';
            }
            int idx = proj_index(projs, np, root);
            if (idx < 0) {
                idx = np++;
                memset(&projs[idx], 0, sizeof(projs[idx]));
            }
            parse_project_json(ctx->gbuf, &projs[idx], rel, text);
        }
        free(text);
    }
    if (np == 0) {
        free(projs);
        return 0;
    }
    ctx->uipath_active = true;
    for (int i = 0; i < file_count; i++) {
        if (cbm_pipeline_check_cancel(ctx)) {
            free(projs);
            return 0;
        }
        const char *rel = files[i].rel_path;
        if (!rel || is_dot_local(rel)) {
            continue;
        }
        const char *bn = base_name(rel);
        up_proj *own = owning_proj(projs, np, rel);
        size_t bl = strlen(bn);
        if (bl > 5 && strcmp(bn + bl - 5, ".xaml") == 0) {
            scan_xaml_file(ctx, rel, files[i].path, own);
        } else if (bl > 3 && strcmp(bn + bl - 3, ".cs") == 0) {
            int trunc = 0;
            size_t len = 0;
            char *src = read_cap(files[i].path, 4 * 1024 * 1024, &len, &trunc);
            if (src) {
                scan_coded(ctx->gbuf, ctx->pipeline, rel, src, own);
                free(src);
            }
        } else if ((bl > 5 && strcmp(bn + bl - 5, ".json") == 0 && strncmp(bn, "Config", 6) == 0) ||
                   (bl > 7 && strcmp(bn + bl - 7, ".config") == 0)) {
            uipath_config_index_file(ctx->gbuf, files[i].path, rel, own ? own->qn : "");
        } else if (strstr(rel, ".objects/") && bl > 5 && strcmp(bn + bl - 5, ".json") == 0) {
            char props[160];
            snprintf(props, sizeof(props),
                     "{\"domain\":\"uipath\",\"strategy\":\"uipath_structure\",\"spike\":true}");
            up_upsert(ctx->gbuf, "ObjectRepositoryElement", bn, rel, rel, 1, 1, props);
        } else if (strstr(rel, ".entities/EntitiesStore.json")) {
            char qn[700];
            snprintf(qn, sizeof(qn), "%s:entity:%s", own ? own->qn : "uipath:.", rel);
            char props[160];
            snprintf(
                props, sizeof(props),
                "{\"domain\":\"uipath\",\"kind\":\"entity\",\"strategy\":\"uipath_resource\"}");
            up_upsert(ctx->gbuf, "OrchestratorResource", "EntitiesStore", qn, rel, 1, 1, props);
        }
    }
    if (ctx->repo_path) {
        for (int i = 0; i < np; i++) {
            index_known_configs(ctx->gbuf, ctx->repo_path, &projs[i]);
        }
    }
    cbm_gbuf_delete_edges_with_property_needle(ctx->gbuf, "\"strategy\":\"uipath");
    link_all(ctx->gbuf);
    cbm_log_info("pass.done", "pass", "uipath", "projects", "1");
    free(projs);
    return 0;
}
