#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "mcp/uipath_mcp.h"
#include "pipeline/pipeline.h"
#include "pipeline/uipath.h"
#include "sqlite3.h"
#include "store/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_file(const char *dir, const char *name, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        cbm_mkdir_p(path, 0755);
        *slash = '/';
    }
    FILE *f = cbm_fopen(path, "wb");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static int sql_count(const char *db_path, const char *sql) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

static int edge_count(const char *db_path, const char *type) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM edges WHERE type='%s' AND instr(properties,'uipath')>0", type);
    return sql_count(db_path, sql);
}

static int label_count(const char *db_path, const char *label) {
    char sql[192];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM nodes WHERE label='%s'", label);
    return sql_count(db_path, sql);
}

TEST(uipath_expr_reads_config_and_writes) {
    const char *vars[] = {"str_Id"};
    const char *vtypes[] = {"System.String"};
    const char *args[] = {"in_Config"};
    const char *atypes[] = {"Dictionary"};
    uipath_expr_facts ef;
    uipath_expr_analyze("[in_Config(\"InvoiceId\").ToString]", 0, vars, vtypes, 1, args, atypes, 1,
                        0, &ef);
    ASSERT_EQ(ef.nconfig, 1);
    ASSERT_STR_EQ(ef.config_keys[0], "InvoiceId");
    uipath_expr_analyze("[str_Id]", 0, vars, vtypes, 1, args, atypes, 1, 1, &ef);
    ASSERT_EQ(ef.nwrites, 1);
    ASSERT_STR_EQ(ef.writes[0], "str_Id");
    PASS();
}

TEST(uipath_selector_risk_scores_idx) {
    char reasons[64];
    int score = uipath_selector_risk("<wnd app='sap.exe' idx='1' />", reasons, sizeof(reasons));
    ASSERT_GT(score, 0);
    ASSERT_NOT_NULL(strstr(reasons, "idx"));
    PASS();
}

