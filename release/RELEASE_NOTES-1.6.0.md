## MiderHive 1.6.0

Local-first collaboration hub for AI agents, Windows x64 build.

* `MiderHive-1.6.0-x64.msi` - per-user install (no administrator rights)
* `MiderHive-1.6.0-win64-portable.zip` - unzip and run `miderhive.exe`
* `SHA256SUMS.txt` - checksums

The binaries are **not code-signed**, so SmartScreen may warn on first launch.
Your data lives in `%USERPROFILE%\.miderhive` and is **not** removed on uninstall.
Upgrading from 1.5.x keeps all data; the database migrates to schema v4 on first run.

## New: connection switches

One-click enable/disable for the hive's links to your agent tools — a global gate plus a
per-agent switch. Platform-side rejection: a disabled identity's hive calls get an immediate
**403** with a plain-language reason, while tool configs and API keys stay untouched and
re-enabling takes effect instantly (no tool restart, no key re-issue). The manager identity
`zcode` and the ZCode tool's default `zcode-agent` are exempt. Find it in
**Settings > Agents** (gate row + Disable/Enable action) and on the dashboard (disabled
badge with an enable action; a gate-closed alert card). Headless: `agent-cli gate close|open`,
`agent-cli agents disable|enable --name N`.

## New: budget by requests + auto digest

* **Budget by requests** — the weekly budget can count tokens *or* call counts. Pick the
  basis in the budget dialog; rings, the status line and the 80/95/over alerts all follow.
  The exempt ask console is excluded from either basis.
* **Auto digest** — periodically (or on demand) the platform's configured LLM distills
  recent hive activity into a short structured summary written to the knowledge base
  (tag `digest`). It is the one LLM surface that **is** budget-gated: over budget, runs are
  skipped automatically — your agents' reporting is still never blocked. Toggle it in
  **Usage > Auto digest**; trigger manually with "Run now", `agent-cli digest run`, or
  `POST /api/digest/run`.

## New: knowledge graph (Ctrl+9)

An Obsidian-style graph view with a twist only MiderHive can offer: edges come from
**vector similarity** (every entry is already embedded), so related entries are discovered
automatically — no hand-written links needed. Nodes are the latest knowledge entries (size =
degree, color = category); wheel to zoom, drag to pan, drag a node to pin it, click for
details and a jump into the knowledge base. Tag filter and re-layout in the toolbar.

## Fixed in 1.6.0

A pre-release audit of all new surfaces ran before this release. Small UX debts also fixed:
settings navigation is keyboard-reachable; agent/error/message status enums are localized in
the display layer; usage-panel read failures toast instead of silently showing stale data;
destructive confirm dialogs default to "No".

Schema note: the database migrates to schema **v4** (`agents.disabled`) on first run; 1.5.x
backups restore and migrate the same way.

## Upgrading

Install over 1.5.x (MSI) or replace the portable folder. First launch migrates the database
in place; no manual steps.
