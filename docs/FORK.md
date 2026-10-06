# Why this fork exists

This file is the rationale for Vinicius's personal fork of codebase-memory-mcp. It sits next to [UIPATH.md](UIPATH.md), which describes the graph. DeusData's product story stays in the upstream README.

Work around this fork is Cursor, the UiPath skills, and the `uip` CLI, on projects shaped by [ViniUiPathProjectKit](https://github.com/viinearaujo/ViniUiPathProjectKit). The fork is the cheap way to learn a process someone else already built, or to revisit one of yours, without the model reading the tree file by file.

## Job

The job is local structural recall for a UiPath repo.

`uipath_overview`, `uipath_workflow_outline`, `uipath_find_usages`, `uipath_invoke_graph`, `uipath_impact`, and `uipath_lint` answer the overview, the outline, who uses a name, the invoke chain, the impact of a change, and lint. The model spends tokens on that answer. It does not spend them walking XAML.

## Not the job

Authoring, debugging, validate, build, run, and package belong to the UiPath skills and to `uip`.

Decisions for a work project stay in that project's `docs/adr/`. Leave `manage_adr` out of those decisions. `manage_adr` stores notes inside this server. The project's ADR folder is the record the team keeps.

## Why UiPathEngineeringMCP is parked

UiPathEngineeringMCP reimplemented analyze, author, validate, and compile beside the skills and the CLI. Cursor, the skills, and `uip` already cover that development work. A second MCP that duplicates them costs more than it saves. The skills are the better authoring surface.

Leave that server parked. Do not revive it, and do not port its writer, its Roslyn host, its Copilot connector, or a gap whose fix is to move logic into C#.

## Split

This fork owns the graph. The UiPath skills own procedure: how a file is written and changed. `uip` is the only execution path: validate, build, run, and package. [ViniUiPathProjectKit](https://github.com/viinearaujo/ViniUiPathProjectKit) owns team conventions, including the ADRs under the kit.

## XAML-majority

New and existing work projects keep at least 75% of the automation in `.xaml`, so a less experienced developer can maintain the process.

Coded workflows are the exception. Use one when the same behavior in XAML would be harder for that developer to change.

Do not add findings that tell the agent to move business logic into C#.

## Next build

After this note, the next build makes the XAML graph trustworthy. The open items are written up in [UIPATH_REAL_PROJECT_DIAGNOSTICS.md](UIPATH_REAL_PROJECT_DIAGNOSTICS.md).

- **R1.** Stored invoke edges survive the index. Today a parallel dump can drop them, so the invoke graph has nothing to walk.
- **R2.** A workflow path that contains a directory and does not match a file stays unresolved.
- **R3.** Lint does not claim `completeness: exact` while the result is truncated.
- **R4.** Invoke walks honor `depth` and `direction`.
- **Shape.** StateMachine and Flowchart structure is stored, so a REFramework `Main` reads as states and transitions.

Borrow the read policy from the parked server's `DependencyGraphBuilder`, and implement it in this tree's C read path. Match a project-relative path, case-insensitive. Fall back to a unique file name only when the reference is a bare name. When the path contains a directory and misses, leave it unresolved.

Leave the C# server where it is.
