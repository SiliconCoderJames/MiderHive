# Changelog

All notable changes to **MiderHive** are documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Version numbers are `MAJOR.MINOR.PATCH`: `MAJOR` for incompatible changes, `MINOR` for
backwards-compatible features, `PATCH` for backwards-compatible fixes. Entries are grouped into
**Added**, **Changed**, **Fixed** and **Security**.

- **Downloads and full release notes**: <https://github.com/SiliconCoderJames/MiderHive/releases>
- **Project overview and getting started**: [README.md](README.md) · [README.zh-CN.md](README.zh-CN.md)

MiderHive is a local-first collaboration hub for AI agents: it binds `127.0.0.1` only, keeps all
state in one SQLite file, and lets Claude / Codex / Cursor / Copilot / ZCode or your own scripts
join the hive over HTTP or MCP.

> **Brand history:** releases **1.0.0** and **1.0.1** shipped as **AgentHive**
> (`AgentHive-1.0.0/1.0.1-*` artifacts, `agenthive.exe`, `%USERPROFILE%\.agenthive`,
> `AGENTHIVE_*` variables); 1.0.0 was published as a pre-release. From **1.0.2** onward the
> project ships as **MiderHive** (`miderhive.exe`, `MIDERHIVE_*` variables,
> `%USERPROFILE%\.miderhive`). The old `AGENTHIVE_*` / `ZCODE_*` names and the old data
> directory are recognised and migrated automatically.

## [Unreleased]

### Added
- **Nine more MCP tools (18 → 27)**, closing the loops an MCP-only agent previously could not
  reach: `usage_report` (with idempotency), `knowledge_add_version` / `knowledge_get` /
  `knowledge_versions` (refine shared know-how instead of duplicating it), `skill_register`
  (publish to the skill market), `message_reply`, `usage_daily`, `usage_breakdown`, and
  `hive_status` — one call that answers "am I connected, who else is here, what did we spend".
- **One-stop connect wizard** (`ConnectDialog`, also reachable via Settings → Agents): pick a
  tool, click once, and it provisions the identity, emits that tool's **own** config format
  (JSON / Codex TOML / DSH profile patch / Hermes YAML / env vars / instructions block), and —
  where the file can be merged safely — **writes it for you**, backing the original up as
  `.miderhive.bak`. The panel polls the platform and flips to a live **"Connected"** when the
  agent comes online.
- **Eight-client integration registry** as the single source of truth (`integrations.h`),
  covering Claude, ChatGPT / Codex, Factory Droid, DSH, Hermes and ZCode — plus Cursor and
  GitHub Copilot — with per-client install detection and real config-path resolution.
- **Qt's own translations are now installed** (`qtbase_zh_CN.qm`), so Qt-supplied strings
  (dialog OK/Cancel, message-box buttons, input context menus) follow the UI language instead of
  staying English in Chinese mode.

### Changed
- First-run onboarding now offers all eight clients (grid layout) and can auto-write the config
  from the welcome dialog too.
- `humanError` gained disk-full and permission-denied diagnoses, and its port branch no longer
  swallows any message merely containing "address".

### Fixed
- **Codex TOML config was unusable for real Windows paths**: it emitted basic (double-quoted)
  strings, where `\Q` is an invalid escape, so the whole `config.toml` failed to parse. It now
  emits TOML literal (single-quoted) strings; a self-test asserts both the literal form and that
  the double-quoted form is never produced.
- **`skills_panel` filter sentinel**: the category/owner combos used the item text `"全部"` as the
  "no filter" sentinel, so translating it would have silently broken filtering in English. The
  item text and the comparison are now bound to one translated value.
- `memory_panel`'s section list held Chinese display names in a static-init table, which would
  have frozen the startup language; it is now a `{zh, en, key}` table read at use time.
- Documented — and worked around — a real Claude Code trap: in PowerShell the npm `claude.ps1`
  shim swallows everything after `--`, producing
  `error: missing required argument 'commandOrUrl'`. The docs now lead with the project
  `.mcp.json` route (verified: Claude Code lists the server and reports `√ Connected`) and the
  CLI form is marked cmd.exe-only.

### Documentation
- `docs/mcp.md` rewritten: per-client setup for all eight tools, the 27-tool inventory grouped by
  loop, and a DSH-specific warning that its subprocess environment is scrubbed, so the key must
  live in the config's `env` block.

## [1.1.1] - 2026-09-13