static void write_demo(const char *tmp) {
    write_file(tmp, "project.json",
               "{\n"
               "  \"name\": \"Demo\",\n"
               "  \"main\": \"Main.xaml\",\n"
               "  \"expressionLanguage\": \"VisualBasic\",\n"
               "  \"studioVersion\": \"24.10.0\",\n"
               "  \"targetFramework\": \"Windows\",\n"
               "  \"dependencies\": {\"UiPath.System.Activities\": \"[24.10.1]\"},\n"
               "  \"designOptions\": {\"outputType\": \"Process\"},\n"
               "  \"entryPoints\": [{\"filePath\": \"Main.xaml\"}]\n"
               "}\n");
    write_file(tmp, "Data/Config.json",
               "{\n"
               "  \"Settings\": [{\"Name\": \"InvoiceId\", \"Value\": \"INV-1\", \"Description\": "
               "\"id\"}],\n"
               "  \"Assets\": [{\"Name\": \"ERPCred\", \"Asset\": \"ERP_Credential\", \"Folder\": "
               "\"Finance\"}]\n"
               "}\n");
    write_file(tmp, "Main.xaml",
               "<Activity x:Class=\"Main\"\n"
               " xmlns=\"http://schemas.microsoft.com/netfx/2009/xaml/activities\"\n"
               " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\"\n"
               " xmlns:ui=\"http://schemas.uipath.com/workflow/activities\"\n"
               " xmlns:scg=\"clr-namespace:System.Collections.Generic;assembly=mscorlib\">\n"
               "  <x:Members>\n"
               "    <x:Property Name=\"in_Config\" Type=\"InArgument(x:String)\" />\n"
               "    <x:Property Name=\"out_Total\" Type=\"OutArgument(x:String)\" />\n"
               "  </x:Members>\n"
               "  <Sequence DisplayName=\"Main\" sap2010:WorkflowViewState.IdRef=\"Sequence_1\">\n"
               "    <Sequence.Variables>\n"
               "      <Variable x:TypeArguments=\"x:String\" Name=\"str_Id\" />\n"
               "    </Sequence.Variables>\n"
               "    <Assign DisplayName=\"Set id\" sap2010:WorkflowViewState.IdRef=\"Assign_1\">\n"
               "      <Assign.To><OutArgument x:TypeArguments=\"x:String\">[str_Id]</OutArgument></Assign.To>\n"
               "      <Assign.Value><InArgument x:TypeArguments=\"x:String\">[in_Config(\"InvoiceId\").ToString]</InArgument></Assign.Value>\n"
               "    </Assign>\n"
               "    <ui:InvokeWorkflowFile DisplayName=\"Post\" WorkflowFileName=\"Post.xaml\" "
               "sap2010:WorkflowViewState.IdRef=\"InvokeWorkflowFile_1\">\n"
               "      <ui:InvokeWorkflowFile.Arguments>\n"
               "        <InArgument x:TypeArguments=\"x:String\" x:Key=\"in_Id\">[str_Id]</InArgument>\n"
               "      </ui:InvokeWorkflowFile.Arguments>\n"
               "    </ui:InvokeWorkflowFile>\n"
               "    <ui:GetAsset AssetName=\"ERP_Credential\" DisplayName=\"Get cred\" "
               "sap2010:WorkflowViewState.IdRef=\"GetAsset_1\" />\n"
               "    <ui:Click Selector=\"&lt;wnd app='sap.exe' idx='1' /&gt;\" DisplayName=\"Click\" "
               "sap2010:WorkflowViewState.IdRef=\"Click_1\" />\n"
               "  </Sequence>\n"
               "</Activity>\n");
    write_file(tmp, "Post.xaml",
               "<Activity x:Class=\"Post\"\n"
               " xmlns=\"http://schemas.microsoft.com/netfx/2009/xaml/activities\"\n"
               " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">\n"
               "  <x:Members>\n"
               "    <x:Property Name=\"in_Id\" Type=\"InArgument(x:String)\" />\n"
               "  </x:Members>\n"
               "  <Sequence DisplayName=\"Post\" sap2010:WorkflowViewState.IdRef=\"Sequence_1\">\n"
               "    <ui:LogMessage xmlns:ui=\"http://schemas.uipath.com/workflow/activities\" "
               "DisplayName=\"Log\" sap2010:WorkflowViewState.IdRef=\"LogMessage_1\" "
               "Message=\"[in_Id]\" />\n"
               "  </Sequence>\n"
               "</Activity>\n");
    write_file(tmp, ".local/noise.xaml",
               "<Activity xmlns=\"http://schemas.microsoft.com/netfx/2009/xaml/activities\">\n"
               "  <Sequence />\n"
               "</Activity>\n");
    write_file(tmp, "Window.xaml",
               "<Window xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\"\n"
               " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">\n"
               "  <Grid><Button Content=\"Ok\" /></Grid>\n"
               "</Window>\n");
}

TEST(uipath_indexes_workflow_invoke_and_skips_noise) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_uipath_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmp));
    write_demo(tmp);
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/graph.db", tmp);
    cbm_pipeline_t *p = cbm_pipeline_new(tmp, db_path, CBM_MODE_FAST);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);

    ASSERT_EQ(label_count(db_path, "UiPathProject"), 1);
    ASSERT_EQ(label_count(db_path, "Workflow"), 2);
    ASSERT_GT(label_count(db_path, "Activity"), 0);
    ASSERT_GT(label_count(db_path, "Argument"), 0);
    ASSERT_GT(edge_count(db_path, "INVOKES_WORKFLOW"), 0);
    ASSERT_GT(edge_count(db_path, "CALLS"), 0);
    ASSERT_GT(edge_count(db_path, "READS_CONFIG"), 0);
    ASSERT_GT(edge_count(db_path, "WRITES"), 0);
    ASSERT_GT(edge_count(db_path, "PASSES_ARGUMENT"), 0);
    ASSERT_GT(edge_count(db_path, "USES_ASSET"), 0);
    ASSERT_GT(edge_count(db_path, "SELECTS_UI_ELEMENT"), 0);
    ASSERT_EQ(sql_count(db_path, "SELECT COUNT(*) FROM nodes WHERE file_path LIKE '.local/%'"), 0);
    ASSERT_EQ(sql_count(db_path, "SELECT COUNT(*) FROM nodes WHERE label='Workflow' AND "
                                 "file_path='Window.xaml'"),
              0);
    ASSERT_GT(label_count(db_path, "ConfigKey"), 0);
    ASSERT_GT(label_count(db_path, "Package"), 0);

    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

