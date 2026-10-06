#include "mcp/uipath_mcp.h"

#include "mcp/compact_out.h"
#include "mcp/mcp.h"
#include "store/store.h"
#include "yyjson/yyjson.h"

#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *arg_str(const char *args, const char *key) {
    return cbm_mcp_get_string_arg(args, key);
}

static int arg_int(const char *args, const char *key, int def) {
    return cbm_mcp_get_int_arg(args, key, def);
}

static char *finish(cbm_sb_t *sb, int is_error) {
    char *text = cbm_sb_finish(sb);
    if (!text) {
        return cbm_mcp_text_result("uipath output allocation failed", true);
    }
    char *result = cbm_mcp_text_result(text, is_error);
    free(text);
    return result;
}

static int count_label(sqlite3 *db, const char *project, const char *label) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM nodes WHERE project=?1 AND label=?2", -1, &st,
                           NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, label, -1, SQLITE_TRANSIENT);
    int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return n;
}

static int count_edge(sqlite3 *db, const char *project, const char *type) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT COUNT(*) FROM edges e JOIN nodes n ON n.id=e.source_id "
        "WHERE n.project=?1 AND e.type=?2 AND instr(e.properties, '\"strategy\":\"uipath')>0";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, type, -1, SQLITE_TRANSIENT);
    int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return n;
}

static const char *coltxt(sqlite3_stmt *st, int i) {
    const char *s = (const char *)sqlite3_column_text(st, i);
    return s ? s : "";
}

static char *tool_overview(sqlite3 *db, const char *project) {
    cbm_sb_t sb;
    cbm_sb_init(&sb);
    cbm_tree_scalar_str(&sb, "project", project);
    int projects = count_label(db, project, "UiPathProject");
    cbm_tree_scalar_int(&sb, "uipath_projects", projects);
    cbm_tree_scalar_int(&sb, "workflows", count_label(db, project, "Workflow"));
    cbm_tree_scalar_int(&sb, "activities", count_label(db, project, "Activity"));
    cbm_tree_scalar_int(&sb, "arguments", count_label(db, project, "Argument"));
    cbm_tree_scalar_int(&sb, "config_keys", count_label(db, project, "ConfigKey"));
    cbm_tree_scalar_int(&sb, "assets", count_label(db, project, "Asset"));
    cbm_tree_scalar_int(&sb, "queues", count_label(db, project, "Queue"));
    cbm_tree_scalar_int(&sb, "dynamic_targets", count_label(db, project, "DynamicTarget"));
    cbm_tree_scalar_int(&sb, "invokes", count_edge(db, project, "INVOKES_WORKFLOW"));
    cbm_tree_scalar_int(&sb, "calls", count_edge(db, project, "CALLS"));
    int dyn = count_label(db, project, "DynamicTarget");
    int broken = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM nodes WHERE project=?1 AND label='Workflow' "
                           "AND instr(properties,'\"parse_status\":\"truncated\"')>0",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            broken = sqlite3_column_int(st, 0);
        }
        sqlite3_finalize(st);
    }
    cbm_tree_scalar_str(&sb, "completeness", dyn > 0 || broken > 0 ? "lower_bound" : "exact");
    cbm_tree_scalar_int(&sb, "coverage_truncated_workflows", broken);
    if (sqlite3_prepare_v2(db,
                           "SELECT name, qualified_name, properties FROM nodes "
                           "WHERE project=?1 AND label='UiPathProject' ORDER BY qualified_name",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        int rows = 0;
        cbm_sb_t body;
        cbm_sb_init(&body);
        while (sqlite3_step(st) == SQLITE_ROW && rows < 32) {
            const char *props = coltxt(st, 2);
            const char *fw = "unknown";
            if (strstr(props, "REFramework")) {
                fw = "REFramework";
            }
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, coltxt(st, 0), true);
            cbm_tree_cell_str(&body, coltxt(st, 1), false);
            cbm_tree_cell_str(&body, fw, false);
            cbm_tree_row_end(&body);
            rows++;
        }
        sqlite3_finalize(st);
        if (rows) {
            static const char *cols[] = {"name", "qn", "framework"};
            cbm_tree_table_header(&sb, "projects", rows, cols, 3);
            cbm_sb_append(&sb, body.buf ? body.buf : "");
        }
        cbm_sb_free(&body);
    }
    return finish(&sb, 0);
}

