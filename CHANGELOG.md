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

### Fixed
All of the following were found by the UI harness once it rendered real fonts, applied the
application stylesheet and compared pixels between runs (see Added). None of them is a
functional failure; they are controls that lost their styling or UI state that did not
survive the 3-second refresh.
- **Empty-state call-to-action buttons looked like plain text** (knowledge, errors, skills and
  memory panels: "＋ New entry", "＋ Report an error", "＋ Register a skill", "＋ Write a
  memory"). The empty-state container set a selector-less stylesheet
  (`background:transparent; border:none;`). In Qt such declarations apply to the widget **and
  every descendant**, and an ancestor's stylesheet beats the application stylesheet, so the
  primary button lost its fill and border. Containers now use `ui::setSelfStyleSheet()`
  (`#objectName { … }`), which applies to the widget itself only.
- **Buttons in the dashboard's banners lost their fill** (health banner, onboarding banner):
  same selector-less pattern on the banner containers.
- **The onboarding banner had no background and drew a box inside its box.** Its stylesheet used
  `@warnbg@`, a placeholder `ui::th()` did not know, so Qt could not parse the stylesheet at all
  (one "Could not parse stylesheet" warning per rebuild). Its `QWidget { … }` rule also framed
  the label inside it. `warnbg` is now a derived colour like `okbg` / `errbg`, and the banner
  styles only itself.
- **The "per-agent usage" bars re-animated on every refresh** (every 3 seconds they swept from
  zero): the chart was re-fed on each poll. The bar charts now ignore a call whose entries are
  unchanged, which also stops the usage panel's trend and model charts from re-animating.
- **Usage panel, per-agent table**: the selected row jumped to another row on every refresh, and
  a sort order chosen by clicking a header was reset, because the table was rebuilt and
  re-sorted on each poll. It is rebuilt only when the data changes, and the selection is
  restored by agent name.
- **The audit log tree jumped back to the top on every refresh**, and day groups the user had
  collapsed were re-expanded. The tree is rebuilt only when the records change; on a real change
  the scroll position, the current row and the collapsed days are restored.
- **Usage panel, the auto-digest checkbox was invisible**: "Auto digest" was created but never
  added to the layout, so it sat at position (0,0) underneath the interval spin box — the label
  was completely covered and only a clickable corner of the box peeked out: an invisible toggle.
- **Usage panel, "left 10M" was drawn on the budget ring's lower arc** (dim text on the arc,
  looking cut off). The ring now reserves a strip below itself for the caption and shrinks to
  fit; the caption never touches the arc.
- **Spin-box up/down buttons were invisible**: the stylesheet styles the box but had no rules
  for the buttons, which Qt then collapsed to two hairlines (the interval spin box looked like
  it had a broken handle). The buttons now have a real size, a separator and a hover state, with
  crisp SVG arrows.
- The graph's initial layout no longer depends on the global `std::rand()` state: the canvas has
  its own fixed-seed generator, so the same data starts from the same arrangement on every
  launch. "Re-layout" still produces a fresh arrangement.
- `scripts/e2e_collaboration_test.py` had been failing since 1.2.2 and nobody noticed: its step 10
  expected agent A to see agent B's row in `GET /api/audit`, but since 1.2.2 that endpoint is
  scoped per agent. The step now checks the scoping itself (A sees only its own rows) and, when
  `MH_ZCODE_KEY` is set, that the manager sees everything. The other 22 steps pass again.

### Added
- **The UI harness is a required check** (47 scenes; it fails CI and `scripts/ci_local.ps1`
  like any other gate). It starts the workbench exactly as `gui/main.cpp` does (application
  font, global stylesheet, built-in HTTP service), loads fonts (the offscreen platform loads
  none by default, so every screenshot was boxes), freezes the display clock and seeds "now
  minus fixed offsets", points tool detection at a fixed fake environment, refreshes a panel
  synchronously before capturing it, and leaves out Qt's scroll-area scaffolding (whether a
  scrollbar shows depends on font metrics, and showing it reorders the children). Verified:
  20 runs on fresh data directories produce byte-identical snapshots and golden images; on the
  CI runner (UTC, different fonts) all 47 scenes match the baselines; a run with a Latin-only
  font set matches as well. Before this, CI reported 27–30 differences as a success.
- New invariant rules: standard controls must be reachable by Tab (the Tab order is part of each
  baseline); stylesheets must not contain unreplaced `@token@` placeholders; a container's
  stylesheet rule must not reach the controls inside it; user state (selection, filter text,
  scroll position, combo box choice) must survive an automatic refresh.
- Baseline differences now report where they differ; every scene is captured as a screenshot
  and listed in `build/ui_harness/ui_report.html`; CI uploads the snapshots and the report as the
  `ui-harness` artifact.
- `scripts/ui_harness_rounds.py`: runs the harness N times on fresh data directories and
  requires every snapshot and golden image to be byte-identical across rounds.
- `ui::displayNow()` (GUI) and `GraphPanel::settleLayout()` are test seams: the first lets the
  harness freeze the clock through `MIDERHIVE_UI_NOW`, the second runs the force-directed layout
  to its stop condition synchronously. Neither affects normal use.
- `scripts/ui_harness.ps1`: the UI harness in one command, with the same invocation as CI, and
  the path of its report (`-Filter`, `-UpdateBaseline`, `-OpenReport`).
