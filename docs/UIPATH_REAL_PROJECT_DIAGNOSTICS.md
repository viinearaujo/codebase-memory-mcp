# UiPath diagnostics: Odyssey_Validator_Process

Index run: evening of 2026-10-03, this local tree, not a release binary. Source check for this note: 2026-10-04. The UiPath project was not modified and was not re-indexed for this note.

- Binary: `build/c/codebase-memory-mcp.exe`, built from this tree with MSYS2 MinGW gcc 16.2.0 via `scripts/build.sh` (no UI).
- Cache: `C:\Users\arauj\AppData\Local\cbm-odyssey-review`
- Project name: `C-Users-arauj-Documents-uipath-rpa.odyssey.lib-Odyssey_Validator_Process`
- Repo indexed, persistence false: `C:\Users\arauj\Documents\uipath\rpa.odyssey.lib\Odyssey_Validator_Process`
- Status ready. 8962 nodes, 95 workflows (92 XAML + 3 coded), 7644 activities, 332 arguments, 22 config keys, 8 packages, 4 assets, 2 queues, 13 dynamic targets, 0 stored invokes. The buffer held 21507 edges; the store kept 353. `uipath_overview` completeness is `lower_bound`.

Run note, already fixed in this tree, not an open recommendation: MinGW `_wfopen` rejects mode `wbx` (errno 22). `create_staging_path` in `src/pipeline/pipeline.c` calls `cbm_fopen(path, "wbx")`. The same mode string is still passed in `src/cli/config_json_like.c` and `src/cli/config_yaml_edit.c`. `cbm_fopen` in `src/foundation/compat_fs.c` (`cbm_fopen_exclusive_open_flags`) translates `x` to `_wsopen` with `_O_CREAT|_O_EXCL`. Leave that translation in place.

Extraction that already held on this index: project `Odyssey_Validator_Process`, entry `Main.xaml`, C#, Windows, and the overview prints REFramework. Eight packages, including Odyssey_Library 1.0.222, RpaFramework, RPA.Efiling.Objects, and tyler.intellidactcloud. The only config file is `Data/Config.xlsx` (22 keys, including `OrchestratorQueueName`). `Tests.xlsx` is absent. All 95 workflows have `parse_status` ok. Main's states exist as activity nodes: Initialization, Get Transaction Data, Process Transaction, End Process. Main arguments include `in_UseIntellidactC`, `in_IntellidactCConfig`, and `in_OdysseyAppArguments`. The InitAllSettings activity still carries the invoke fact `Framework\InitAllSettings.xaml` and the load of `Data\Config.xlsx`. `Test_GetCaseInformation` still names missing `Processes\SearchFiling\SearchFiling.xaml`, and that path is one of the 13 DynamicTargets. Four CSI invokes in AnyDocketToObject still name `CSI Framework\String\GetCaseInfoFromCaseNumber.xaml`. The OrchestratorQueueName assign keeps `out_Config["OrchestratorQueueName"]`. AddressParser, Execute, and ParseAddress were extracted from `RPA Framework/AddressParser.cs`.

## Required before uipath_invoke_graph and uipath_lint can be trusted

`uipath_invoke_graph` from `Main.xaml`, outbound, depth 4, returned 0 edges. Stored invoke count is 0, so it cannot be compared to the 222 `InvokeWorkflowFile` attribute sites. Fix R1 first. R2 and R3 are already visible on stored nodes and on the lint text. R4 is why a later non-zero result is still a single hop.

### R1. Relationship edges are dropped on dump

**Symptom.** Schema edges in the store are the file tree plus a few C# links: DEFINES 199, CONTAINS_FILE 102, CONTAINS_FOLDER 20, and 32 edges of other types (199+102+20+32 = 353). No `INVOKES_WORKFLOW`, `READS_CONFIG`, `USES_ASSET`, `PASSES_ARGUMENT`, or `INSTANCE_OF`. The InitAllSettings activity still has the invoke fact, so extraction ran and the dump dropped the edge. `IMPLEMENTED_BY` for AddressParser is the same drop until a post-R1 index says otherwise: a search restricted to `IMPLEMENTED_BY` returned nothing, while the C# file still has 3 `DEFINES_METHOD` and 3 `CALLS`.