static char *tool_outline(sqlite3 *db, const char *project, const char *args) {
    char *wf = arg_str(args, "workflow");
    if (!wf || !wf[0]) {
        free(wf);
        return cbm_mcp_text_result("workflow is required", true);
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT name, properties, start_line, end_line FROM nodes "
                           "WHERE project=?1 AND label='Workflow' AND "
                           "(qualified_name=?2 OR file_path=?2) LIMIT 1",
                           -1, &st, NULL) != SQLITE_OK) {
        free(wf);
        return cbm_mcp_text_result("outline query failed", true);
    }
    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, wf, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        free(wf);
        return cbm_mcp_text_result("workflow not found", true);
    }
    cbm_sb_t sb;
    cbm_sb_init(&sb);
    cbm_tree_scalar_str(&sb, "workflow", wf);
    cbm_tree_scalar_str(&sb, "name", coltxt(st, 0));
    cbm_tree_scalar_int(&sb, "start_line", sqlite3_column_int(st, 2));
    cbm_tree_scalar_int(&sb, "end_line", sqlite3_column_int(st, 3));
    const char *props = coltxt(st, 1);
    cbm_tree_scalar_str(&sb, "signature", strstr(props, "\"signature\"") ? "present" : "");
    if (strstr(props, "REFramework") || strstr(props, "\"role\"")) {
        cbm_tree_scalar_str(&sb, "framework_note", "see project framework");
    }
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db,
                           "SELECT name, qualified_name, start_line FROM nodes "
                           "WHERE project=?1 AND label='Activity' AND file_path=?2 "
                           "ORDER BY start_line, qualified_name LIMIT 80",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, wf, -1, SQLITE_TRANSIENT);
        cbm_sb_t body;
        cbm_sb_init(&body);
        int rows = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            char line[16];
            snprintf(line, sizeof(line), "%d", sqlite3_column_int(st, 2));
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, line, true);
            cbm_tree_cell_str(&body, coltxt(st, 1), false);
            cbm_tree_cell_str(&body, coltxt(st, 0), false);
            cbm_tree_row_end(&body);
            rows++;
        }
        sqlite3_finalize(st);
        if (rows) {
            static const char *cols[] = {"line", "id", "name"};
            cbm_tree_table_header(&sb, "outline", rows, cols, 3);
            cbm_sb_append(&sb, body.buf ? body.buf : "");
        }
        cbm_sb_free(&body);
    }
    int dyn = count_label(db, project, "DynamicTarget");
    cbm_tree_scalar_str(&sb, "completeness", dyn > 0 ? "lower_bound" : "exact");
    free(wf);
    return finish(&sb, 0);
}

enum { UIP_FACT_CAP = 12 };

static int activity_fact_kind(const char *line, char *kind, size_t cap) {
    const char *tab = strchr(line, '\t');
    size_t n = tab ? (size_t)(tab - line) : strlen(line);
    if (n == 0 || n >= cap) {
        return 0;
    }
    memcpy(kind, line, n);
    kind[n] = '\0';
    return strcmp(kind, "invoke") == 0 || strcmp(kind, "invoke_expr") == 0 ||
           strcmp(kind, "load") == 0 || strcmp(kind, "write") == 0 || strcmp(kind, "asset") == 0 ||
           strcmp(kind, "queue") == 0;
}