TEST(uipath_incremental_keeps_invoke_edges) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_uipath_inc_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmp));
    write_demo(tmp);
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/graph.db", tmp);
    cbm_pipeline_t *p = cbm_pipeline_new(tmp, db_path, CBM_MODE_FAST);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    int full_invokes = edge_count(db_path, "INVOKES_WORKFLOW");
    ASSERT_GT(full_invokes, 0);
    cbm_pipeline_free(p);

    write_file(tmp, "Main.xaml",
               "<Activity x:Class=\"Main\"\n"
               " xmlns=\"http://schemas.microsoft.com/netfx/2009/xaml/activities\"\n"
               " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\"\n"
               " xmlns:ui=\"http://schemas.uipath.com/workflow/activities\">\n"
               "  <Sequence DisplayName=\"Main\" sap2010:WorkflowViewState.IdRef=\"Sequence_1\">\n"
               "    <ui:InvokeWorkflowFile DisplayName=\"Post\" WorkflowFileName=\"post.xaml\" "
               "sap2010:WorkflowViewState.IdRef=\"InvokeWorkflowFile_2\" />\n"
               "  </Sequence>\n"
               "</Activity>\n");
    p = cbm_pipeline_new(tmp, db_path, CBM_MODE_FAST);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    int again = edge_count(db_path, "INVOKES_WORKFLOW");
    ASSERT_GT(again, 0);
    cbm_pipeline_free(p);
    th_rmtree(tmp);
    PASS();
}

static char *save_env(const char *name) {
    const char *value = getenv(name);
    return value ? strdup(value) : NULL;
}

static void restore_env(const char *name, char *saved) {
    if (saved) {
        cbm_setenv(name, saved, 1);
        free(saved);
    } else {
        cbm_unsetenv(name);
    }
}

static void write_project(const char *dir, const char *name, const char *main_rel) {
    char body[640];
    snprintf(body, sizeof(body),
             "{\n"
             "  \"name\": \"%s\",\n"
             "  \"main\": \"%s\",\n"
             "  \"expressionLanguage\": \"VisualBasic\",\n"
             "  \"studioVersion\": \"24.10.0\",\n"
             "  \"targetFramework\": \"Windows\",\n"
             "  \"dependencies\": {\"UiPath.System.Activities\": \"[24.10.1]\"},\n"
             "  \"designOptions\": {\"outputType\": \"Process\"},\n"
             "  \"entryPoints\": [{\"filePath\": \"%s\"}]\n"
             "}\n",
             name, main_rel, main_rel);
    write_file(dir, "project.json", body);
}

static void write_plain_xaml(const char *dir, const char *rel, const char *class_name) {
    char body[640];
    snprintf(body, sizeof(body),
             "<Activity x:Class=\"%s\"\n"
             " xmlns=\"http://schemas.microsoft.com/netfx/2009/xaml/activities\"\n"
             " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">\n"
             "  <Sequence DisplayName=\"%s\" sap2010:WorkflowViewState.IdRef=\"Sequence_1\" />\n"
             "</Activity>\n",
             class_name, class_name);
    write_file(dir, rel, body);
}