### Added
- **First-run onboarding**: on the very first launch the workbench offers a connect guide — pick
  **Claude Code / Cursor / Codex CLI**, and it detects the local install, provisions the agent
  identity and generates the matching MCP config (JSON for Claude/Cursor, TOML for Codex) with a
  copy button and the config-file location.
- **"Connected" confirmation**: once the agent makes its first contact, the workbench pops a
  "Connected" notice and writes a **welcome memory** entry into *User Memory → Project*
  automatically (once per identity).
- **Settings → Agents → *Reopen onboarding*** brings the connect guide back at any time.
- **Startup self-check**: data-directory writability, `agents.json` readability, port
  availability and database integrity are checked before the window opens; problems surface as
  one dismissible, bilingual dialog with next-step advice.
- **Health banners** on the overview, shown only when something is actually wrong (service down,
  data directory not writable, database trouble, unreadable key cache, per-agent key loss), with
  an action on every row.
- **Lost plaintext key → one-click rotation**: an agent whose key-cache entry is gone gets a
  banner with *Fix: rotate key*; rotating invalidates the old key immediately and hands over the
  new key to paste into the agent's config.
- **Offline agents explain themselves**: right-clicking an offline agent shows the concrete
  diagnosis (with a fix entry when one applies) before the actions.
- **Errors speak human**: technical failures (locked database, occupied port, key mismatch,
  version conflict, …) are translated into plain English/Chinese with the next step, and nothing
  fails silently.
- **Guided empty states**: the knowledge, skills, memory and errors panels explain what the
  module is for and offer the obvious action (new entry / register skill / write memory / manual
  report) instead of a blank page.
- **Verification tooling**: 35 offscreen GUI assertions (`gui_selftest` +
  `scripts/verify_gui_selftest.py`) drive the real UI headlessly through onboarding → agent
  online → "Connected" popup + welcome memory → keyfile-loss banner → rotate fix (old key 401,
  new key 200) → Settings re-entry; 14 platform assertions
  (`scripts/test_onboarding_and_safety.py`) cover provisioning, heartbeat, welcome memory,
  keyfile-loss diagnostics and key rotation over HTTP; `scripts/capture_screenshots.py` generates
  the README screenshots from seeded demo data.

### Changed
- Unit-test coverage grew to **291 assertions** (from 243), and the packaging pipeline was
  re-verified end to end.

### Fixed
- The keyfile-loss health banner could stay on screen forever after the fix: the "healthy"
  signature equals the cleared one, short-circuiting the rebuild. A dirty flag now forces one
  rebuild after every fix action.
- The brand icon resource was registered under a mangled alias (`:/brand/../../docs/assets/…`), so
  `:/brand/logo.png` never resolved — the main window silently fell back to the vector hive, the
  tray icon was blank and the onboarding/settings logos were empty. Both executables now register
  the logo under its proper alias.

## [1.1.0] - 2026-09-13

### Added
- **Usage analytics page** (`Ctrl+2`): slice token consumption by time range (7 / 14 / 30 days),
  agent and model, with every view re-aggregating on the spot; weekly budget ring; three KPI
  tiles (spend with input/output split, call count, average per call); daily trend chart and
  model distribution chart that both follow the filters; per-agent breakdown table
  (input / output / total / calls / share, sortable by any column, share colour-coded by
  consumption). Double-click a row to filter the page to that agent.
- **Budget editor**: the *Adjust budget* button on both the overview and the usage page opens a
  dialog with a thousands-separated field and 1M / 5M / 10M / 50M / 100M presets. Until now the
  weekly budget could only be changed by editing the database or running a script. Budget changes
  are audit-logged.
- API: `GET /api/usage/breakdown?days=&agent=&model=` returns the same aggregate the usage page
  uses (totals, per-agent, per-model, per-day) from one consistent query.

### Changed
- **Visual hierarchy**: metric tiles gained trend sparklines (tokens) and ratio micro-bars
  (agents online/offline, error severity mix); numbers are drawn in a monospaced face so columns
  align; period-over-period badges state what they compare against in their tooltip.
- **Layout**: the budget ring became a compact card (ring + used / left / share + 14-day mini
  trend) instead of a large, low-information block; the event stream is capped so it no longer
  eats the first screen.
- **Interaction**: charts keep their hover readouts and gained click-through — click an agent bar
  to open the usage page filtered to that agent, a model bar to filter by model, a trend bar to
  open the usage page, an alert card to jump to the error list. The event stream filters by type
  (memory / knowledge / skills / messages / errors / agents) and a double-click jumps to the
  matching panel.