**Mechanism (verified).** Parallel indexing (`run_parallel_pipeline` in `src/pipeline/pipeline.c`, selected when `worker_count > 1` and `file_count > 50`, `MIN_FILES_FOR_PARALLEL`) keeps a `shared_ids` atomic for worker buffers. The main graph buffer is created with `cbm_gbuf_new`, so `shared_ids` on that buffer is NULL (`src/graph_buffer/graph_buffer.c`). `alloc_next_id` then uses `gb->next_id++`.

The driver copies the atomic onto the main buffer after extract, and it publishes the registry watermark back into the atomic when registry nodes advanced it (the comment above that publish already describes this failure: a later `next_id` reset collides with serial nodes and orphans their edges). `cbm_pipeline_pass_uipath` then runs on `ctx->gbuf`, which is the main buffer (`src/pipeline/pipeline.c` sets `.gbuf = p->gbuf`). UiPath node ids are allocated from `next_id` and are never stored into `shared_ids`. `cbm_parallel_resolve` allocates from that same atomic, so resolve ids overlap the start of the UiPath range. After resolve, `cbm_gbuf_set_next_id(p->gbuf, atomic_load(&shared_ids))` writes the lower watermark back.

Dump uses that lowered value as the remap ceiling. `cbm_gbuf_dump_to_sqlite` sets `max_temp_id = gb->next_id`. `build_dump_nodes` still writes every live node, and it fills `temp_to_final[id]` only when `n->id < max_temp_id`. `remap_id` returns 0 at or past the ceiling. `build_dump_edges` skips any edge whose source or target remaps to 0. `cbm_gbuf_flush_to_store` and `cbm_gbuf_merge_into_store` use the same ceiling. The sequential driver (`run_sequential_pipeline`) does not rewind `next_id`, so a one-worker run hides this. The same rewind is in the parallel branch of `run_extract_resolve` in `src/pipeline/pipeline_incremental.c` (pass, then resolve, then `cbm_gbuf_set_next_id`).

Two nodes can share one temp id in the overlap. `temp_to_final` keeps one of them. Raising the dump ceiling without publishing the watermark still aliases that overlap.

**What to change.** After `cbm_pipeline_pass_uipath` returns, and before `cbm_parallel_resolve`, publish the main buffer's `cbm_gbuf_next_id` into `shared_ids` when it is higher. That is the same publish the registry block already does. On the post-resolve `cbm_gbuf_set_next_id`, assign the max of the atomic and the current `next_id`, so a later pass cannot rewind the watermark. Mirror both steps in `src/pipeline/pipeline_incremental.c`.

**How to tell it worked.** Re-index into a scratch cache. Leave the UiPath repo untouched. Stored edge count should sit with the buffer count (21507 on this run), aside from edges whose endpoints were removed from the qualified-name index. `INVOKES_WORKFLOW` from activities whose `file_path` is `Main.xaml` should include the InitAllSettings target. The source of that edge is the activity node (`link_all` inserts `acts[i].id`), and the activity's `file_path` is the workflow path, so a `Main.xaml` file filter is the right probe. Stored invoke count should be non-zero. Equality with 222 `InvokeWorkflowFile` sites is an R2 question: dynamic sites and the CSI basename links change the count. `tests/test_uipath.c` stays on the sequential path while its fixture is under 50 files, so a regression needs the parallel driver (`file_count > 50`, more than one worker) or CI will stay green.

**Blocked on R1.** G2 (`IMPLEMENTED_BY` for AddressParser). `uipath_lint` rule `binding.status` (it reads `PASSES_ARGUMENT`). Link rows in `uipath_activity_details`, `uipath_find_usages`, and `uipath_impact`. Any comparison of stored invokes to 222. Whether `READS` / `WRITES` / `USES_ASSET` edges kept the language tag.

**Independent of R1.** R2, R3, R4. Root kind, outline rows, workflow line span, expression fact tags, the DynamicTarget node list, the empty `framework` JSON field, and the coded-workflow count. Those are on node properties or in the tool SQL.

### R2. Four CSI invokes resolve as a unique basename

**Symptom.** Facts name `CSI Framework\String\GetCaseInfoFromCaseNumber.xaml`. That string is not among the 13 DynamicTargets. The only in-repo workflow with that filename is `RPA Framework/String/GetCaseInfoFromCaseNumber.xaml`. `SearchFiling`, which matches nothing, did become a DynamicTarget. `uipath_lint` never reports a missing file. After R1 the invoke graph would still draw those four sites as a successful link.