/* LogMessage elements are the only counted activities (the root Activity is not). */
static void write_log_workflow(const char *dir, const char *rel, int n) {
    size_t cap = 512 + (size_t)n * 192;
    char *body = malloc(cap);
    if (!body) {
        return;
    }
    int used = snprintf(body, cap,
                        "<Activity x:Class=\"Logs\"\n"
                        " xmlns=\"http://schemas.microsoft.com/netfx/2009/xaml/activities\"\n"
                        " xmlns:ui=\"http://schemas.uipath.com/workflow/activities\"\n"
                        " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">\n");
    for (int i = 0; i < n && used > 0 && (size_t)used < cap; i++) {
        used += snprintf(body + used, cap - (size_t)used,
                         "  <ui:LogMessage DisplayName=\"L%d\" Message=\"m%d\" "
                         "sap2010:WorkflowViewState.IdRef=\"LogMessage_%d\" />\n",
                         i, i, i);
    }
    if (used > 0 && (size_t)used + 16 < cap) {
        snprintf(body + used, cap - (size_t)used, "</Activity>\n");
    }
    write_file(dir, rel, body);
    free(body);
}

static void write_invoke_xaml(const char *dir, const char *rel, const char *class_name,
                              const char *target) {
    char body[768];
    snprintf(body, sizeof(body),
             "<Activity x:Class=\"%s\"\n"
             " xmlns=\"http://schemas.microsoft.com/netfx/2009/xaml/activities\"\n"
             " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\"\n"
             " xmlns:ui=\"http://schemas.uipath.com/workflow/activities\">\n"
             "  <ui:InvokeWorkflowFile DisplayName=\"Go\" WorkflowFileName=\"%s\" "
             "sap2010:WorkflowViewState.IdRef=\"InvokeWorkflowFile_1\" />\n"
             "</Activity>\n",
             class_name, target);
    write_file(dir, rel, body);
}

static void write_unresolved_fixture(const char *dir) {
    write_project(dir, "DirMiss", "Main.xaml");
    write_plain_xaml(dir, "RPA Framework/String/GetCaseInfoFromCaseNumber.xaml", "GetCaseInfo");
    write_plain_xaml(dir, "Nested/Only.xaml", "Only");
    write_plain_xaml(dir, "Post.xaml", "Post");
    write_file(dir, "Main.xaml",
               "<Activity x:Class=\"Main\"\n"
               " xmlns=\"http://schemas.microsoft.com/netfx/2009/xaml/activities\"\n"
               " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\"\n"
               " xmlns:ui=\"http://schemas.uipath.com/workflow/activities\">\n"
               "  <Sequence DisplayName=\"Main\" sap2010:WorkflowViewState.IdRef=\"Sequence_1\">\n"
               "    <ui:InvokeWorkflowFile DisplayName=\"CSI\" "
               "WorkflowFileName=\"CSI Framework\\String\\GetCaseInfoFromCaseNumber.xaml\" "
               "sap2010:WorkflowViewState.IdRef=\"InvokeWorkflowFile_1\" />\n"
               "    <ui:InvokeWorkflowFile DisplayName=\"Missing\" "
               "WorkflowFileName=\"Missing\\NoSuch.xaml\" "
               "sap2010:WorkflowViewState.IdRef=\"InvokeWorkflowFile_2\" />\n"
               "    <ui:InvokeWorkflowFile DisplayName=\"Only\" WorkflowFileName=\"Only.xaml\" "
               "sap2010:WorkflowViewState.IdRef=\"InvokeWorkflowFile_3\" />\n"
               "    <ui:InvokeWorkflowFile DisplayName=\"Post\" WorkflowFileName=\"Post.xaml\" "
               "sap2010:WorkflowViewState.IdRef=\"InvokeWorkflowFile_4\">\n"
               "      <ui:InvokeWorkflowFile.Arguments>\n"
               "        <InArgument x:TypeArguments=\"x:String\" "
               "x:Key=\"not_an_arg\">plain</InArgument>\n"
               "      </ui:InvokeWorkflowFile.Arguments>\n"
               "    </ui:InvokeWorkflowFile>\n"
               "  </Sequence>\n"
               "</Activity>\n");
}

static int index_uipath(const char *tmp, char *db_path, size_t db_cap, char **project_out) {
    snprintf(db_path, db_cap, "%s/graph.db", tmp);
    cbm_pipeline_t *p = cbm_pipeline_new(tmp, db_path, CBM_MODE_FAST);
    if (!p) {
        return -1;
    }
    int rc = cbm_pipeline_run(p);
    if (project_out) {
        const char *name = cbm_pipeline_project_name(p);
        *project_out = name ? strdup(name) : NULL;
    }
    cbm_pipeline_free(p);
    return rc;
}

static int workflow_activities(const char *db_path, const char *file_path) {
    char sql[384];
    snprintf(sql, sizeof(sql),
             "SELECT CAST(json_extract(properties,'$.activity_count') AS INTEGER) "
             "FROM nodes WHERE label='Workflow' AND file_path='%s'",
             file_path);
    return sql_count(db_path, sql);
}

static const char *require_text(const char *text, const char *needle) {
    if (text && needle && strstr(text, needle)) {
        return strstr(text, needle);
    }
    fprintf(stderr, "missing [%s] in:\n%s\n", needle ? needle : "", text ? text : "(null)");
    return NULL;
}

static int text_lacks(const char *text, const char *needle) {
    if (text && needle && strstr(text, needle)) {
        fprintf(stderr, "unexpected [%s] in:\n%s\n", needle, text);
        return 0;
    }
    return text != NULL;
}

static int count_substr(const char *text, const char *needle) {
    int n = 0;
    if (!text || !needle || !needle[0]) {
        return 0;
    }
    size_t len = strlen(needle);
    for (const char *p = text; (p = strstr(p, needle)) != NULL; p += len) {
        n++;
    }
    return n;
}

static int tree_int_field(const char *text, const char *key) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%s: ", key);
    const char *hit = text ? strstr(text, prefix) : NULL;
    if (!hit) {
        return -1;
    }
    return atoi(hit + strlen(prefix));
}

