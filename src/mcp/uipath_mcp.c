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

static char *tool_invoke_graph(sqlite3 *db, const char *project, const char *args) {
    char *wf = arg_str(args, "workflow");
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
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT s.qualified_name, t.qualified_name, e.properties FROM edges e "
                      "JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id "
                      "WHERE s.project=?1 AND e.type='INVOKES_WORKFLOW' "
                      "AND (?2 IS NULL OR s.file_path=?2 OR s.qualified_name=?2) "
                      "ORDER BY s.qualified_name, t.qualified_name LIMIT 100";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        if (wf && wf[0]) {
            sqlite3_bind_text(st, 2, wf, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(st, 2);
        }
        cbm_sb_t body;
        cbm_sb_init(&body);
        int rows = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *props = coltxt(st, 2);
            const char *res = "literal";
            if (strstr(props, "dynamic")) {
                res = "dynamic";
            } else if (strstr(props, "folded")) {
                res = "folded";
            } else if (strstr(props, "literal_ci")) {
                res = "literal_ci";
            }
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, coltxt(st, 0), true);
            cbm_tree_cell_str(&body, coltxt(st, 1), false);
            cbm_tree_cell_str(&body, res, false);
            cbm_tree_row_end(&body);
            rows++;
        }
        sqlite3_finalize(st);
        cbm_tree_scalar_int(&sb, "edges", rows);
        if (rows) {
            static const char *cols[] = {"from", "to", "resolution"};
            cbm_tree_table_header(&sb, "invokes", rows, cols, 3);
            cbm_sb_append(&sb, body.buf ? body.buf : "");
        }
        cbm_sb_free(&body);
    }
    int dyn = count_label(db, project, "DynamicTarget");
    cbm_tree_scalar_str(&sb, "completeness", dyn > 0 ? "lower_bound" : "exact");
    free(wf);
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
                           "AS INTEGER) > 30 LIMIT 20",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            cbm_tree_row_begin(&body);
            cbm_tree_cell_str(&body, "refactor.large_workflow", true);
            cbm_tree_cell_str(&body, coltxt(st, 0), false);
            cbm_tree_cell_str(&body, "activity_count>30", false);
            cbm_tree_row_end(&body);
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
    cbm_tree_scalar_int(&sb, "findings", rows);
    if (rows) {
        static const char *cols[] = {"rule", "file", "evidence"};
        cbm_tree_table_header(&sb, "lint", rows, cols, 3);
        cbm_sb_append(&sb, body.buf ? body.buf : "");
    }
    cbm_sb_free(&body);
    cbm_tree_scalar_str(&sb, "completeness", "exact");
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
