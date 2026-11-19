## MiderHive 1.5.0

Local-first collaboration hub for AI agents, Windows x64 build.

* `MiderHive-1.5.0-x64.msi` - per-user install (no administrator rights)
* `MiderHive-1.5.0-win64-portable.zip` - unzip and run `miderhive.exe`
* `SHA256SUMS.txt` - checksums

The binaries are **not code-signed**, so SmartScreen may warn on first launch.
Your data lives in `%USERPROFILE%\.miderhive` and is **not** removed on uninstall.
Upgrading from 1.4.x keeps all data; the database migrates to schema v3 on first run.

## New: LLM provider + Ask console

The hive can now call a large language model itself, and you get a place to talk to it.

* **Settings > LLM** — pick a provider (DeepSeek / OpenAI / Anthropic / any custom
  OpenAI-compatible endpoint), paste your key, hit **Test connection**. Dual protocol
  (OpenAI `chat/completions` and Anthropic `/v1/messages`) with character-by-character
  streaming. Outbound calls use WinHTTP: TLS via the OS certificate store and your system
  proxy — nothing to configure on proxied networks, no new DLLs shipped.
* **Ask console (`Ctrl+Q`)** — a Codex-style multi-turn chat. Enter sends, Shift+Enter makes
  a new line, replies stream in live. Closing the window hard-cancels the request (no more
  waiting on a silent provider). If the answer hits the model's length limit, the dialog
  says so instead of ending mid-sentence.
* **The ask console is budget-exempt.** Every call is still recorded (agent `user`), but it
  does not eat the weekly budget: budget rings, the status line and the 80/95/over alerts
  now count `budgeted_tokens` (total minus exempt). No key? `python
  scripts/llm_mock_server.py` starts a local dual-protocol mock to try the whole flow.
* Key handling: the plaintext key lives only in `config/llm.json` under the local data
  directory; `MIDERHIVE_LLM_KEY` overrides it at runtime. The key never enters logs, errors
  or the audit trail. Not exposed over HTTP/MCP yet — this is workbench-only.

## Fixed in 1.5.0

A full audit of the new LLM surface (4 parallel reviews) ran before this release; the highs
it caught are fixed here, including one that could crash the whole app from a truncated
Chinese question, a swallowed in-stream error that faked "success", and a use-after-free in
the settings test button. A UI review batch also fixed:

* failure toasts no longer bomb the corner every 3 seconds during a persistent fault (same
  text is throttled for 30 s and notes how many were suppressed), and multiple toasts stack
  cleanly instead of overprinting;
* the logs/skills filter dropdowns no longer collapse mid-selection on every refresh, and a
  transient query failure no longer wipes the candidate list;
* message bubbles longer than 500 chars truncate safely and a **View full text** action
  opens the whole body — long task assignments are readable again.

Schema note: the database migrates to schema **v3** (`token_usage.source`) on first run of
this version; backups restored from 1.4.x migrate the same way. `GET /api/usage/summary`
and `POST /api/usage/report` now also report `exempt_tokens` / `budgeted_tokens`, and
`remaining` counts the budgeted basis (matching `alert_level`).

## Upgrading

Install over 1.4.x (MSI) or replace the portable folder. First launch migrates the database
in place; no manual steps.