static char *uipath_tool(const char *db_path, const char *project, const char *tool,
                         const char *args) {
    cbm_store_t *store = cbm_store_open_path_query(db_path);
    if (!store) {
        return NULL;
    }
    char *out = cbm_uipath_dispatch_tool(store, project, tool, args);
    cbm_store_close(store);
    return out;
}

TEST(uipath_parallel_keeps_invoke_edges) {
    char *saved_workers = save_env("CBM_WORKERS");
    int env_rc = cbm_setenv("CBM_WORKERS", "2", 1);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_uip_par_XXXXXX");
    int made = cbm_mkdtemp(tmp) != NULL;
    int rc1 = -1;
    int rc2 = -1;
    int invokes = -1;
    int child_edges = -1;
    int invokes2 = -1;
    if (made && env_rc == 0) {
        write_project(tmp, "Parallel", "Main.xaml");
        write_invoke_xaml(tmp, "Main.xaml", "Main", "Child.xaml");
        write_plain_xaml(tmp, "Child.xaml", "Child");
        for (int i = 0; i < 52; i++) {
            char name[64];
            char body[64];
            snprintf(name, sizeof(name), "pad/pad_%02d.py", i);
            snprintf(body, sizeof(body), "def pad_%02d():\n    return %d\n", i, i);
            write_file(tmp, name, body);
        }
        char db_path[512];
        snprintf(db_path, sizeof(db_path), "%s/graph.db", tmp);
        cbm_pipeline_t *p = cbm_pipeline_new(tmp, db_path, CBM_MODE_FAST);
        if (p) {
            rc1 = cbm_pipeline_run(p);
            if (rc1 == 0) {
                invokes = edge_count(db_path, "INVOKES_WORKFLOW");
                child_edges = sql_count(
                    db_path,
                    "SELECT COUNT(*) FROM edges e "
                    "JOIN nodes s ON s.id=e.source_id JOIN nodes t ON t.id=e.target_id "
                    "WHERE e.type='INVOKES_WORKFLOW' AND s.file_path='Main.xaml' "
                    "AND t.file_path='Child.xaml'");
            }
            cbm_pipeline_free(p);
            write_file(tmp, "Main.xaml",
                       "<Activity x:Class=\"Main\"\n"
                       " xmlns=\"http://schemas.microsoft.com/netfx/2009/xaml/activities\"\n"
                       " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\"\n"
                       " xmlns:ui=\"http://schemas.uipath.com/workflow/activities\">\n"
                       "  <ui:InvokeWorkflowFile DisplayName=\"Child again\" "
                       "WorkflowFileName=\"Child.xaml\" "
                       "sap2010:WorkflowViewState.IdRef=\"InvokeWorkflowFile_2\" />\n"
                       "</Activity>\n");
            p = cbm_pipeline_new(tmp, db_path, CBM_MODE_FAST);
            if (p) {
                rc2 = cbm_pipeline_run(p);
                if (rc2 == 0) {
                    invokes2 = edge_count(db_path, "INVOKES_WORKFLOW");
                }
                cbm_pipeline_free(p);
            }
        }
    }
    restore_env("CBM_WORKERS", saved_workers);
    ASSERT_EQ(env_rc, 0);
    ASSERT_TRUE(made);
    ASSERT_EQ(rc1, 0);
    ASSERT_GT(invokes, 0);
    ASSERT_GT(child_edges, 0);
    ASSERT_EQ(rc2, 0);
    ASSERT_GT(invokes2, 0);
    th_rmtree(tmp);
    PASS();
}