static void activity_fact_detail(const char *line, char *out, size_t cap) {
    const char *tab = strchr(line, '\t');
    const char *rest = tab ? tab + 1 : "";
    size_t o = 0;
    for (; *rest && o + 1 < cap; rest++) {
        char c = *rest == '\t' ? ' ' : *rest;
        if (c == '\n' || c == '\r') {
            break;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

/* Invoke, load, write, asset, and queue lines already stored on the activity.
 * Other fact kinds stay on the node. The cap keeps a large activity small. */
static void emit_activity_facts(cbm_sb_t *sb, const char *props) {
    if (!props || !props[0]) {
        return;
    }
    yyjson_doc *doc = yyjson_read(props, strlen(props), 0);
    if (!doc) {
        return;
    }
    yyjson_val *facts = yyjson_obj_get(yyjson_doc_get_root(doc), "facts");
    const char *text = yyjson_is_str(facts) ? yyjson_get_str(facts) : NULL;
    char *copy = text ? strdup(text) : NULL;
    yyjson_doc_free(doc);
    if (!copy) {
        return;
    }
    cbm_sb_t body;
    cbm_sb_init(&body);
    int rows = 0;
    int capped = 0;
    const char *p = copy;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[1600];
        if (n >= sizeof(line)) {
            n = sizeof(line) - 1;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        char kind[32];
        if (activity_fact_kind(line, kind, sizeof(kind))) {
            if (rows >= UIP_FACT_CAP) {
                capped = 1;
                break;
            }
            char detail[500];
            activity_fact_detail(line, detail, sizeof(detail));
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, kind, true);
            cbm_tree_cell_str(&body, detail, false);
            cbm_tree_row_end(&body);
            rows++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    free(copy);
    if (rows) {
        static const char *cols[] = {"kind", "detail"};
        cbm_tree_table_header(sb, "facts", rows, cols, 2);
        cbm_sb_append(sb, body.buf ? body.buf : "");
    }
    cbm_sb_free(&body);
    if (capped) {
        cbm_tree_scalar_bool(sb, "facts_capped", true);
    }
}

static char *tool_activity(sqlite3 *db, const char *project, const char *args) {
    char *act = arg_str(args, "activity");
    if (!act || !act[0]) {
        free(act);
        return cbm_mcp_text_result("activity is required", true);
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT id, name, qualified_name, file_path, start_line, properties "
                           "FROM nodes WHERE project=?1 AND label='Activity' AND "
                           "(qualified_name=?2 OR qualified_name LIKE '%' || ?2) LIMIT 1",
                           -1, &st, NULL) != SQLITE_OK) {
        free(act);
        return cbm_mcp_text_result("activity query failed", true);
    }
    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, act, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        free(act);
        return cbm_mcp_text_result("activity not found", true);
    }
    sqlite3_int64 id = sqlite3_column_int64(st, 0);
    cbm_sb_t sb;
    cbm_sb_init(&sb);
    cbm_tree_scalar_str(&sb, "activity", coltxt(st, 2));
    cbm_tree_scalar_str(&sb, "name", coltxt(st, 1));
    cbm_tree_scalar_str(&sb, "file", coltxt(st, 3));
    cbm_tree_scalar_int(&sb, "line", sqlite3_column_int(st, 4));
    const char *props = coltxt(st, 5);
    cbm_tree_scalar_str(&sb, "protected_by",
                        strstr(props, "\"protected_by\":\"\"") ? "" : "see properties");
    emit_activity_facts(&sb, props);
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db,
                           "SELECT e.type, t.label, t.name, t.qualified_name, e.properties "
                           "FROM edges e JOIN nodes t ON t.id=e.target_id "
                           "WHERE e.source_id=?1 AND instr(e.properties,'uipath')>0 "
                           "ORDER BY e.type, t.qualified_name LIMIT 40",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        cbm_sb_t body;
        cbm_sb_init(&body);
        int rows = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, coltxt(st, 0), true);
            cbm_tree_cell_str(&body, coltxt(st, 2), false);
            cbm_tree_cell_str(&body, coltxt(st, 3), false);
            cbm_tree_row_end(&body);
            rows++;
        }
        sqlite3_finalize(st);
        if (rows) {
            static const char *cols[] = {"edge", "target", "qn"};
            cbm_tree_table_header(&sb, "links", rows, cols, 3);
            cbm_sb_append(&sb, body.buf ? body.buf : "");
        }
        cbm_sb_free(&body);
    }
    cbm_tree_scalar_str(&sb, "completeness", "exact");
    free(act);
    return finish(&sb, 0);
}