**Mechanism (verified).** In `link_all` (`src/pipeline/pass_uipath.c`), `join_under_root` turns backslashes into slashes, then `find_wf` requires the full path. On a miss, the code counts workflows whose `base_name(qn)` equals `base_name(resolved)`. `base_name` is the last `/` segment. One hit, and a raw string with no `(`, sets the target with `resolution` `pattern`, confidence 0.50, and `candidate` true. A path that names a directory is accepted when the filename is unique. Zero hits call `dyn_target`, which is why SearchFiling is a DynamicTarget and the CSI paths are not.

`tool_lint` in `src/mcp/uipath_mcp.c` has no query for a missing workflow path or a DynamicTarget. `selector.fragile` reads `Selector` nodes. `binding.status` reads `PASSES_ARGUMENT` edges (blocked on R1). A clean lint report after R1 still omits SearchFiling and the four CSI paths.

`tool_invoke_graph` maps `dynamic`, `folded`, and `literal_ci`. A `pattern` property falls through and is printed as `literal`.

**What to change.** In the invoke branch of `link_all`, a normalized path that contains a directory and misses `find_wf` (exact and case-insensitive) should take the same `dyn_target` path as SearchFiling. Keep a basename fallback only for a bare filename, if one is still wanted. Add a `uipath_lint` row for unresolved workflow paths (DynamicTarget values that are workflow paths, and any remaining `pattern` edges whose candidate file differs from the fact). Teach the invoke-graph resolution ladder the `pattern` token so a remaining candidate link is not labeled `literal`.

**How to tell it worked.** The four AnyDocketToObject activities whose facts contain `CSI Framework\String\GetCaseInfoFromCaseNumber.xaml` must not point at `RPA Framework/String/GetCaseInfoFromCaseNumber.xaml`. That CSI string should be unresolved (DynamicTarget or a lint missing-file row). SearchFiling stays unresolved. The in-repo `GetCaseInfoFromCaseNumber.xaml` workflow node stays; it is simply not the target of those four facts. The fact text is already stored, so this check does not need R1. After R1, the edge target must match that outcome.

**Blocked on R1.** Seeing the wrong edge in the store. The wrong resolution is already decided in `link_all` before dump.

**Independent of R1.** The fact text, the absence of the CSI string from the 13 DynamicTargets, and lint's lack of a missing-file rule.

### R3. uipath_lint says exact and drops the largest workflows

**Symptom.** `uipath_lint` returned 20 `refactor.large_workflow` hits and `completeness` `exact`. AnyDocketToObject (303 activities), `Framework/Process.xaml` (143), and `Processes/ProcessRobotSkills.xaml` (148) are over 30 and were not in that list. Those three numbers include AssemblyReference rows (B2); they are still above the threshold.

**Mechanism (verified).** `tool_lint` in `src/mcp/uipath_mcp.c` always finishes with `completeness` `exact`. The large-workflow query is `activity_count > 30` with `LIMIT 20` and no `ORDER BY`, so SQLite returns an arbitrary 20 rows. `scope` and `categories` are in the tool schema and ignored (`(void)args`).

`binding.status` selects `PASSES_ARGUMENT` edges whose properties contain `missing` or `mismatch`. Those edges are absent until R1. `selector.fragile` selects `Selector` nodes whose `risk_score` is not 0. That query is on nodes, which the dump writes even when edges are dropped. This run's 20 findings were all `refactor.large_workflow`, so the selector query added no rows. Do not treat the selector query as blocked on R1.

**What to change.** Order the large-workflow query by `activity_count` descending, and set `completeness` to a truncated/lower-bound value when the limit cuts the result (or drop the limit). Keep using `activity_count` only with B2 in mind: the counter includes reference rows. Leave `binding.status` empty-as-success untrusted until R1 has `PASSES_ARGUMENT` rows.

**How to tell it worked.** On this index, lint lists AnyDocketToObject, `Framework/Process.xaml`, and `Processes/ProcessRobotSkills.xaml`, and it does not say `exact` while the result is capped. This is visible without another index only if the tool is pointed at the existing cache; a code change needs a re-run of the tool, not a re-index, because `activity_count` is already on the workflow nodes.

**Blocked on R1.** `binding.status` only.

**Independent of R1.** The `exact` flag, the unordered `LIMIT 20`, and the three missing large workflows.

