#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat_fs.h"
#include "pipeline/pipeline.h"
#include "pipeline/uipath.h"
#include "sqlite3.h"
#include "store/store.h"

#include <stdio.h>
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

SUITE(uipath) {
    RUN_TEST(uipath_expr_reads_config_and_writes);
    RUN_TEST(uipath_selector_risk_scores_idx);
    RUN_TEST(uipath_indexes_workflow_invoke_and_skips_noise);
    RUN_TEST(uipath_incremental_keeps_invoke_edges);
}