- Offline agent cards offer actions (copy the connect command, copy the name, view that agent's
  usage); empty states explain what will appear and what to do next; a skeleton placeholder
  covers the first load.
- The application icon ships seven sizes (16/24/32/48/64/128/256) instead of a single 256px frame,
  so desktop and Start-menu shortcuts are crisp rather than scaled, with a simplified glyph below
  24px where the honeycomb detail cannot resolve.
- **Updater resilience**: release download URLs are case-sensitive about the tag, so a manifest
  pointing at `v1.0.4` fails when the tag is `V1.0.4`. The packager now follows the tag that
  actually exists in the repository and the updater retries with the case-flipped tag as a
  fallback, so existing releases keep working either way.
- All four executables now carry proper `VERSIONINFO` metadata (product/file version,
  description, copyright), so Explorer's *Product version* column and file properties are filled
  in.

### Fixed
- A duplicated "Errors" entry in the left navigation (caused by a hard-coded index).
- **Dashboard stability**: small windows no longer push the event stream and alerts below the
  fold (word-wrapped labels no longer inflate card minimum heights), and the event stream stops
  rebuilding itself when nothing changed — the 3-second refresh no longer flickers or resets your
  selection.

### Security
- **MCP server hardening**: malformed `tools/call` arguments can no longer crash the server;
  `config/agents.json` is written atomically and a corrupted key file is refused rather than
  overwritten; multi-step writes run inside transactions with rollback on failure.

## [1.0.5] - 2026-09-13

*No `v1.0.5` tag or published release exists — this section dates from the release-notes file,
and the changes below are also described in the 1.1.0 notes.*

### Added
- Usage panel (new page, `Ctrl+2`) with time-range (7 / 14 / 30 days), agent and model filters, a
  weekly budget ring, three KPI tiles, daily trend and model distribution charts, and a sortable
  per-agent breakdown table.
- Budget editor (*Adjust budget*, thousands-separated field, 1M / 5M / 10M / 50M / 100M presets,
  audit-logged).
- API: `GET /api/usage/breakdown?days=&agent=&model=`.

### Changed
- Workbench UI upgrade: trend sparklines and period-over-period badges, monospaced numbers, a
  compact budget card, a capped event stream, hover readouts plus click-through charts, an
  event-stream type filter, offline-agent card actions, empty states and a skeleton placeholder.
- The application icon ships seven sizes (16/24/32/48/64/128/256) with a simplified glyph below
  24px.
- Updater resilience to tag-casing drift (`v1.0.4` vs `V1.0.4`).

### Fixed
- A duplicated "Errors" entry in the left navigation (hard-coded index).

## [1.0.4] - 2026-09-12

### Added
- **MCP server (`miderhive-mcp.exe`)**: an 18-tool stdio server — agent list & heartbeat, shared
  memory (write / history / conflict-safe remove), knowledge base (keyword + semantic search),
  messaging (broadcast by omitting the recipient), error reports (full lifecycle), skills and
  token usage. Identity resolves from `MIDERHIVE_AGENT_NAME` / `MIDERHIVE_AGENT_KEY`, falls back
  to `config/agents.json`, and can auto-register with `MIDERHIVE_MASTER_KEY`. Setup for Claude
  Code (one-liner) and JSON blocks for Claude Desktop / Cursor are in `docs/mcp.md`.

### Changed
- Updater version handling: `v` / `V` prefixes are normalized across build, manifest and updater,
  so "Skip this version" keeps working regardless of prefix drift, the UI no longer displays a
  doubled "vv" prefix, and pre-release tags (e.g. `v1.0.5-rc1`) package and publish with the
  correct artifact names.
- Upgrades keep the same MSI upgrade identity as earlier versions, so the previous version is
  removed first and nothing is left behind; an old `%USERPROFILE%\.agenthive` is renamed to
  `%USERPROFILE%\.miderhive` automatically, and portable installs can point at another data home
  with `MIDERHIVE_HOME`.

### Security
- `tools/call` with malformed arguments can no longer crash the MCP server; `config/agents.json`
  is written atomically (temp file + rename) and a corrupted key file is refused instead of being
  overwritten; multi-step writes (knowledge body/vector, memory removal, token usage) complete
  inside transactions with rollback on failure.

## [1.0.1] - 2026-09-11

*Same feature set as 1.0.0; shipped under the AgentHive name
(`AgentHive-1.0.1-x64.msi`, `AgentHive-1.0.1-win64-portable.zip`, `agenthive.exe`,
`%USERPROFILE%\.agenthive`, `AGENTHIVE_PORT`).*

