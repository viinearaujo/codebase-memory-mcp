#ifndef CBM_UIPATH_H
#define CBM_UIPATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cbm_gbuf;

typedef struct {
    char reads[12][80];
    int nreads;
    char writes[8][80];
    int nwrites;
    char config_keys[8][160];
    int nconfig;
    char calls[8][96];
    int ncalls;
    char types[8][96];
    int ntypes;
} uipath_expr_facts;

/* lang_cs: 0 = VB, 1 = C#. is_write_site marks the whole expression as a write
 * (Assign.To / OutArgument). Names match case-insensitively when lang_cs is 0. */
void uipath_expr_analyze(const char *expr, int lang_cs, const char *const *var_names,
                         const char *const *var_types, int nvars, const char *const *arg_names,
                         const char *const *arg_types, int nargs, int is_write_site,
                         uipath_expr_facts *out);

typedef enum {
    UIP_XAML_ACTIVITY = 1,
    UIP_XAML_ARGUMENT,
    UIP_XAML_VARIABLE,
    UIP_XAML_META
} uipath_xaml_kind;

typedef struct {
    uipath_xaml_kind kind;
    char name[160];
    char id_ref[96];
    char id_source[16];
    char type_name[200];
    char clr_type[220];
    char package[96];
    char slot[48];
    char parent_id[96];
    char direction[16];
    char annotation[240];
    char breadcrumb[300];
    char protected_by[96];
    char catches[140];
    char x_name[96];
    char facts[4096];
    char root_kind[48];
    char x_class[220];
    char expr_lang[24];
    int start_line;
    int end_line;
    int ordinal;
    int activity_count;
    int parse_status; /* 0 ok, 1 partial, 2 truncated */
    int naming_ok;
} uipath_xaml_item;

typedef void (*uipath_xaml_emit_fn)(void *ud, const uipath_xaml_item *item);

/* Always returns 0. Malformed input yields partial items and parse_status. */
int uipath_xaml_scan(const char *src, size_t len, uipath_xaml_emit_fn emit, void *ud);

/* Index one config file (json, xlsx, or .config) into ConfigFile/ConfigKey nodes.
 * is_loaded is left false; the linker flips it after load-site detection. */
int uipath_config_index_file(struct cbm_gbuf *gb, const char *abs_path, const char *rel,
                             const char *project_qn);

int uipath_selector_risk(const char *text, char *reasons, size_t reasons_cap);

/* Workflow-to-workflow skeleton Jaccard. Emits SIMILAR_TO with strategy
 * uipath_skeleton. Caller deletes previous uipath edges first. */
void uipath_emit_similarity(struct cbm_gbuf *gb);

#endif
