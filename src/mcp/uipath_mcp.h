#ifndef CBM_UIPATH_MCP_H
#define CBM_UIPATH_MCP_H

struct cbm_store;

/* Dispatch one uipath_* tool. `store` may be NULL (returns an error result).
 * `project` is the indexed project name. Returns a cbm_mcp_text_result string. */
char *cbm_uipath_dispatch_tool(struct cbm_store *store, const char *project, const char *tool,
                               const char *args_json);

/* Compact project summary appended to get_architecture when the uipath aspect
 * is requested. Caller frees. Empty string when the project has no UiPath roots. */
char *cbm_uipath_architecture_summary(struct cbm_store *store, const char *project);

#endif