### Changed
- The artifacts for this release are built by CI from the tagged commit with the same reproducible
  pipeline (`scripts/package.ps1`) used for local builds.
- The notes document the upgrade path: installing this MSI over the old one (or replacing the
  portable folder) carries knowledge, messages, memory and agent keys over, because the data
  directory is never removed by installing or uninstalling; the workbench can also fetch the
  update itself (Settings → Update), previously registered agents keep working, and a lost key is
  re-issued from **Settings → Agents → Rotate API key**.

## [1.0.0] - 2026-09-10

*First stable release, published as a pre-release under the AgentHive name
(`AgentHive-1.0.0-x64.msi`, `AgentHive-1.0.0-win64-portable.zip`).*

### Added
- Local-first hub: binds `127.0.0.1` with no account, no cloud dependency and no telemetry, all
  state in a single SQLite file, plus a full HTTP API (unified response envelope, error codes,
  task state machine, examples) documented in `docs/api.md`.
- **Connect common agents wizard** (Settings → Agents) with presets for ZCode, Codex / ChatGPT,
  Claude Code, Factory Droid, Hermes Agent, Cursor and GitHub Copilot: one click issues the
  credential and generates a ready-to-paste setup block (base URL, agent name, API key, request
  headers, a 60-second heartbeat example, and where each tool expects its instructions —
  `AGENTS.md`, `CLAUDE.md`, `.cursor/rules`, `.github/copilot-instructions.md`, or environment
  variables). Idempotent: provisioning an existing name rotates its key.
- **API key rotation** (Settings → Agents → *Rotate API key*, or `POST /api/agents/rotate` with
  the master key). The database stores only salted hashes, so re-issuing is the only way to
  recover a lost key, and the old key stops working immediately.
- **Built-in auto-update**: a daily GitHub Releases check (toggleable in Settings); one click
  downloads, verifies the SHA256 and installs the new MSI — the portable build is never
  auto-installed, so you do not end up with two copies.
- A reproducible release pipeline (`scripts/package.ps1`) producing the MSI, the portable ZIP, a
  machine-readable `latest.json` update manifest and checksums, with a runnability self-check and
  WiX ICE validation.
- A single-instance guard: launching again focuses the existing window instead of failing with a
  "port already in use" error.

### Changed
- One consistent vector icon set (the leftover emoji were removed, including the 🐝 in the brand
  area); timestamps are relative ("last active 1 day ago") with the exact local time on hover;
  panels scroll instead of clipping; charts show exact values on hover; table columns fit their
  content; the knowledge toolbar was reorganised; button hierarchy and disabled states are
  distinguishable.
- Single source of truth for the version: the sidebar, `/api/health` and the installer metadata
  now always agree.

### Fixed
- A silent credential-loss mode: a failed write of the per-agent key cache was swallowed (the
  writer always reported success) and bootstrap never rewrote the file — in the worst case an
  agent lost its only plaintext key and just showed "offline" forever, with no clue. Writes are
  now atomic, failures are reported, and a missing cache is recorded in the audit log
  (`system.keyfile_missing`) with the recovery hint.

### Security
- Fixed a **privilege-escalation hole via privileged identities**: the name `user` is treated as
  the human user internally (allowed to resolve anyone's errors and move anyone's task state), yet
  the registration endpoint accepted it. `user` / `zcode` / `system` are now reserved, and
  registration only accepts the `member` role.
- Fixed **missing read isolation on point-to-point messages**: non-managers now only see
  broadcasts, messages addressed to them, and messages they sent.
- Fixed broadcast messages being unreachable (older rows mixed an empty string and NULL for
  `recipient`).
- Key comparison is now constant-time; request bodies are capped at 1 MiB; field type errors
  return 400 with the standard JSON envelope instead of a bare 500.
- Hardened the outbound URL guard against shorthand/octal bypasses (`127.1`, `0177.0.0.1`,
  `0x7f.1`, `2130706433`).

<!--
Versions 1.0.2 and 1.0.3 shipped without release notes in this repository, so they are not
documented here. 1.0.5 has no tag or release either, so it has no compare link below.
-->

[Unreleased]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.1.1...HEAD
[1.1.1]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/SiliconCoderJames/MiderHive/compare/V1.0.4...v1.1.0
[1.0.4]: https://github.com/SiliconCoderJames/MiderHive/compare/V1.0.3...V1.0.4
[1.0.1]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/SiliconCoderJames/MiderHive/releases/tag/v1.0.0