static char *tool_usages(sqlite3 *db, const char *project, const char *args) {
    char *kind = arg_str(args, "kind");
    char *name = arg_str(args, "name");
    if (!kind || !name) {
        free(kind);
        free(name);
        return cbm_mcp_text_result("kind and name are required", true);
    }
    const char *edge = "CALLS";
    const char *label = "Workflow";
    if (strcmp(kind, "argument") == 0) {
        edge = "READS";
        label = "Argument";
    } else if (strcmp(kind, "variable") == 0) {
        edge = "READS";
        label = "Variable";
    } else if (strcmp(kind, "config_key") == 0) {
        edge = "READS_CONFIG";
        label = "ConfigKey";
    } else if (strcmp(kind, "asset") == 0) {
        edge = "USES_ASSET";
        label = "Asset";
    } else if (strcmp(kind, "queue") == 0) {
        edge = "ENQUEUES";
        label = "Queue";
    } else if (strcmp(kind, "activity_type") == 0) {
        edge = "INSTANCE_OF";
        label = "ActivityType";
    } else if (strcmp(kind, "selector") == 0) {
        edge = "SELECTS_UI_ELEMENT";
        label = "Selector";
    } else if (strcmp(kind, "package") == 0) {
        edge = "DEPENDS_ON";
        label = "Package";
    } else if (strcmp(kind, "class") == 0) {
        edge = "IMPLEMENTED_BY";
        label = "Class";
    } else if (strcmp(kind, "workflow") == 0) {
        edge = "INVOKES_WORKFLOW";
        label = "Workflow";
    }
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT s.file_path, s.qualified_name, s.start_line, e.type, e.properties "
                      "FROM edges e "
                      "JOIN nodes t ON t.id=e.target_id "
                      "JOIN nodes s ON s.id=e.source_id "
                      "WHERE t.project=?1 AND t.label=?2 AND (t.name=?3 OR t.qualified_name=?3 OR "
                      "t.qualified_name LIKE '%' || ?3) AND e.type=?4 "
                      "ORDER BY s.file_path, s.start_line LIMIT 80";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        free(kind);
        free(name);
        return cbm_mcp_text_result("usages query failed", true);
    }
    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, edge, -1, SQLITE_TRANSIENT);
    cbm_sb_t sb;
    cbm_sb_init(&sb);
    cbm_tree_scalar_str(&sb, "kind", kind);
    cbm_tree_scalar_str(&sb, "name", name);
    cbm_sb_t body;
    cbm_sb_init(&body);
    int rows = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        char line[16];
        snprintf(line, sizeof(line), "%d", sqlite3_column_int(st, 2));
        cbm_tree_row_begin(&body);
        cbm_tree_cell_str(&body, coltxt(st, 0), true);
        cbm_tree_cell_str(&body, coltxt(st, 1), false);
        cbm_tree_cell_str(&body, line, false);
        cbm_tree_cell_str(&body, coltxt(st, 3), false);
        cbm_tree_row_end(&body);
        rows++;
    }
    sqlite3_finalize(st);
    cbm_tree_scalar_int(&sb, "sites", rows);
    if (rows) {
        static const char *cols[] = {"file", "site", "line", "edge"};
        cbm_tree_table_header(&sb, "usages", rows, cols, 4);
        cbm_sb_append(&sb, body.buf ? body.buf : "");
    }
    cbm_sb_free(&body);
    int dyn = count_label(db, project, "DynamicTarget");
    cbm_tree_scalar_str(&sb, "completeness", (rows == 0 && dyn > 0) ? "lower_bound" : "exact");
    if (rows == 0 && dyn > 0) {
        cbm_tree_scalar_str(&sb, "absence", "not proven; dynamic targets exist");
    }
    free(kind);
    free(name);
    return finish(&sb, 0);
}

enum { UIP_EDGE_CAP = 100, UIP_PATH_CAP = 512 };

typedef char uip_path[UIP_PATH_CAP];

typedef struct {
    char *from;
    char *to;
    char *props;
} uip_seen;

static const char *invoke_resolution(const char *props) {
    if (!props) {
        return "literal";
    }
    if (strstr(props, "dynamic")) {
        return "dynamic";
    }
    if (strstr(props, "folded")) {
        return "folded";
    }
    if (strstr(props, "literal_ci")) {
        return "literal_ci";
    }
    if (strstr(props, "pattern")) {
        return "pattern";
    }
    return "literal";
}

static const char *invoke_direction(const char *raw) {
    if (raw && strcmp(raw, "inbound") == 0) {
        return "inbound";
    }
    if (raw && strcmp(raw, "both") == 0) {
        return "both";
    }
    return "outbound";
}

/* DynamicTarget qualified names are hashes. The path lives on the node name. */
static const char *invoke_target_text(const char *label, const char *qn, const char *name) {
    if (label && name && name[0] && strcmp(label, "DynamicTarget") == 0) {
        return name;
    }
    return qn ? qn : "";
}

static void emit_invoke_row(cbm_sb_t *body, const char *from, const char *to, const char *props) {
    cbm_tree_row_begin(body);
    cbm_tree_cell_str(body, from, true);
    cbm_tree_cell_str(body, to, false);
    cbm_tree_cell_str(body, invoke_resolution(props), false);
    cbm_tree_row_end(body);
}

static int uip_seen_has(const uip_seen *seen, int n, const char *from, const char *to,
                        const char *props) {
    for (int i = 0; i < n; i++) {
        if (strcmp(seen[i].from, from) == 0 && strcmp(seen[i].to, to) == 0 &&
            strcmp(seen[i].props, props) == 0) {
            return 1;
        }
    }
    return 0;
}

static int uip_seen_add(uip_seen *seen, int n, const char *from, const char *to,
                        const char *props) {
    char *from_copy = strdup(from ? from : "");
    char *to_copy = strdup(to ? to : "");
    char *props_copy = strdup(props ? props : "");
    if (!from_copy || !to_copy || !props_copy) {
        free(from_copy);
        free(to_copy);
        free(props_copy);
        return 0;
    }
    seen[n].from = from_copy;
    seen[n].to = to_copy;
    seen[n].props = props_copy;
    return 1;
}