### R4. uipath_invoke_graph echoes depth and does not walk it

**Symptom.** The measured call was outbound depth 4 from `Main.xaml` and returned 0 edges. The 0 is R1. After R1 the same call still returns one SQL hop.

**Mechanism (verified).** `tool_invoke_graph` in `src/mcp/uipath_mcp.c` clamps `depth` to 1..8, prints it, and never puts it in the SQL. `direction` is in the tool schema (`inbound` / `outbound` / `both`, default outbound) and is never read. The query selects `INVOKES_WORKFLOW` edges whose source `file_path` or `qualified_name` equals the workflow argument, `LIMIT 100`. Sources are activities, so a `Main.xaml` file filter returns invokes inside Main only. It does not enter Process Transaction and continue to `Main_DataConvert`, `PrepareDocketRobotSkills`, `ExtractCaseData`, `SearchCase`, or `ProcessRobotSkills`. There is no truncation flag when the limit fills.

**What to change.** Walk `depth` hops, honor `direction`, and surface `pattern` (see R2). When the walk hits `LIMIT 100`, say the result is truncated. A one-hop query can stay as depth 1.

**How to tell it worked.** After R1, depth 1 from `Main.xaml` lists InitAllSettings and the other literal invokes in that file. Depth 4 also reaches Process's callees listed in the ground-truth section. Inbound from `Framework/InitAllSettings.xaml` lists Main. This Odyssey check waits on R1. The unused parameters are visible in `tool_invoke_graph` now.

**Blocked on R1.** A live Odyssey walk.

**Independent of R1.** The SQL shape. Unit-test the walker on a small fixture once R1 persists edges at all; a sequential fixture is enough for the walker and is not enough for R1.

## Further findings

### B1. root_kind is Collection, and there are no transition edges

**Evidence.** `root_kind` is Collection for Main, `Framework/Process.xaml` (143 activities), `Processes/ProcessRobotSkills.xaml` (148), AnyDocketToObject (303), and `Framework/RetryCurrentTransaction.xaml` (41). States and sequences are nested activities. No transition edges were stored.

**Source.** `uipath_xaml_scan` in `src/pipeline/uipath_xaml.c` marks a non-property, non-designer element as an activity. `xmeta_local` does not list `Collection` or `AssemblyReference`. The references block is `TextExpression.ReferencesForImplementation` (a property, skipped) whose children are `Collection` and `AssemblyReference` (emitted). `root_kind` is set from the first non-collapsed activity local name, and it is replaced only while the current value is empty or `Activity`, so `Collection` sticks. `on_xaml_item` copies that onto the workflow. The scanner never inserts a transition or flow-step edge. State, StateMachine, Flowchart, and FlowStep are ordinary activities. This is independent of R1: the edges were never created.

**Decision:** implement.
**Risk of doing it:** treating every `Collection` as non-root can hide a workflow whose real root activity is a collection. Gate the skip on the references property slot.
**Risk of skipping it:** overview consumers keep reading Collection for a state machine and a flowchart, and state order stays a parent/child activity list.

### B2. Outlines are filled with AssemblyReference rows

**Evidence.** An 80-row window for `Process.xaml` never reaches a real activity. Main's window reaches StateMachine "General Business Process", State "Initialization", and "Invoke InitAllSettings workflow". The other three Main states exist as nodes outside that window.

**Source.** `uipath_xaml.c` emits `AssemblyReference` as activities (B1). `tool_outline` in `src/mcp/uipath_mcp.c` selects activities for the file `ORDER BY start_line, qualified_name LIMIT 80`. Reference elements sit at the top of the XAML, so they occupy the window. The same activity counter is `activity_count` on the workflow, which R3 thresholds. Independent of R1.

**Decision:** implement.
**Risk of doing it:** a real activity that is literally named AssemblyReference would leave the outline. Skip by CLR/namespace (`System.Activities` reference collections), not by display name alone.
**Risk of skipping it:** `uipath_workflow_outline` on Process stays a reference list, and `activity_count` keeps inflating R3.

### B3. Every Workflow node is stored at lines 1-1

**Evidence.** Every Workflow node is lines 1-1. The Module node for `GetCaseInfoFromCaseNumber` still spans 1-926.