TEST(uipath_directory_invoke_is_unresolved) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_uip_dir_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmp));
    write_unresolved_fixture(tmp);
    char db_path[512];
    ASSERT_EQ(index_uipath(tmp, db_path, sizeof(db_path), NULL), 0);

    ASSERT_EQ(sql_count(db_path,
                        "SELECT COUNT(*) FROM edges e JOIN nodes t ON t.id=e.target_id "
                        "WHERE e.type='INVOKES_WORKFLOW' AND t.file_path="
                        "'RPA Framework/String/GetCaseInfoFromCaseNumber.xaml'"),
              0);
    ASSERT_GT(sql_count(db_path, "SELECT COUNT(*) FROM nodes WHERE label='DynamicTarget' "
                                 "AND instr(name,'CSI Framework')>0"),
              0);
    ASSERT_GT(sql_count(db_path, "SELECT COUNT(*) FROM nodes WHERE label='DynamicTarget' "
                                 "AND instr(name,'NoSuch.xaml')>0"),
              0);
    ASSERT_GT(sql_count(db_path,
                        "SELECT COUNT(*) FROM edges e JOIN nodes t ON t.id=e.target_id "
                        "WHERE e.type='INVOKES_WORKFLOW' AND t.file_path='Nested/Only.xaml' "
                        "AND instr(e.properties,'\"resolution\":\"pattern\"')>0"),
              0);
    ASSERT_EQ(sql_count(db_path, "SELECT COUNT(*) FROM nodes WHERE label='DynamicTarget' "
                                 "AND name='not_an_arg'"),
              0);
    ASSERT_GT(sql_count(db_path,
                        "SELECT COUNT(*) FROM nodes WHERE label='Workflow' AND file_path="
                        "'RPA Framework/String/GetCaseInfoFromCaseNumber.xaml'"),
              0);

    th_rmtree(tmp);
    PASS();
}

