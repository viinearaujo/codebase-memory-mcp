# Using a UiPath repo in Cursor

This is a walkthrough for opening a UiPath git repo in Cursor on Windows. You talk to the agent in plain language. It calls the UiPath tools on a local index of the project.

## What you get

From that index, the agent can answer:

- What calls this workflow?
- Who uses this argument, config key, queue, or asset?
- What breaks if I change this?

The index is built from `.xaml` workflows, coded workflows, and the config file a workflow actually loads. Studio and Orchestrator remain the tools for editing and running the automation.

## Set up once

Install on Windows with the existing steps under [Quick Start](../README.md#quick-start) in the README: download `install.ps1`, inspect it if you want, unblock it, and run it.

Restart Cursor. Confirm `codebase-memory-mcp` is listed in `.cursor/mcp.json`.

Open the git folder that contains the UiPath `project.json`. If the repo is a monorepo, open the folder that contains the projects.

In chat, say:

> Index this project.

Later prompts use the project name `list_projects` reports. That name is usually the folder name.

## A normal day

Six prompts you can paste. Each one says when to use it, then the prompt. The tool name after the prompt is what the agent calls.

### 1. First look

Use this when you have just opened the repo and want the shape of it.

> Give me a UiPath overview of this project.

`uipath_overview` returns the project roots, counts of workflows, activities, config keys, queues, and assets, and whether coverage is complete.

### 2. Explain one workflow

Use this when you want one workflow explained before you read it in Studio. Swap in the real file name.

> Explain Process.xaml.

The reply is an outline in prose, and the raw XAML stays in the file. This matches `explain_uipath_workflow`: the agent calls `uipath_workflow_outline` and writes from that outline.

### 3. Who uses a name

Use this before you delete or rename an argument, config key, asset, or queue. Put the real name in the prompt.

> Who uses the config key SharedFolder? I am about to rename it.

`uipath_find_usages` looks up that name. The same prompt works for an argument, an asset, or a queue: say which kind of name it is.

### 4. Invoke chain

Use this for the invoke chain from an entry workflow, outbound, with dynamic targets called out.

> Show the outbound invoke chain from Main.xaml, and call out dynamic targets.

`uipath_invoke_graph` walks outbound from that workflow and lists dynamic targets.

### 5. Before an edit

Use this when you are about to change an argument and need the rest of the edit list. Replace the argument and workflow names.

> I'm changing argument X on workflow Y. What else do I have to touch?

This matches `plan_uipath_change`. The agent calls `uipath_impact` for the edit list, then `uipath_find_usages` for every affected name, and `uipath_lint` for selectors and bindings.

### 6. Before a review

Use this before you review a change, when you want the structural findings in one pass.

> Lint this UiPath project: fragile selectors, broken argument bindings, config files that no workflow loads, and workflows over 30 activities.

`uipath_lint` reports those findings.

## How to read an answer

`completeness: lower_bound` means some Invoke Workflow targets are expressions the indexer could not resolve. Treat "nothing uses this" as incomplete until you check those dynamic sites in Studio.

Renaming a DisplayName leaves the node where it is. Studio `IdRef` is the id the index keeps for that activity.

In a Dispatcher/Performer repo, each `project.json` is its own root.

## What stays on your machine, and what is skipped

Indexing is local. Credential-shaped values are stored as `[redacted]`. Files under `.local/` are ignored.

WPF XAML is not a workflow. Expressions are scanned for reads, writes, and config keys. They are not fully compiled.

## When a file search is the right next step

Literal text in comments still needs a normal file search. So does a workflow the overview did not list.

Coded workflows are in the index. A class marked `[Workflow]`, `[TestCase]`, or `CodedWorkflow` is a workflow, and the graph points at its `Execute` method.

The node and edge model is in [docs/UIPATH.md](UIPATH.md).