**Source.** `finish_workflow_node` in `src/pipeline/pass_uipath.c` upserts the workflow at start line 1 and end line `max_line` (the real end). `link_all` then calls `up_upsert` for every workflow with start 1 and end 1, to attach `member`. `copy_label` keeps `start_line` only, so `link_all` does not have the previous end line. `cbm_gbuf_upsert_node` in `src/graph_buffer/graph_buffer.c` treats a full tie (same file, same start line, same name, same label) as the same entity and refreshes in place, including `end_line`. Both calls use start line 1, so the refresh writes end line 1. The Module span is a different node from the definitions pass. Independent of R1. `tool_outline` prints these start and end lines.

**Decision:** implement.
**Risk of doing it:** changing the general tie rule in `cbm_gbuf_upsert_node` can disturb C/C++ same-name entities. Pass the existing end line from `link_all` instead, and keep the upsert tie rule.
**Risk of skipping it:** every workflow outline reports lines 1-1 while the file span lives on the Module node.

### B4. expr_lang is mixed, and C# facts are tagged vb

**Evidence.** `expr_lang` is mixed on Main, Process, ProcessRobotSkills, AnyDocketToObject, and RetryCurrentTransaction (50 of 95 workflows). The OrchestratorQueueName assign has C# expr facts and a write fact tagged `vb`. The SelectTokens invoke has an expr tagged `vb` for `rows[*].columns[*].Value`. GetRobotAsset facts are `Row["Asset"].ToString()`. The four Asset nodes (`odyssey_credentials`, `odyssey_sequenced_db_connectionnames`, `odyssey_customerconfig_storagefilename`, `efile_db_credentials`) match config keys and have an empty file path.

**Source.** `uipath_xaml_scan` sets `expr_lang` to `mixed` when it sees both a `CSharp*` carrier and any other expression carrier (`InArgument`, `Literal`, `VisualBasicValue`, and the rest of `expr_local`). `attach_expr_facts` tags a non-`CSharp*` carrier as `vb`. `note_resource_attrs` always writes `write\tvb` for `Assign`, and `expr\tvb` for every attribute whose value starts with `[`. Asset and queue folding, and `invoke_expr` folding, call `uipath_expr_analyze` with `lang_cs` 0 (`src/pipeline/uipath.h`: 0 means VB, case-insensitive names). Asset nodes are upserted with an empty file path in that same branch. Fact tags are already on the activity nodes. The edges those analyzers create are blocked on R1.

**Decision:** implement.
**Risk of doing it:** a VB project that uses bracket attributes would start being analyzed as C# if the flag is taken from the project language blindly. Drive `lang_cs` from the project `expressionLanguage` (this project is C#) and from `CSharpValue` / `CSharpReference` carriers, and stop hardcoding `write\tvb` on Assign.
**Risk of skipping it:** C# assigns stay tagged `vb`, and expression invokes fold with the VB analyzer. Literal path invokes (InitAllSettings, the CSI strings) do not go through that analyzer.

### T1. uipath_activity_details hides the facts that survived

**Evidence.** `uipath_activity_details` for `Main.xaml#InvokeWorkflowFile_1` and `Framework/InitAllSettings.xaml#Assign_13` returns display name, file, and line. The invoke target, the `Data\Config.xlsx` load, and `out_Config["OrchestratorQueueName"]` are only on the activity `facts` property. GetRobotAsset facts are `Row["Asset"].ToString()`.

**Source.** `tool_activity` in `src/mcp/uipath_mcp.c` selects `properties` and uses them for `protected_by`. It prints a links table from outgoing edges (empty until R1) and sets `completeness` to `exact`. The facts string is already on the node.

**Decision:** implement.
**Risk of doing it:** printing the whole facts blob can flood the tool result on a large activity. Print invoke, load, write, asset, and queue lines, with a cap.
**Risk of skipping it:** the two probed activities keep looking empty aside from name, file, and line, including after R1 fills the links table.

### G1. DynamicTarget mixes a missing workflow with argument names

**Evidence.** One DynamicTarget is the missing path `Processes\SearchFiling\SearchFiling.xaml`. The other names are argument-shaped: `in_Config`, `io_RpaTransaction`, `io_Docket`, `out_flEnvelopeFound`, `address`, `Output`, and others. All thirteen have an empty file path.