static void uip_seen_free(uip_seen *seen, int n) {
    if (!seen) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(seen[i].from);
        free(seen[i].to);
        free(seen[i].props);
    }
    free(seen);
}

static int uip_path_has(const uip_path *paths, int n, const char *path) {
    for (int i = 0; i < n; i++) {
        if (strcmp(paths[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void uip_queue_file(uip_path *next, int *nnext, uip_path *seen_files, int *nseen_files,
                           const char *path) {
    if (!path || !path[0] || uip_path_has(seen_files, *nseen_files, path)) {
        return;
    }
    if (*nseen_files >= UIP_EDGE_CAP + 1 || *nnext >= UIP_EDGE_CAP) {
        return;
    }
    snprintf(seen_files[*nseen_files], UIP_PATH_CAP, "%s", path);
    (*nseen_files)++;
    snprintf(next[*nnext], UIP_PATH_CAP, "%s", path);
    (*nnext)++;
}

static const char *invoke_hop_sql(int outbound) {
    if (outbound) {
        return "SELECT s.qualified_name, t.qualified_name, e.properties, t.label, t.name, "
               "t.file_path, s.file_path FROM edges e "
               "JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id "
               "WHERE s.project=?1 AND s.label='Activity' AND e.type='INVOKES_WORKFLOW' "
               "AND (s.file_path=?2 OR s.qualified_name=?2) "
               "ORDER BY s.qualified_name, t.qualified_name, e.id LIMIT ?3 OFFSET ?4";
    }
    return "SELECT s.qualified_name, t.qualified_name, e.properties, t.label, t.name, "
           "t.file_path, s.file_path FROM edges e "
           "JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id "
           "WHERE s.project=?1 AND s.label='Activity' AND t.label='Workflow' "
           "AND e.type='INVOKES_WORKFLOW' AND (t.file_path=?2 OR t.qualified_name=?2) "
           "ORDER BY s.qualified_name, t.qualified_name, e.id LIMIT ?3 OFFSET ?4";
}

/* One frontier file. LIMIT is one past the edges still allowed, so a full page
 * proves another row exists. Duplicates do not count toward the cap. */
static void invoke_expand(sqlite3_stmt *st, const char *project, const char *file, int outbound,
                          int follow, uip_seen *seen, cbm_sb_t *body, int *rows, int *truncated,
                          uip_path *next, int *nnext, uip_path *seen_files, int *nseen_files) {
    int offset = 0;
    while (!*truncated) {
        int limit = UIP_EDGE_CAP - *rows + 1;
        if (limit < 1) {
            *truncated = 1;
            return;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, file, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, limit);
        sqlite3_bind_int(st, 4, offset);
        int fetched = 0;
        int step;
        while ((step = sqlite3_step(st)) == SQLITE_ROW) {
            fetched++;
            const char *from = coltxt(st, 0);
            const char *to_qn = coltxt(st, 1);
            const char *props = coltxt(st, 2);
            const char *label = coltxt(st, 3);
            const char *name = coltxt(st, 4);
            const char *target_file = coltxt(st, 5);
            const char *source_file = coltxt(st, 6);
            if (uip_seen_has(seen, *rows, from, to_qn, props)) {
                continue;
            }
            if (*rows >= UIP_EDGE_CAP) {
                *truncated = 1;
                return;
            }
            if (!uip_seen_add(seen, *rows, from, to_qn, props)) {
                return;
            }
            emit_invoke_row(body, from, invoke_target_text(label, to_qn, name), props);
            (*rows)++;
            if (!follow) {
                continue;
            }
            if (outbound) {
                if (strcmp(label, "Workflow") == 0) {
                    uip_queue_file(next, nnext, seen_files, nseen_files, target_file);
                }
            } else {
                uip_queue_file(next, nnext, seen_files, nseen_files, source_file);
            }
        }
        if (step != SQLITE_DONE) {
            return;
        }
        if (fetched < limit) {
            return;
        }
        offset += fetched;
    }
}

/* Outbound steps through Workflow file_path only. Inbound steps to the source
 * activity file_path. `seen_files` stops cycles. `both` is two of these walks. */
static void invoke_walk(sqlite3 *db, const char *project, const char *start, int depth,
                        int outbound, uip_seen *seen, cbm_sb_t *body, int *rows, int *truncated) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, invoke_hop_sql(outbound), -1, &st, NULL) != SQLITE_OK) {
        return;
    }
    uip_path *cur = calloc((size_t)UIP_EDGE_CAP, sizeof(uip_path));
    uip_path *next = calloc((size_t)UIP_EDGE_CAP, sizeof(uip_path));
    uip_path *seen_files = calloc((size_t)UIP_EDGE_CAP + 1, sizeof(uip_path));
    if (!cur || !next || !seen_files) {
        free(cur);
        free(next);
        free(seen_files);
        sqlite3_finalize(st);
        return;
    }
    /* Hop 0 binds `start` itself. Later hops bind file_path copies. The start
     * file is already visited so a cycle back to it does not expand again. */
    int nseen_files = 0;
    if (start && start[0]) {
        snprintf(seen_files[0], UIP_PATH_CAP, "%s", start);
        nseen_files = 1;
    }
    int ncur = 0;
    for (int hop = 0; hop < depth && !*truncated; hop++) {
        int nnext = 0;
        int follow = hop + 1 < depth;
        int nfiles = hop == 0 ? 1 : ncur;
        for (int i = 0; i < nfiles && !*truncated; i++) {
            const char *file = hop == 0 ? start : cur[i];
            invoke_expand(st, project, file, outbound, follow, seen, body, rows, truncated, next,
                          &nnext, seen_files, &nseen_files);
        }
        if (nnext > 0) {
            memcpy(cur, next, (size_t)nnext * sizeof(uip_path));
        }
        ncur = nnext;
    }
    free(cur);
    free(next);
    free(seen_files);
    sqlite3_finalize(st);
}

static void invoke_project_wide(sqlite3 *db, const char *project, cbm_sb_t *body, int *rows,
                                int *truncated) {
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT s.qualified_name, t.qualified_name, e.properties, t.label, t.name "
                      "FROM edges e "
                      "JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id "
                      "WHERE s.project=?1 AND e.type='INVOKES_WORKFLOW' "
                      "ORDER BY s.qualified_name, t.qualified_name LIMIT ?2";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, UIP_EDGE_CAP + 1);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (*rows >= UIP_EDGE_CAP) {
            *truncated = 1;
            break;
        }
        emit_invoke_row(body, coltxt(st, 0),
                        invoke_target_text(coltxt(st, 3), coltxt(st, 1), coltxt(st, 4)),
                        coltxt(st, 2));
        (*rows)++;
    }
    sqlite3_finalize(st);
}