TEST(uipath_lint_orders_large_and_unresolved) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_uip_lint_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmp));
    write_unresolved_fixture(tmp);
    write_log_workflow(tmp, "big31.xaml", 31);
    write_log_workflow(tmp, "big40.xaml", 40);
    write_log_workflow(tmp, "big50.xaml", 50);
    char db_path[512];
    char *project = NULL;
    ASSERT_EQ(index_uipath(tmp, db_path, sizeof(db_path), &project), 0);
    ASSERT_NOT_NULL(project);
    ASSERT_EQ(workflow_activities(db_path, "big31.xaml"), 31);
    ASSERT_EQ(workflow_activities(db_path, "big40.xaml"), 40);
    ASSERT_EQ(workflow_activities(db_path, "big50.xaml"), 50);

    char *out = uipath_tool(db_path, project, "uipath_lint", "{}");
    const char *big50 = require_text(out, "big50.xaml");
    const char *big40 = require_text(out, "big40.xaml");
    const char *big31 = require_text(out, "big31.xaml");
    ASSERT_NOT_NULL(out);
    ASSERT_NOT_NULL(big50);
    ASSERT_NOT_NULL(big40);
    ASSERT_NOT_NULL(big31);
    ASSERT_TRUE(big50 < big40);
    ASSERT_TRUE(big40 < big31);
    ASSERT_NOT_NULL(require_text(out, "invoke.unresolved"));
    ASSERT_NOT_NULL(require_text(out, "CSI Framework"));
    ASSERT_NOT_NULL(require_text(out, "NoSuch.xaml"));
    ASSERT_TRUE(text_lacks(out, "not_an_arg"));
    ASSERT_NOT_NULL(require_text(out, "completeness: exact"));
    ASSERT_TRUE(text_lacks(out, "completeness: truncated"));

    free(out);
    free(project);
    th_rmtree(tmp);
    PASS();
}

TEST(uipath_lint_truncates_large_workflows) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_uip_cap_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmp));
    write_project(tmp, "Cap", "bulk/w00.xaml");
    for (int i = 0; i < 21; i++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "bulk/w%02d.xaml", i);
        write_log_workflow(tmp, rel, 31);
    }
    char db_path[512];
    char *project = NULL;
    ASSERT_EQ(index_uipath(tmp, db_path, sizeof(db_path), &project), 0);
    ASSERT_NOT_NULL(project);
    ASSERT_EQ(workflow_activities(db_path, "bulk/w00.xaml"), 31);
    ASSERT_EQ(sql_count(db_path,
                        "SELECT COUNT(*) FROM nodes WHERE label='Workflow' AND "
                        "CAST(json_extract(properties,'$.activity_count') AS INTEGER) > 30"),
              21);

    char *out = uipath_tool(db_path, project, "uipath_lint", "{}");
    const char *row = require_text(out, "refactor.large_workflow ");
    ASSERT_NOT_NULL(out);
    ASSERT_NOT_NULL(row);
    ASSERT_EQ(strncmp(row + strlen("refactor.large_workflow "), "bulk/w", 6), 0);
    ASSERT_EQ(count_substr(out, "refactor.large_workflow"), 20);
    ASSERT_NOT_NULL(require_text(out, "completeness: truncated"));

    free(out);
    free(project);
    th_rmtree(tmp);
    PASS();
}