**Source.** `dyn_target` in `src/pipeline/pass_uipath.c` upserts label `DynamicTarget` with file `""`. Unresolved invokes call it with the path (SearchFiling). The bind walk calls it with the argument name when the name is not an argument of the resolved target (`status` `extra`). Those argument nodes are why the list is mixed. `uipath_overview` and `uipath_invoke_graph` set `lower_bound` whenever any DynamicTarget exists. SearchFiling alone would already do that. Independent of R1 for the node list. After R1 the extra-bind branch also emits `PASSES_ARGUMENT` to those nodes.

**Decision:** implement, scoped to the bind-extra branch. Leave the unresolved-workflow `dyn_target` call so SearchFiling stays.
**Risk of doing it:** a workflow expression that is only an argument name would no longer be a DynamicTarget if the path branch is changed too. Keep that branch.
**Risk of skipping it:** the dynamic list is not a list of missing workflows, and completeness stays `lower_bound` for argument names as well as SearchFiling.

### G2. AddressParser is not linked to Execute

**Evidence.** No `IMPLEMENTED_BY` row. The C# file has 3 `DEFINES_METHOD` and 3 `CALLS`. AddressParser, Execute, and ParseAddress were extracted.

**Source.** `link_all` inserts `IMPLEMENTED_BY` from a workflow whose kind is `coded` or `test_case` to a `Method` named `Execute` in the same file. AddressParser is a coded workflow with `Execute`. The edge source is the workflow id allocated in `pass_uipath`, so R1 drops it the same way it drops invoke edges. Confirm after R1. If the edge is present then, close this item.

**Decision:** defer.
**Risk of doing it before R1:** a second writer for the same edge, then two rows once dump is fixed.
**Risk of skipping it after R1 shows it still missing:** coded workflows stay disconnected from `Execute` even though `docs/UIPATH.md` describes that link.

### G3. entry-points.json is never compared to project.json

**Evidence.** Nothing records an `entry-points.json` versus `project.json` disagreement. Three extra arguments are on Main. Project entries are `Main.xaml` twice plus `RPA Framework/AddressParser.cs`. The `framework` property queried back empty while the overview still prints REFramework.

**Source.** `parse_project_json` in `src/pipeline/pass_uipath.c` reads `main` and the `entryPoints` array inside `project.json`. It adds `main` to the entries list and adds each `entryPoints` path again, so Main appears twice when it is also an entry point. There is no reader for `entry-points.json` anywhere under `src/`. `slot->framework` is only the zeroed buffer; the first project JSON write stores `"framework":""`. When `link_all` sees `InitAllSettings.xaml` or an activity named `Get Transaction Data` or `Process Transaction`, it appends a second `"framework":"REFramework"` key. `yyjson_obj_get` returns the first key. `tool_overview` prints REFramework because `strstr(props, "REFramework")` hits the second key. The duplicate key is independent of R1. The entry-point file diff is a new feature.

**Decision:** reject-if the work is an `entry-points.json` diff or a reconciliation of the three extra Main arguments. The duplicate `framework` key is B5.
**Risk of doing the file diff:** a new policy for which file wins, with no product rule in this repo.
**Risk of skipping the file diff:** the three extra Main arguments and any entry-list drift stay unrecorded. The overview will keep printing REFramework via the substring.

### B5. Stored framework field is an empty duplicate key

**Evidence.** Overview prints REFramework. A query of the `framework` property came back empty.

**Source.** Same splice as G3: `parse_project_json` writes `"framework":""`, and `link_all` appends a second key. Replace the existing key, or set `slot->framework` before the single write. Independent of R1.

**Decision:** implement.
**Risk of doing it:** a careless rewrite of project properties can drop `dependencies` or `entries`.
**Risk of skipping it:** `json_extract` of `framework` stays empty while the overview text says REFramework.

### G4. Odyssey_Library is only the package node

**Evidence.** Odyssey_Library is the package node at 1.0.222. Sibling workflows are not in this graph.

**Source.** `parse_project_json` stores NuGet dependencies as `Package` nodes. It does not index other projects. Index the sibling only after R1 leaves `INVOKES_WORKFLOW` in the store; until then a second project adds nodes whose relationship edges would be dropped the same way.

**Decision:** reject-if this pass starts indexing the sibling library.
**Risk of doing it now:** a larger graph with the same empty invoke list.
**Risk of skipping it:** cross-project calls into Odyssey_Library stay unresolved. That is acceptable until R1 is proven.

### T2. Generated connection files count as workflows

