# UiPath hybrid graph

Everyday setup and paste-ready prompts are in [UIPATH_GUIDE.md](UIPATH_GUIDE.md).

Workflow Foundation XAML is indexed as a workflow-and-code graph, not as generic XML.

## What gets indexed

A `project.json` that contains `expressionLanguage`, `studioVersion`, or a `UiPath.` dependency is a project root. Paths in that project resolve against the root, so a Dispatcher/Performer monorepo keeps each project separate. Files under `.local/` are ignored.

Each `.xaml` that uses the Workflow Foundation activities namespace becomes a `Workflow`. Activities, arguments, and variables are separate nodes. Ids come from Studio `IdRef` values, so a DisplayName rename does not change the node. WPF XAML is left to the generic XML extractor.

Coded workflows (`[Workflow]`, `[TestCase]`, `CodedWorkflow`) become `Workflow` nodes too, with `IMPLEMENTED_BY` pointing at the `Execute` method.

## Edges

`CALLS` stays the workflow-to-workflow traversal edge, with a `sites` count. Each invoke site is also an `INVOKES_WORKFLOW` edge from the activity, so two calls to the same workflow stay distinct. Unresolved expressions become `DynamicTarget` nodes. Tools then report `completeness: lower_bound` instead of claiming the search was exhaustive.

Expressions are lexed, not parsed. The lexer records `READS`, `WRITES`, `READS_CONFIG`, and argument bindings (`PASSES_ARGUMENT`) with a status of ok, missing, extra, or a mismatch.

`Config.json` and `Config.xlsx` both become `ConfigKey` nodes with the same `lookup_key`. Only a file a workflow actually loads is authoritative. Asset, queue, and other Orchestrator names become their own nodes. Selector text is stored with a risk score. Credential-shaped values are stored as `[redacted]`.

## Tools

| Tool | Use |
| --- | --- |
| `uipath_overview` | Project roots, counts, and coverage gaps |
| `uipath_workflow_outline` | One workflow as an activity outline |
| `uipath_activity_details` | Expressions, reads, writes, selectors, bindings |
| `uipath_find_usages` | Who uses a workflow, argument, config key, asset, or queue |
| `uipath_invoke_graph` | Invoke edges, including dynamic ones |
| `uipath_impact` | What an argument, workflow, config, or package change touches |
| `uipath_lint` | Fragile selectors, binding errors, unused config, large workflows |

Scout can call overview, outline, and find-usages. Analysis can call all seven. `get_code_snippet` outlines a `Workflow` unless `source_mode` is `full`. `get_architecture` accepts a `uipath` aspect. `search_graph` hides `Activity`, `Variable`, `Argument`, and `Selector` unless a `label` is passed.

Prompts: `explain_uipath_workflow` and `plan_uipath_change`.