static char *tool_invoke_graph(sqlite3 *db, const char *project, const char *args) {
    char *wf = arg_str(args, "workflow");
    char *dir_arg = arg_str(args, "direction");
    const char *direction = invoke_direction(dir_arg);
    int depth = arg_int(args, "depth", 3);
    if (depth < 1) {
        depth = 1;
    }
    if (depth > 8) {
        depth = 8;
    }
    cbm_sb_t sb;
    cbm_sb_init(&sb);
    cbm_tree_scalar_str(&sb, "project", project);
    cbm_tree_scalar_int(&sb, "depth", depth);
    cbm_tree_scalar_str(&sb, "direction", direction);
    cbm_sb_t body;
    cbm_sb_init(&body);
    int rows = 0;
    int truncated = 0;
    if (wf && wf[0]) {
        uip_seen *seen = calloc((size_t)UIP_EDGE_CAP, sizeof(uip_seen));
        if (seen) {
            if (strcmp(direction, "inbound") != 0) {
                invoke_walk(db, project, wf, depth, 1, seen, &body, &rows, &truncated);
            }
            if (!truncated && strcmp(direction, "outbound") != 0) {
                invoke_walk(db, project, wf, depth, 0, seen, &body, &rows, &truncated);
            }
            uip_seen_free(seen, rows);
        }
    } else {
        invoke_project_wide(db, project, &body, &rows, &truncated);
    }
    cbm_tree_scalar_int(&sb, "edges", rows);
    if (rows) {
        static const char *cols[] = {"from", "to", "resolution"};
        cbm_tree_table_header(&sb, "invokes", rows, cols, 3);
        cbm_sb_append(&sb, body.buf ? body.buf : "");
    }
    cbm_sb_free(&body);
    int dyn = count_label(db, project, "DynamicTarget");
    cbm_tree_scalar_str(&sb, "completeness", dyn > 0 ? "lower_bound" : "exact");
    if (truncated) {
        cbm_tree_scalar_bool(&sb, "truncated", true);
    }
    free(wf);
    free(dir_arg);
    return finish(&sb, 0);
}