**Evidence.** Workflow count is 95: 92 XAML plus 3 coded. Two of the coded nodes are `.codedworkflows/ConnectionsFactory.cs` and `.codedworkflows/ConnectionsManager.cs`.

**Source.** `scan_coded` in `src/pipeline/pass_uipath.c` treats a file as a workflow when `strstr` finds `[Workflow]`, `[TestCase]`, or `CodedWorkflow`. `ConnectionsFactory.cs` is `using UiPath.CodedWorkflows;` and an empty namespace. `ConnectionsManager.cs` is that using plus a class that takes `ICodedWorkflowsServiceContainer`. The substring `CodedWorkflow` matches both. `finish_workflow_node` stores kind `coded`. The third coded workflow is `RPA Framework/AddressParser.cs`, which has `Execute` and should stay.

**Decision:** defer.
**Risk of doing it:** a real coded workflow whose only marker is the `UiPath.CodedWorkflows` import would disappear. Require `[Workflow]`, `[TestCase]`, or a class base that is `CodedWorkflow`, and do not treat the namespace import as a workflow.
**Risk of skipping it:** overview workflow count stays 95, including two connection stubs.

## Ground truth

Use this when checking a re-index. It was measured outside the graph.

- 92 XAML files. 222 `InvokeWorkflowFile` attribute sites. Expressions are `CSharpValue`.
- Main is a state machine: Initialization, Get Transaction Data, Process Transaction, End Process.
- `Framework/RetryCurrentTransaction.xaml` is a flowchart.
- Process calls `Main_DataConvert`, `PrepareDocketRobotSkills`, `ExtractCaseData`, `SearchCase`, and `ProcessRobotSkills`.
- Known bad targets: `Processes\SearchFiling\SearchFiling.xaml` is missing. Four CSI paths `CSI Framework\String\GetCaseInfoFromCaseNumber.xaml` are stale. The in-repo file with that filename is `RPA Framework/String/GetCaseInfoFromCaseNumber.xaml`.

## Checked, and not the cause

- Per-activity facts are a 4096-byte buffer (`xframe.facts` in `src/pipeline/uipath_xaml.c`). `fact_add` stops when it is full. The CSI lines and the InitAllSettings invoke line were still on the activity facts. All 95 workflows are `parse_status` ok. No workflow was truncated.
- `link_all` analyzes at most 64 variables and 64 arguments per activity. Main has 6 arguments. Sampled facts were short. Those caps are not visible on this project.
- `uipath_xaml.c` sets `naming_ok` false when an In/Out/InOut argument lacks the `in_` / `out_` / `io_` prefix. The REFramework arguments that use those prefixes are stored with direction In. `tool_lint` does not read `naming_ok`. Do not add a lint rule that flags those names.

## Decision table

Defaults: do now only for defects that make `uipath_invoke_graph` or `uipath_lint` wrong; after R1 when the store cannot answer yet; skip for product expansions and for defects that do not gate those two tools.

| id | tag | blocked tools | default |
| --- | --- | --- | --- |
| R1 | bug | `uipath_invoke_graph`, `uipath_lint` (`binding.status`), `uipath_find_usages`, `uipath_impact`, `uipath_activity_details` links, overview invoke count | do now |
| R2 | bug | `uipath_invoke_graph`, `uipath_lint` | do now |
| R3 | tweak | `uipath_lint` | do now |
| R4 | bug | `uipath_invoke_graph` | do now |
| B1 | bug | workflow `root_kind`; no transition edges exist to query | skip |
| B2 | bug | `uipath_workflow_outline`; feeds R3 `activity_count` | skip |
| B3 | bug | `uipath_workflow_outline` line span | skip |
| B4 | bug | expression folds and future `READS` / `WRITES` / asset edges | after R1 |
| T1 | tweak | `uipath_activity_details` | skip |
| G1 | gap | overview and invoke-graph `completeness`; `PASSES_ARGUMENT` targets after R1 | do now |
| G2 | gap | `IMPLEMENTED_BY` (AddressParser to Execute) | after R1 |
| G3 | gap | none until an entry-points diff is a product requirement | skip |
| B5 | bug | `framework` JSON field versus overview text | skip |
| G4 | gap | cross-project calls into Odyssey_Library | skip |
| T2 | tweak | overview workflow count (95 versus 92 XAML + AddressParser) | skip |