- The harness's fake machine is now sealed against the real one for the "already connected"
  check too: `MIDERHIVE_CONNECT_ROOT` (the same redirect the GUI self-test uses) sends every
  tool-config read into the fake root, so the onboarding banner counts the four planted tools no
  matter what is installed on the machine running the harness. Before, the GUI resolved configs
  through the real home directory, so the banner showed whatever the machine had connected — it
  flipped from four tools to one mid-day. The fake executables also lost their `.cmd` suffixes
  (real-time AV scanning briefly locks fresh executables, which made the file-existence checks
  fail for one run), the harness now waits until all four planted tools are detected (and aborts
  loudly if not), and the environment line reports how many tools were detected.
- `scripts/check_symbols.py`: asks DbgHelp whether an executable can be debugged at source level
  (does the PDB belong to this exact build, do line numbers resolve). `scripts/deploy_dev.ps1`
  runs it on every deploy.
- `docs/dev.md`: how to run, debug and test the workbench, with every command tried on a real
  build; linked from both READMEs.

### Changed
- `scripts/deploy_dev.ps1` deploys RelWithDebInfo by default: the same optimisation as Release
  plus debug symbols, so the copy in `dist\` can be run and attached to a debugger. It used to
  deploy Release, which carries no PDBs. The result of the symbol check and short run/debug
  instructions are written into `dist\BUILD_INFO.txt`.
- Baselines in `tests/baseline/` were re-recorded. The previous ones recorded what was installed
  on the machine they were made on (nine tools), local-time timestamps and stale panel content;
  they could never match another machine.
- `scripts/verify_gui_selftest.py` gives the GUI self-test a font directory too.
- The numbers under "Quality and verification" in both READMEs were stale; they now read 2,454
  unit-test checks, 45 integration assertions and 79 GUI assertions, and list the UI harness and
  the bool-return check.

## [1.6.1] - 2026-11-24

Patch release. It reverts a regression in 1.6.0 that broke ZCode onboarding and closes a set of
auto-digest defects — the most serious being that a failing digest could retry every 15 minutes
and spend outside the budget. No database migration (schema stays v4).

### Fixed
- **ZCode onboarding regression (1.6.0)**: a pre-release audit fix had made `zcode-agent` a
  reserved agent name, so the onboarding wizard's default identity for the ZCode tool could no
  longer be issued (`POST /api/agents/provision` answered 400 `reserved agent name: zcode-agent`).
  Reverted. `zcode-agent` keeps its gate exemption, and an explicitly disabled `zcode-agent` is
  still denied. A new test (`wizard_default_identities`) asserts that every default identity the
  wizard can prefill is issuable, in-process and over HTTP.
- **Auto digest retried every 15 minutes and could spend outside the budget (1.6.0)**: the
  scheduler decided "due" from the watermark, which only advances when a digest succeeds. After a
  failed run it was therefore still due, and the 15-minute tick called the model again regardless
  of the configured interval. The provider bills a failed call too, but its tokens were never
  recorded, so the budget gate could not see that spend. Now every run that reaches the model
  records an attempt timestamp first (`digest_last_attempt_at`) and the scheduler backs off for a
  full interval from it; budget skips count as an attempt too. If the timestamp cannot be written,
  the model is not called.
- **Auto digest suspends instead of looping**: three failed runs in a row (model call failed, or
  the summary could not be saved), or one run whose summary was saved but whose progress could not
  be recorded, suspend digest runs until MiderHive restarts or the LLM / digest settings are saved
  again. While suspended, the scheduler stays quiet and manual triggers (the button,
  `agent-cli digest run`, `POST /api/digest/run`) are refused with the reason.
- **The suspended state is visible**: the Usage panel shows "Auto digest suspended" with the
  reason. Before, the toggle still read as enabled and the status line looked like "nothing for
  days".
- **Digest reported success when nothing was saved (1.6.0)**: if the summary was generated but the
  knowledge-base write failed, the run returned success and the UI showed "Digest written to
  knowledge base ✓". It now returns failure and leaves the watermark alone, so that activity is
  summarized again next time.
- **Digest read failures were treated as "no activity" (1.6.0)**: when the watermark or one of the
  activity streams (audits / messages / errors) could not be read, the run continued with partial
  data and advanced the watermark, so that activity was never summarized. Any read failure now
  aborts the run without touching the watermark, and a watermark read failure no longer falls back
  to "30 days ago".
- **Digest backlog handling re-summarized the same batch (1.6.0)**: when a stream hit its per-run
  cap, the watermark was only advanced to the oldest consumed row. Each run still takes the newest
  N rows, so nearly the same batch was summarized and paid for again on every run, while the older
  rows stayed out of reach regardless. The watermark advances normally again, and
  `activity_truncated: true` in the run stats reports that older activity was beyond the cap.
- **Digest settings read failure no longer looks like defaults (1.6.0)**: a failed read returned
  "disabled / never run", which could make the scheduler treat a run as due while the panel showed
  a disabled toggle. The read now reports failure: the scheduler skips the round and the panel
  keeps its last state.
- **UI harness: seeding failures abort the run**: 13 seed calls ignored their result, so the
  harness could continue on partially seeded data. It now exits with code 3.
- **Bool-return discipline**: 24 unchecked `bool` returns in the digest functions and the harness
  seeding — the reason the last CI step was red on master while 1.6.0 was released. All are
  checked now, and `scripts/ci_local.ps1` runs that gate (and every other CI step) before pushing.

### Changed
- **Auto digest does not overshoot the budget**: with a token budget, a run is skipped
  (`skipped_reason: "budget headroom"`) when what is left cannot cover the run's worst case (half
  the prompt's UTF-8 bytes plus the 1024-token output cap). Before, only "already over budget" was
  checked, so the digest itself could push usage past the line. Request budgets were already exact.
- Budget skips back off for a full interval. Before, the scheduler re-checked every 15 minutes and
  wrote a `digest.skip` audit row each time.
- Skip toasts say why in words ("budget used up" / "not enough budget left to finish one run" /
  "nothing new to summarize") instead of printing the run's JSON. The new UI strings ship in all 8
  languages.
- Run stats gained `consecutive_failures` and `suspended`.

### Added
- `scripts/ci_local.ps1`: every post-build CI step, in CI order with the same arguments, against
  the local build; reports all results at once instead of stopping at the first failure.
- `scripts/deploy_dev.ps1`: deploys the current build to `dist\` as a runnable tree
  (`MiderHive.exe`, `platformd`, `agent-cli`, `miderhive-mcp`, Qt runtime) with a `BUILD_INFO.txt`
  and a start-up smoke test.
- `ah::digestDue()` decides whether an auto-digest run is due (unit-tested; the logic used to live
  in the window class).
- `docs/api.md`: `POST /api/digest/run` is documented (it had been missing since 1.6.0).
- **UI/UX test harness** (`ui_harness`, first full phase): 46 scripted scenes compared against
  structural baselines in `tests/baseline/`, invariant rules, a dialog auto-answerer, golden image
  hashes and an HTML report. **Not yet a regression gate** — see Known issues.
- Tests: 2454 checks (+107).

### Known issues
- The **UI harness** still runs as observation only in CI (`continue-on-error`). Its golden-image
  comparison cannot currently report a difference (the hash is recorded before it is compared),
  its baselines contain machine-dependent content (27 of 46 scenes differ on the CI runner, and
  one date field drifts with the calendar), a new baseline does not fail the exit code, and two of
  the agreed interaction checks are not implemented (focus order, state retention).
- Digest collection uses second-resolution timestamps. Activity written in the same second as the
  collection query but after it is not picked up by the next run. Fixing this needs id-based
  cursors.
- A failed model call cannot be accounted (the provider's usage is not available on a failed
  response); the back-off and suspension above bound this to at most three attempts per
  configuration.
- Digest entries are ordinary knowledge entries with tag `digest` and are not pruned.

## [1.6.0] - 2026-11-21

### Added
- **Connection switches (schema v4)**: one-click enable/disable for the platform's links to
  agent tools — a global gate plus a per-agent switch. Platform-side rejection semantics:
  a disabled identity's every hive call is immediately answered **403** with a plain-language
  reason (`connection disabled: …`), while tool configs and API keys stay untouched; re-enable
  takes effect instantly. Exempt: the manager identity `zcode` and the ZCode tool's default
  identity `zcode-agent`. HTTP `POST /api/agents/disable|enable`, `PUT /api/agents-gate`; CLI
  `agents disable|enable --name N`, `gate close|open`; Settings > Agents gets a gate row and a
  Disable/Enable action; the dashboard shows a "disabled" badge with an enable action and a
  gate-closed alert card. `agents.disabled` is a dedicated column (the `status` column is
  heartbeat-inferred and cannot hold a disabled state).
- **Budget by requests (dual basis)**: the weekly budget can now count tokens *or* call
  counts — `budget_mode` (`tokens` | `requests`) plus `weekly_request_budget`, edited as one
  three-key config in the budget dialog; alert levels switch basis accordingly (exempt ask
  console excluded in both). `/api/usage/budget` GET/PUT expose all three keys (PUT stays
  backward compatible: `budget`-only = legacy single-key edit); summary/report add `mode`,
  `calls`, `budgeted_calls`; the skill-invoke response's `remaining` is now mode-aware.
- **Auto digest (summary harvesting)**: the platform can periodically have its configured LLM
  distill recent hive activity (audits/messages/errors since a watermark) into a ≤500-char
  structured summary written to the knowledge base (tag `digest`). Budget-gated: when the
  active basis is over budget the run is skipped without calling the LLM. Accounting uses
  `source=llm-digest` (counted against the budget — the gate exists for it). Manual trigger:
  Usage panel "Run now" (success/skip/failure all toast), `agent-cli digest run`, or
  `POST /api/digest/run`; scheduling lives in the workbench (15-min check, workbench-closed
  means not running).
- **Knowledge graph view (Obsidian-style, 9th panel, Ctrl+9)**: force-directed graph where
  nodes are the latest knowledge entries (size = degree, color = category) and edges are
  vector-kNN similarity — MiderHive's own take on the graph view: related entries are
  discovered by embeddings, not by hand-written links. Wheel zoom, drag-pan, drag-to-pin,
  click for a detail pane with a jump into the knowledge base; positions are cached per uuid
  across refreshes; tag filter and re-layout. Data plane: `knowledgeGraphSnapshot` (per-entry
  self-vector kNN, dedup by uuid pair, similarity = 1/(1+distance)).
- The UI speaks the existing 8 languages across the new surfaces (39+13 new keys per pack).

### Fixed
- **Full pre-release audit of all new surfaces (2 parallel reviews)** before this release;
  findings fixed (see the audit note in the release page).
- Small UX debts from the earlier UI review: settings navigation is keyboard-reachable;
  agent status / error severity+status / message status enums are localized in the display
  layer (storage stays English); usage-panel read failures toast instead of silently keeping
  stale data; three destructive confirm dialogs default to "No".

## [1.5.0] - 2026-11-19

### Added
- **LLM provider support**: the platform can now call a large language model itself —
  DeepSeek, OpenAI, Anthropic, or any OpenAI-compatible endpoint (dual protocol: OpenAI
  `chat/completions` and Anthropic `/v1/messages`, including SSE streaming; a
  `stop_reason=max_tokens` / `finish_reason=length` truncation is surfaced instead of
  silently cutting the answer). Outbound calls go through a new WinHTTP transport (TLS via
  the OS certificate store, system proxy/PAC for free, hard-cancel support, 32 MB response
  cap; no new dependencies). Configuration lives in `config/llm.json` (plaintext key on this
  machine only; atomic write, corrupt-file refusal, audit never contains the key and
  `base_url` is sanitized before audit); `MIDERHIVE_LLM_KEY` overrides the key at ask time.
  New "LLM" page in Settings with preset backfill, masked key entry and a live
  "Test connection" button (worker thread with cancel-on-close). Not yet exposed over
  HTTP/MCP.
- **Ask console (Ctrl+Q)**: a Codex-style multi-turn chat dialog with per-character streaming
  replies, Enter-to-send, session token totals, and a cancel path that closes the HTTP handle
  outright — closing the window no longer waits on the provider. Usage is recorded (agent
  `user`) but **exempt from the weekly budget**; an accounting failure is shown as a warning
  instead of claiming "recorded".
- **Budget-exempt usage accounting (schema v3)**: `token_usage` gains a `source` column
  (`''` = agent-reported; `llm-ask` = platform's own ask console). Budget numerators and the
  80/95/over alert levels now use `budgeted_tokens` (total minus exempt); `/api/usage/summary`
  and `/api/usage/report` additionally report `exempt_tokens` / `budgeted_tokens` and their
  `remaining` uses the budgeted basis. External agents cannot create exempt rows (the report
  endpoint ignores the field).
- The UI speaks the existing 8 languages across the new LLM/ask surfaces (37 new keys per
  language pack).
- `scripts/llm_mock_server.py`: a dependency-free local mock of both protocols for manually
  verifying the settings page and the ask console without touching a real provider.

### Fixed
- **Full-tree audit of the new LLM surface (4 parallel reviews)** before release. Highs: a
  truncated multi-byte audit string could produce invalid UTF-8 and crash the whole process
  from the ask worker thread (any Chinese question ≥ 68 chars triggered it); in-band
  `data:{"error":…}` events inside OpenAI-compatible streams were swallowed into a
  "successful" half-answer; the settings "Test connection" used a detached thread that could
  invoke into a destroyed dialog (UAF); closing the ask console mid-stream could freeze the
  UI for up to 120 s (now a hard transport cancel); a leaked Toast stacking counter pushed
  every later Toast off-screen after parent-window teardowns.
- **UI review batch**: persistent-failure Toast storms are throttled (same text 30 s, with a
  "suppressed N times" note) and stack cleanly instead of overprinting; the logs/skills
  filter dropdowns no longer collapse mid-selection on every 3 s refresh (and a transient
  query failure no longer wipes the candidate list); message bubbles truncate safely
  (before HTML escaping, surrogate-pair aware) and a "View full text" action opens the full
  body — long task assignments are readable again.
- Schema migrations now hard-fail bootstrap when an `ALTER TABLE` fails for a reason other
  than "column exists" — previously a failed migration could leave the database stamped with
  the new `user_version` but missing the column, surfacing far away as "no such column".

## [1.4.0] - 2026-10-11

### Added
- **13 first-class clients + a custom-client fallback + auto-connect, three ways**: Gemini CLI,
  Qwen Code, iFlow CLI, Windsurf and Kiro joined the onboarding registry (all standard
  `mcpServers` JSON — the wizard writes their settings files while preserving every unrelated
  setting); anything else that speaks the Claude shape is covered by a custom-client fallback;
  and connecting became one click, three ways — a "Connect all detected" button in the wizard,
  a dashboard banner when installed-but-unconnected tools are found, and `agent-cli
  auto-connect` for headless setups (idempotent re-runs). All three deliberately skip tools
  that are online or already wired: provisioning rotates keys, and re-issuing would invalidate
  keys you already pasted.
- **The UI now speaks 8 languages**: 简体中文, 繁體中文, 日本語, 한국어, Deutsch, Français,
  Italiano and English (per-language Qt translations included). The i18n architecture moved
  from hardcoded zh/en pairs to N-language override tables keyed by the Simplified Chinese
  source text; new tooling: `scripts/i18n_extract.py` and `scripts/merge_i18n_packs.py`.
- **The update manifest is signed and verified**: CI signs `latest.json` with RSA-2048 and
  publishes `latest.json.sig`; the updater verifies the signature before trusting the manifest
  and refuses a mismatched one. (1.3.0)

### Fixed
- **Non-ASCII data directories work end to end**: the platform core read `%USERPROFILE%` via
  `std::getenv` (ANSI bytes) while SQLite interprets paths as UTF-8 — on a Chinese Windows
  account the GUI and platformd failed to start at all, backups were invisible, and the MCP
  server exited permanently. All path handling now goes through explicit UTF-8→wide
  conversion; verified by a new end-to-end test that bootstraps, writes, backs up and restores
  inside a Chinese-named directory.
- **A corrupted `agents.json` no longer crashes the app on every start**: bootstrap parsed it
  with a throwing parser and both entry points had no handler — `std::terminate` with zero
  diagnostics. Corrupt files are now treated as unreadable and land in the existing
  keyfile-missing diagnostics.
- **Modal dialogs no longer race the 3-second auto-refresh**: resolving an error or replying to
  a message held a reference into a vector that refresh rebuilds — a use-after-free that could
  write the resolution onto a different record. The identity is copied before the dialog opens.
- **Batch connect no longer rotates keys of Claude Code users** connected via the recommended
  project-root `.mcp.json` (the "already connected" check only looked at the Desktop config).
- **Broadcast messages can be acted on by everyone**: accepting a broadcast task or marking a
  broadcast note read used to return 400 for every non-manager agent (deadlocking broadcast
  tasks in pending) — the broadcast exemption that `messageReply` already had now applies to
  status changes too.
- **Single-connect re-issuing an online agent now asks first**: provisioning an online
  identity rotates its key and drops the tool offline; both dialogs confirm before rotating.
- **The Codex TOML writer refuses two more shapes it cannot merge** (top-level inline
  `mcp_servers = { ... }` tables and commented `[mcp_servers]` headers with an occupied
  `miderhive` key) instead of appending a table that breaks the whole config.toml.
- **`agent-cli auto-connect` writes Hermes into `%LOCALAPPDATA%`** where Hermes actually reads
  it (it used the wrong root and reported success), and every generated config now carries
  `MIDERHIVE_PORT` when the platform runs on a non-default port.
- **Old versions' vector rows leave the dimension table** when a knowledge entry gets a new
  version — ghost rows used to crowd out kNN recall and degrade semantic search as version
  chains grew.
- **Port settings are validated everywhere** (platformd, the GUI, the CLI — including `--port`
  written after the subcommand): invalid values now fail loudly or fall back explicitly instead
  of silently binding to a random OS port no agent can find.
- Plus: skill-invoke rejects wrapped token counts like the usage endpoint does; skill names
  with `/` are rejected at registration (they could never be invoked); length caps cover
  resolve notes, heartbeat tasks and reply bodies; a removed agent's leftover plaintext key is
  audited; read failures in the knowledge panel and settings dialogs surface as toasts instead
  of fake empty states; table context menus copy what you actually right-clicked; CSV export
  reports disk failures; the updater is cross-instance single-flight, detects mirror error
  pages instead of calling them tampered, and `fetch-deps.ps1` now vendors SQLite too (truly
  offline builds).

## [1.3.0] - 2026-10-07

- **The UI now speaks 8 languages**: 简体中文, 繁體中文, 日本語, 한국어, Deutsch, Français, Italiano and English, switchable from the footer menu (per-language Qt translations included). The i18n architecture moved from hardcoded zh/en pairs to N-language override tables keyed by the Simplified Chinese source text — all 668 existing call sites unchanged, missing entries gracefully fall back to English, and adding a new language is one header table plus one registry line. New tooling: `scripts/i18n_extract.py` (key extraction + coverage check) and `scripts/merge_i18n_packs.py` (merge translator output into the pack headers). The GUI self-test now asserts per-language landmarks (switch language → the settings tooltip must equal the pack's translation of "设置").
- **Five more first-class clients + a custom-client fallback**: Gemini CLI, Qwen Code, iFlow CLI,
  Windsurf and Kiro join the onboarding registry — all speak the standard `mcpServers` JSON
  shape, so the wizard writes their settings files directly while preserving every unrelated
  setting (theme, model, ...). Anything else that understands the Claude-shaped config is covered
  by a **custom client** fallback: pick any config file path and the wizard merges a standard
  `miderhive` entry with the same safety net (backup, atomic write, shape refusal).
- **Auto-connect, three ways**: a "Connect all detected" button in the onboarding wizard that
  provisions and wires every installed-not-yet-connected tool in one go; a dashboard banner that
  appears when installed-but-unconnected tools are found (click to batch-connect — nothing is
  written silently); and `agent-cli auto-connect` for headless setups (idempotent: re-runs skip
  everything already wired). All three deliberately skip tools that are online or already
  connected — provisioning rotates keys, and re-issuing would invalidate keys you already pasted.
- Install detection moved into the core (PATH scan + candidate install/config paths, shared by
  the GUI and the CLI), fixing a latent Windows bug along the way: file operations in the config
  writer used narrow paths, which the OS interprets in the legacy ANSI code page — user profiles
  with non-ASCII names (Chinese, Cyrillic, ...) would fail detection and writes. Everything now
  goes through proper UTF-8 conversion.

## [1.3.0] - 2026-10-07

### Added
- **The update manifest is signed and verified**: CI signs `latest.json` with an RSA-2048 key
  (PKCS1 v1.5 / SHA256) and publishes the signature as a `latest.json.sig` release asset. The
  private key exists only in GitHub Secrets and on the maintainer's machine; the public key is
  compiled into the app. The updater downloads the sidecar, verifies the signature **before
  trusting the manifest**, and refuses the whole check on mismatch — a tampered distribution
  channel can no longer redirect users to a hostile installer. SHA256 verification of the
  installer binary stays in place (a second, orthogonal gate). Manifests published before 1.2.3
  carry no signature and are accepted with a hash-only check during the transition window.
  Verification is built on Windows CNG (`BCryptVerifySignature` with explicit
  `BCRYPT_PAD_PKCS1` — RSA signature primitives reject the ECDSA-style `dwFlags=0`); no new
  third-party dependencies.
- **`release-audit` workflow** (`.zcode/workflows/release-audit.dwf.ts`): the full-tree audit
  that produced the 1.2.2 fix batch is now a saved, re-runnable project workflow — nine areas
  read in parallel, every candidate finding independently confirmed or refuted, a report with
  path:line evidence and a fix roadmap. Referenced from CONTRIBUTING as a pre-release step.

### Fixed
- **Usage report rejects malformed token counts instead of silently mangling them**: the HTTP
  layer derived `tokens_in` / `tokens_out` as 32-bit ints — values above 2^31 wrapped into
  garbage, floats were truncated, and everything landed in the usage statistics. Non-integers
  now return 400 and the service layer enforces a per-report cap of 10,000,000 (same order as
  the default weekly budget).
- `verify_clients.py` no longer treats a `~/.dsh` directory without the `dsh` executable as an
  installed client (false FAILs on machines with leftover state), and `verify_gui_selftest.py`
  rebuilds `gui_selftest` automatically when the binary is older than any `src/gui/**` source —
  running it locally after a code change no longer reports stale results.

## [1.2.2] - 2026-10-06

### Fixed
- **Audit rows now name the object they describe**: `knowledge.create`, `message.send` and
  `error.report` wrote audit rows with an empty `target` (the service filled the output struct
  only after commit, while the audit step runs inside the transaction), and the `version` in
  `knowledge.version.add` / `memory.set` details was always the caller's default `1` (the detail
  string was built before the write happened). The trail existed but could not be traced back to
  an object — contrary to the "actor + time + action + object" contract. Services now populate
  the output before their in-transaction audit step, and the details are built when the audit
  actually runs.
- **Self-declared embeddings must name their model**: posting a raw `embedding` with an empty
  `embedder` used to be labeled with the built-in `ngram-hash-v2` tag — any model's vectors could
  then enter a dimension table under that fake identity, defeating the dimension-provider binding
  (cross-model vectors silently polluted each other's kNN results), while later honest writes were
  rejected by the fake registration. Such requests are now rejected with 400 on the platform side;
  the MCP server no longer defaults `embedder` to `agent`, and `agent-cli` demands
  `--embedder`/`--embed-model` at the entrance.
- **An interrupted restore can no longer leave you on a silent empty database**: the restore
  flow renames the live database to `platform.db.before-restore` before copying the backup in;
  a process killed between those steps used to let the next startup create a fresh empty database
  (SQLite creates on open) and run happily on it. Bootstrap now detects "main database missing,
  pre-restore stash present", moves the stash back, and logs a `system.restore_recovered` audit
  row; the startup self-check gains a `db_restore_leftover` item for the rare case where even the
  recovery is blocked. Writing `master.key` on first start is also checked honestly now (a failed
  write used to leave the platform running with a key that would silently rotate on every
  restart), and the two `create_directories` calls no longer share one error code.
- **Replying to a message and reading the audit log respect visibility**: `messageReply` had no
  permission check anywhere along HTTP - Platform - Service — any authenticated agent could reply
  to any message uuid and, as a side effect, flip a private message between two other agents to
  read (a terminal state). The same whitelist as status changes applies now (user, manager,
  sender, recipient; broadcasts are open), plus an empty-body rejection mirroring `messageSend`.
  `GET /api/audit` is no longer world-readable to agents: audit details carry point-to-point
  recipients and subjects, so non-manager agents now see only their own rows (the workbench,
  which talks to the platform in-process, still sees everything).
- **Two cross-process races closed**: the legacy `knowledge_vec` migration probed, copied and
  dropped in three separate auto-committed statements — a workbench and platformd starting
  simultaneously on an old database could collide on "no such table" and fail the whole
  bootstrap (the copy/drop now run in one BEGIN IMMEDIATE with an in-transaction recheck).
  `MessageService::setStatus` validated the state machine against a read outside the
  transaction, so two processes racing on the same task could land an illegal transition such as
  accepted - declined; the read-validate-write sequence is now atomic.
- **LIKE patterns are literals now**: knowledge keyword fallback search and all three tag
  filters treated `%` and `_` in user input as wildcards — searching "100%" matched unrelated
  entries and the tag filter `_` matched everything. Values are escaped and the queries carry
  an explicit escape character.
- **The skill-invoke response reports usage failures honestly**: it discarded the
  `usageSummary` return value and answered 200 with a default-filled summary on a read failure —
  the same shape of bug as the fixed "budget read pretends the default" issue. Malformed or
  non-object JSON bodies on reply/resolve are now a 400 instead of being treated as empty data
  (which used to write an empty reply or mark an error resolved without notes).
- **Credentials come from the OS CSPRNG**: master key, agent keys, salts and uuids were generated
  from an `mt19937_64` seeded with at most 64 bits of entropy; not exploitable within the stated
  same-user trust model, but there is no reason to keep a predictable source. The outbound URL
  guard also normalizes hosts now (percent-decoded and trailing-dot forms such as
  `%31%32%37.0.0.1` and `localhost.` no longer slip past the literal-address checks).
- **agent-cli tells the truth again**: the advertised `--port N` option was never read (silent
  connections to the wrong instance in multi-instance setups), and every "unknown command /
  unknown subcommand / missing required option" path exited 0. `--port` works (1..65535,
  validated), usage-error paths exit 2. `platformd` installs real signal handlers — the empty
  handlers plus a loop that never checked any flag made "press Ctrl+C to stop" a lie and headless
  operation an unkillable loop.
- **Environment variables arrive as UTF-8 on Windows**: `std::getenv` hands out ANSI code-page
  bytes (GBK on Chinese systems) while the platform stores agent names as UTF-8 — a Chinese agent
  name in `MIDERHIVE_AGENT_NAME` could never authenticate (401) or registered a garbled duplicate
  identity via the master-key path. `ah::envOr` reads the wide environment block and converts to
  UTF-8; the MCP server's identity resolution inherits the fix.
- **The onboarding writer cannot corrupt your config files anymore**: the Codex TOML branch reused
  the YAML quoter, and TOML literal strings do not support doubled-quote escaping — a name like
  O'Brien produced a config.toml that fails to parse entirely, while the UI reported success. The
  YAML path mistook `mcp_servers: {}` (flow value) for a parent key and inserted an indented block
  under it (structurally illegal YAML), and the TOML append path ignored existing non-canonical
  shapes, producing a TOML redefinition conflict. Unmergeable shapes are now refused with a
  paste-it-yourself hint; the backup step reports failure and refuses to write instead of
  claiming a `.miderhive.bak` exists; and pasted CLI commands quote name/key values so `&` in a
  name cannot become a command separator in cmd.exe.
- **The connect wizard pairs identity with credentials**: `ConnectDialog` kept the issued key but
  re-read the name field when writing — renaming after issuing produced a (new name, old key)
  config whose agent can never authenticate, while the panel waits forever. `WelcomeDialog`
  switched the write target before validating, so a failed pick left the previous tool's
  credentials pointing at the new tool's config file. Both fixed; the MCP executable path is no
  longer hardcoded with a `.exe` suffix outside Windows.
- **The updater is single-flight and honest about disk errors**: double-clicking "install" or
  triggering downloads from two windows concurrently appended into the same version-named
  temp MSI (Qt opens files with shared write on Windows), producing a corrupt file that was then
  misreported as "possibly tampered". Downloads ignore re-entry while in flight; failed disk
  writes now abort with a clear message instead of reaching the SHA-256 gate; and a broken
  manifest no longer suppresses automatic checks for 24 hours (`markChecked` moved behind field
  validation).
- **Panels stop dressing read failures up as empty states**: six panels discarded list-call
  return values while the service layer clears the output before failing — a read failure showed
  "no errors — all clear", "0 memories", "no messages", etc. Failures now toast through
  `humanError` and keep the previous content. The errors panel also survives its own 3-second
  auto-refresh (selection restored by uuid, so "Mark resolved" can no longer write the note onto
  a different record), manual reports use the contract enum `warning` (not `warn`), the log
  panel's date filter converts the local midnight correctly, the memory panel aborts a save when
  the version probe fails instead of silently skipping optimistic concurrency, and a failed
  welcome-memory write no longer claims success.
- **Deploy / release / installer stop failing silently**: `deploy.ps1` mirrors the target with
  robocopy `/MIR` after checking only that `miderhive.exe` exists — a missing `_stage` fell back
  to the GUI build output (three files) and would delete platformd, the CLIs, licenses and all
  Qt DLLs from a working install; a required-files gate now refuses to run. The release workflow
  checks `$LASTEXITCODE` after `gh release upload/edit` (a silent upload failure used to leave a
  green step and a stale `latest.json`). The MSI closes `platformd.exe` during upgrades
  (TerminateProcess — the console daemon has no window to receive WM_CLOSE), so a passive upgrade
  no longer ends 3010 with a stale daemon.

## [1.2.1] - 2026-09-25

### Fixed
- **Audit trailing is now part of the write it describes**: a write whose audit row cannot be
  persisted is rejected and rolled back instead of silently succeeding without a trace. The schema
  and README promise "every write is recorded in `audit_log`"; until now that held in exactly one
  place (`skill.invoke`) while some twenty other write paths discarded the audit result — leaving
  data that cannot be traced back to an actor. Writes and their audit rows now share one
  transaction (data and trace live or die together); genuinely best-effort trails — heartbeat,
  token telemetry, meta-audits — are marked `// IGNORE:` with the reason inline, so every
  exemption is itself auditable. Resolving an error is covered too, which also closes a race
  where two concurrent resolves of the same error could overwrite each other's notes.
- **Maintenance stats and the budget read report failure honestly**: `maintenanceRun` discarded
  the result of its three DELETE statements and then read `sqlite3_changes64()` anyway, so
  "deleted 0 rows" and "the delete failed" produced identical stats and a success return. Reading
  the weekly budget used to swallow the failure and return the default (10M) — a user who set
  5M saw no hint that the real answer was unknown. `GET /api/usage/budget` now returns 500 when
  the value cannot be read, and `usage/summary` fails with the budget error instead of letting a
  later query overwrite the diagnosis.
- **Restoring a backup no longer bypasses the schema gate or strands the workbench on a dead
  connection**: the restore path now replays the same initialization as startup (version gate,
  schema, migrations, vec/FTS), so a restored older database is stamped with the current
  `user_version` — previously the "refuse a newer database" gate simply did not exist on this
  path. If anything fails mid-restore, the previous database is stashed first and put back
  (compensation), so the platform stays usable and the error says the old database was left
  untouched; only if that also fails does the platform enter an explicit unusable state — every
  authenticated endpoint returns 503 and the dashboard leads with a "restart MiderHive" banner —
  instead of silently running against a closed connection.
- **Retrying a restore no longer deletes the last copy of your original database**: a failed
  restore whose compensation also failed deliberately keeps the pre-restore file
  (`platform.db.before-restore`); retrying a restore used to delete it unconditionally as the
  first step. The stash now rotates to `.before-restore.previous` (two generations kept), and the
  hard-failure message names the file so the rescue copy can be found.
- **A second backup within the same second no longer fails**: backup filenames have
  second-resolution timestamps and `VACUUM INTO` refuses to overwrite, so "back up now" twice in
  a row — or backup → restore → backup — died with `output file already exists`. Collisions now
  get a short random suffix; normal filenames are unchanged.

### Added
- **A machine-checked rule that failures are not silently swallowed**: CI now runs
  `scripts/check_bool_returns.py --fail` — calls from a checked list (database writes, audit
  logging, service mutations) must use their `bool` return, or carry an inline `// IGNORE:`
  with a reason. A deliberately injected violation was verified to fail the check before it was
  wired in; the unit suite also grew failure-path tests that inject real SQLite `RAISE(ABORT)`
  triggers and assert both the honest failure and the absence of half-applied state.

## [1.2.0] - 2026-09-18

### Added
- **Real-semantic knowledge without bundling a model**: `agent-cli embed` plus
  `knowledge add|search --embed-url` talk to your own OpenAI-compatible embedding endpoint
  (Ollama / LM Studio / vLLM / a gateway), so the knowledge base can use a real model while the
  platform itself still makes **zero outbound calls** — the request leaves from the CLI you invoked,
  never from the platform core. `MIDERHIVE_EMBED_KEY` carries the bearer token. Only `http://` is
  accepted (this build has no TLS), and every failure mode — https, unreachable host, non-2xx,
  non-JSON body, out-of-range dimension — fails loudly with a readable reason instead of silently
  falling back to the built-in embedder, which would have made "why is semantic search wrong?"
  unanswerable. See `docs/api.md` §4.3.1.
- **MCP-only agents can write real semantic vectors too**: `knowledge_add` and
  `knowledge_add_version` now accept an `embedding` (64..4096 numbers) plus the `embedder` that
  produced it — previously only `knowledge_search` accepted a query vector, so an MCP agent could
  search a non-default dimension it had no way to create. Both paths now share the same validation
  (range check and the dimension↔provider binding), and the MCP check grew seven assertions
  covering the write search round-trip at 256 dimensions, the model-name binding and the rejection
  of a non-numeric vector.
- **Verification tooling grew**: three suites that previously ran only when someone remembered to run
  them are now part of CI — the config-merge fuzz suite (39 cases), the per-client connect matrix, and
  the external-embedding round-trip (9 assertions). The unit binary reports 1,661 checks across 532
  assertion sites; note that ~1,000 of those checks are one assertion ("send succeeded") re-run to
  seed 1,001 messages for the pagination test, so the site count — not the check count — is the
  honest measure of coverage. Measurement scripts (`bench_search`, `bench_concurrent`,
  `bench_sqlite_ceiling`, `diag_latency`) stay manual on purpose: they answer questions rather than
  guard behaviour.
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
- Knowledge vectors now record their dimension (`embedding_dim`, schema **v2**); existing rows are
  backfilled from whichever per-dimension vector table holds them, so a knowledge base that was
  written by several models finally knows what it contains.
- The full-text index covers **latest versions only**: appending a version retires the previous text
  from the index, so index size no longer grows with version history. The index version is bumped so
  existing databases rebuild once on upgrade.
- Semantic recall's k cap moved from 10,000 to 50,000 — with sparse tags on a large database the old
  cap systematically under-returned results. The cap only exists to stop pathological memory
  blow-ups, not as a product limit.
- First-run onboarding now offers all eight clients (grid layout) and can auto-write the config
  from the welcome dialog too.
- `humanError` gained disk-full and permission-denied diagnoses, and its port branch no longer
  swallows any message merely containing "address".

### Fixed
- **A dimension mismatch in semantic search used to return an empty result, silently.** It now fails
  with an error that lists which dimensions (and embedding providers) the database actually holds,
  turning "no hits" into a diagnosable usage error.
- **Vectors from different models could pollute each other**: same-dimension vectors share one vec0
  table, so a second model silently overwrote the first model's neighbourhood and kNN results became
  unexplainable. A dimension is now bound to the provider that first wrote it; a later write with a
  different model is rejected with a message naming the bound provider, and the fix is to pick
  another dimension. Pre-existing mixed data is left untouched (the check only governs new writes).
- **`knowledge remove` left orphan vectors behind**: it deleted from one hard-coded vector table, so
  entries whose versions had different dimensions kept rows in the other tables. Deletion now covers
  every dimension of that uuid, in a single statement in the common case.
- **`apply-config` refuses configs it cannot merge safely**: user files using tab indentation or YAML
  block scalars (`key: |`) are declined with a paste-it-yourself hint instead of risking a corrupted
  config. Found by the config-merge fuzz suite — which is now part of CI precisely because nobody
  was running it.
- **Connect configs are replaced atomically**: the rewrite goes straight to a rename, so an
  interrupted write can no longer leave the user's config file missing.
- **Opening a newer database — or restoring a newer backup — is refused** instead of being silently
  half-understood. `schema.sql` declares `PRAGMA user_version`; bootstrap and the restore path both
  check it and tell you to upgrade MiderHive or restore an older backup.
- **MCP same-name registration race**: when two agents of the same name start together, the loser
  re-reads `agents.json` (three attempts) instead of exiting, so a cold parallel start no longer
  fails.
- `knowledge/search` clamps `limit` to 1,000; an unbounded value previously let a single request
  assemble an arbitrarily large result set.
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
- `docs/api.md` gained §4.3.1, the external-embedding-endpoint walkthrough — including the two
  deliberate boundaries (no SSRF allowlist, because the endpoint is user-named; `http://` only,
  because this build has no TLS).
- `docs/hardening-report.md` gained measured concurrency numbers (mixed load scales 3.10× at four
  threads, pure reads 9.47×, pure writes 1.14× because every commit fsyncs), a verdict on the
  long-suspected global lock (**not** a bottleneck, with a lock-free SQLite measurement to prove it),
  and a "known trade-offs" table that closes questions which kept being re-raised as defects.
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

[Unreleased]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.6.1...HEAD
[1.6.1]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.6.0...v1.6.1
[1.6.0]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.5.0...v1.6.0
[1.5.0]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.4.0...v1.5.0
[1.4.0]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.2.2...v1.3.0
[1.2.2]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.2.1...v1.2.2
[1.2.1]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.2.0...v1.2.1
[1.2.0]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.1.1...v1.2.0
[1.1.1]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/SiliconCoderJames/MiderHive/compare/V1.0.4...v1.1.0
[1.0.4]: https://github.com/SiliconCoderJames/MiderHive/compare/V1.0.3...V1.0.4
[1.0.1]: https://github.com/SiliconCoderJames/MiderHive/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/SiliconCoderJames/MiderHive/releases/tag/v1.0.0