static char *tool_impact(sqlite3 *db, const char *project, const char *args) {
    char *target = arg_str(args, "target");
    char *change = arg_str(args, "change");
    if (!change) {
        change = strdup("git");
    }
    cbm_sb_t sb;
    cbm_sb_init(&sb);
    cbm_tree_scalar_str(&sb, "project", project);
    cbm_tree_scalar_str(&sb, "change", change);
    cbm_tree_scalar_str(&sb, "target", target ? target : "");
    sqlite3_stmt *st = NULL;
    int rows = 0;
    cbm_sb_t body;
    cbm_sb_init(&body);
    if (target && strcmp(change, "extract_workflow") != 0 &&
        sqlite3_prepare_v2(db,
                           "SELECT s.qualified_name, e.type, t.name FROM edges e "
                           "JOIN nodes t ON t.id=e.target_id JOIN nodes s ON s.id=e.source_id "
                           "WHERE t.project=?1 AND (t.qualified_name=?2 OR t.name=?2 OR "
                           "t.qualified_name LIKE '%' || ?2) AND e.type IN "
                           "('CALLS','INVOKES_WORKFLOW','PASSES_ARGUMENT','READS','WRITES',"
                           "'READS_CONFIG','USES_ASSET','ENQUEUES','DEQUEUES','TESTS') "
                           "ORDER BY e.type, s.qualified_name LIMIT 60",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, target, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, coltxt(st, 1), true);
            cbm_tree_cell_str(&body, coltxt(st, 0), false);
            cbm_tree_cell_str(&body, coltxt(st, 2), false);
            cbm_tree_row_end(&body);
            rows++;
        }
        sqlite3_finalize(st);
    }
    if (change && strcmp(change, "extract_workflow") == 0 && target) {
        cbm_tree_scalar_str(&sb, "proposed_contract",
                            "in_ for READS outside the range, out_ for WRITES, io_ for both");
    }
    if (change && strcmp(change, "git") == 0) {
        cbm_tree_scalar_str(&sb, "git",
                            "detect_changes walks CALLS plus INVOKES_WORKFLOW, READS, WRITES, "
                            "READS_CONFIG, PASSES_ARGUMENT, USES_ASSET, ENQUEUES, DEQUEUES");
    }
    cbm_tree_scalar_int(&sb, "affected", rows);
    if (rows) {
        static const char *cols[] = {"edge", "site", "name"};
        cbm_tree_table_header(&sb, "edits", rows, cols, 3);
        cbm_sb_append(&sb, body.buf ? body.buf : "");
    }
    cbm_sb_free(&body);
    int dyn = count_label(db, project, "DynamicTarget");
    cbm_tree_scalar_str(&sb, "completeness", dyn > 0 ? "lower_bound" : "exact");
    free(target);
    free(change);
    return finish(&sb, 0);
}