TEST(uipath_invoke_graph_walks_depth) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_uip_graph_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmp));
    write_project(tmp, "Graph", "Main.xaml");
    write_file(tmp, "Main.xaml",
               "<Activity x:Class=\"Main\"\n"
               " xmlns=\"http://schemas.microsoft.com/netfx/2009/xaml/activities\"\n"
               " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\"\n"
               " xmlns:ui=\"http://schemas.uipath.com/workflow/activities\">\n"
               "  <ui:InvokeWorkflowFile DisplayName=\"Mid\" WorkflowFileName=\"Mid.xaml\" "
               "sap2010:WorkflowViewState.IdRef=\"InvokeWorkflowFile_1\" />\n"
               "  <ui:InvokeWorkflowFile DisplayName=\"Miss\" "
               "WorkflowFileName=\"Other\\Leaf.xaml\" "
               "sap2010:WorkflowViewState.IdRef=\"InvokeWorkflowFile_2\" />\n"
               "</Activity>\n");
    write_invoke_xaml(tmp, "Mid.xaml", "Mid", "Leaf.xaml");
    write_plain_xaml(tmp, "Nested/Leaf.xaml", "Leaf");
    write_invoke_xaml(tmp, "CycleA.xaml", "CycleA", "CycleB.xaml");
    write_invoke_xaml(tmp, "CycleB.xaml", "CycleB", "CycleA.xaml");

    char db_path[512];
    char *project = NULL;
    ASSERT_EQ(index_uipath(tmp, db_path, sizeof(db_path), &project), 0);
    ASSERT_NOT_NULL(project);

    char *depth1 = uipath_tool(db_path, project, "uipath_invoke_graph",
                               "{\"workflow\":\"Main.xaml\",\"depth\":1,\"direction\":\"outbound\"}");
    ASSERT_NOT_NULL(require_text(depth1, "Mid.xaml"));
    ASSERT_NOT_NULL(require_text(depth1, "Other"));
    ASSERT_NOT_NULL(require_text(depth1, "Leaf.xaml"));
    ASSERT_TRUE(text_lacks(depth1, "Nested/Leaf.xaml"));
    ASSERT_EQ(tree_int_field(depth1, "edges"), 2);

    char *depth2 = uipath_tool(db_path, project, "uipath_invoke_graph",
                               "{\"workflow\":\"Main.xaml\",\"depth\":2,\"direction\":\"outbound\"}");
    ASSERT_NOT_NULL(require_text(depth2, "Nested/Leaf.xaml"));
    ASSERT_NOT_NULL(require_text(depth2, "pattern"));

    char *in_mid = uipath_tool(db_path, project, "uipath_invoke_graph",
                               "{\"workflow\":\"Mid.xaml\",\"depth\":1,\"direction\":\"inbound\"}");
    ASSERT_NOT_NULL(require_text(in_mid, "Main.xaml"));

    char *in_leaf =
        uipath_tool(db_path, project, "uipath_invoke_graph",
                    "{\"workflow\":\"Nested/Leaf.xaml\",\"depth\":2,\"direction\":\"inbound\"}");
    ASSERT_NOT_NULL(require_text(in_leaf, "Main.xaml"));

    char *both = uipath_tool(db_path, project, "uipath_invoke_graph",
                             "{\"workflow\":\"Mid.xaml\",\"depth\":1,\"direction\":\"both\"}");
    ASSERT_NOT_NULL(require_text(both, "Main.xaml"));
    ASSERT_NOT_NULL(require_text(both, "Nested/Leaf.xaml"));

    char *cycle =
        uipath_tool(db_path, project, "uipath_invoke_graph",
                    "{\"workflow\":\"CycleA.xaml\",\"depth\":4,\"direction\":\"outbound\"}");
    ASSERT_NOT_NULL(require_text(cycle, "CycleA.xaml"));
    ASSERT_NOT_NULL(require_text(cycle, "CycleB.xaml"));
    ASSERT_TRUE(text_lacks(cycle, "truncated"));
    ASSERT_EQ(tree_int_field(cycle, "edges"), 2);

    free(depth1);
    free(depth2);
    free(in_mid);
    free(in_leaf);
    free(both);
    free(cycle);
    free(project);
    th_rmtree(tmp);
    PASS();
}

SUITE(uipath) {
    RUN_TEST(uipath_expr_reads_config_and_writes);
    RUN_TEST(uipath_selector_risk_scores_idx);
    RUN_TEST(uipath_indexes_workflow_invoke_and_skips_noise);
    RUN_TEST(uipath_incremental_keeps_invoke_edges);
    RUN_TEST(uipath_parallel_keeps_invoke_edges);
    RUN_TEST(uipath_directory_invoke_is_unresolved);
    RUN_TEST(uipath_lint_orders_large_and_unresolved);
    RUN_TEST(uipath_lint_truncates_large_workflows);
    RUN_TEST(uipath_invoke_graph_walks_depth);
}