static char *tool_lint(sqlite3 *db, const char *project, const char *args) {
    (void)args;
    cbm_sb_t sb;
    cbm_sb_init(&sb);
    cbm_tree_scalar_str(&sb, "project", project);
    sqlite3_stmt *st = NULL;
    cbm_sb_t body;
    cbm_sb_init(&body);
    int rows = 0;
    const char *completeness = "exact";
    if (sqlite3_prepare_v2(db,
                           "SELECT file_path, name, properties FROM nodes WHERE project=?1 AND "
                           "label='Selector' AND instr(properties,'\"risk_score\":0')=0 "
                           "LIMIT 40",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, "selector.fragile", true);
            cbm_tree_cell_str(&body, coltxt(st, 0), false);
            cbm_tree_cell_str(&body, coltxt(st, 1), false);
            cbm_tree_row_end(&body);
            rows++;
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db,
                           "SELECT s.file_path, t.name, e.properties FROM edges e "
                           "JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id "
                           "WHERE s.project=?1 AND e.type='PASSES_ARGUMENT' AND "
                           "(instr(e.properties,'missing')>0 OR instr(e.properties,'mismatch')>0) "
                           "LIMIT 40",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, "binding.status", true);
            cbm_tree_cell_str(&body, coltxt(st, 0), false);
            cbm_tree_cell_str(&body, coltxt(st, 1), false);
            cbm_tree_row_end(&body);
            rows++;
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db,
                           "SELECT qualified_name FROM nodes WHERE project=?1 AND "
                           "label='Workflow' AND CAST(json_extract(properties,'$.activity_count') "
                           "AS INTEGER) > 30 ORDER BY CAST(json_extract(properties,"
                           "'$.activity_count') AS INTEGER) DESC LIMIT 21",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        int large_n = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (large_n >= 20) {
                completeness = "truncated";
                break;
            }
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, "refactor.large_workflow", true);
            cbm_tree_cell_str(&body, coltxt(st, 0), false);
            cbm_tree_cell_str(&body, "activity_count>30", false);
            cbm_tree_row_end(&body);
            large_n++;
            rows++;
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db,
                           "SELECT file_path FROM nodes WHERE project=?1 AND label='ConfigFile' "
                           "AND instr(properties,'\"is_loaded\":false')>0 LIMIT 10",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, "config.unused_file", true);
            cbm_tree_cell_str(&body, coltxt(st, 0), false);
            cbm_tree_cell_str(&body, "not loaded", false);
            cbm_tree_row_end(&body);
            rows++;
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db,
                           "SELECT name FROM nodes WHERE project=?1 AND label='DynamicTarget' "
                           "AND (instr(name,'.xaml')>0 OR instr(name,'.cs')>0) "
                           "ORDER BY name LIMIT 40",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, "invoke.unresolved", true);
            cbm_tree_cell_str(&body, coltxt(st, 0), false);
            cbm_tree_cell_str(&body, "unresolved", false);
            cbm_tree_row_end(&body);
            rows++;
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db,
                           "SELECT s.file_path, t.file_path FROM edges e "
                           "JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id "
                           "WHERE s.project=?1 AND s.label='Activity' "
                           "AND e.type='INVOKES_WORKFLOW' "
                           "AND instr(e.properties,'\"candidate\":true')>0 "
                           "AND instr(s.properties, t.file_path)=0 "
                           "AND (instr(s.properties,'/')>0 OR instr(s.properties, char(92))>0) "
                           "ORDER BY s.file_path, t.file_path LIMIT 40",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, "invoke.candidate", true);
            cbm_tree_cell_str(&body, coltxt(st, 0), false);
            cbm_tree_cell_str(&body, coltxt(st, 1), false);
            cbm_tree_row_end(&body);
            rows++;
        }
        sqlite3_finalize(st);
    }
    cbm_tree_scalar_int(&sb, "findings", rows);
    if (rows) {
        static const char *cols[] = {"rule", "file", "evidence"};
        cbm_tree_table_header(&sb, "lint", rows, cols, 3);
        cbm_sb_append(&sb, body.buf ? body.buf : "");
    }
    cbm_sb_free(&body);
    cbm_tree_scalar_str(&sb, "completeness", completeness);
    return finish(&sb, 0);
}

char *cbm_uipath_architecture_summary(struct cbm_store *store, const char *project) {
    if (!store || !project) {
        return strdup("");
    }
    sqlite3 *db = cbm_store_get_db(store);
    if (!db) {
        return strdup("");
    }
    int n = count_label(db, project, "UiPathProject");
    if (n <= 0) {
        return strdup("");
    }
    char *buf = malloc(512);
    if (!buf) {
        return NULL;
    }
    snprintf(buf, 512,
             "uipath_projects: %d\nworkflows: %d\nactivities: %d\ninvokes: %d\n"
             "dynamic_targets: %d\ncompleteness: %s\n",
             n, count_label(db, project, "Workflow"), count_label(db, project, "Activity"),
             count_edge(db, project, "INVOKES_WORKFLOW"), count_label(db, project, "DynamicTarget"),
             count_label(db, project, "DynamicTarget") ? "lower_bound" : "exact");
    return buf;
}

char *cbm_uipath_dispatch_tool(struct cbm_store *store, const char *project, const char *tool,
                               const char *args_json) {
    if (!tool) {
        return cbm_mcp_text_result("unknown tool", true);
    }
    if (!store || !project || !project[0]) {
        return cbm_mcp_text_result("project not found or not indexed", true);
    }
    sqlite3 *db = cbm_store_get_db(store);
    if (!db) {
        return cbm_mcp_text_result("project not found or not indexed", true);
    }
    if (strcmp(tool, "uipath_overview") == 0) {
        return tool_overview(db, project);
    }
    if (strcmp(tool, "uipath_workflow_outline") == 0) {
        return tool_outline(db, project, args_json);
    }
    if (strcmp(tool, "uipath_activity_details") == 0) {
        return tool_activity(db, project, args_json);
    }
    if (strcmp(tool, "uipath_find_usages") == 0) {
        return tool_usages(db, project, args_json);
    }
    if (strcmp(tool, "uipath_invoke_graph") == 0) {
        return tool_invoke_graph(db, project, args_json);
    }
    if (strcmp(tool, "uipath_impact") == 0) {
        return tool_impact(db, project, args_json);
    }
    if (strcmp(tool, "uipath_lint") == 0) {
        return tool_lint(db, project, args_json);
    }
    return cbm_mcp_text_result("unknown tool", true);
}
