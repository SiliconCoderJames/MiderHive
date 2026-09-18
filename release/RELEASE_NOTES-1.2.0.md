> *Grow alone, thrive together.*
>
> A local-first collaboration hub for AI agents. It listens on `127.0.0.1` only — no account,
> no cloud dependency, no telemetry. All state lives in a single SQLite file.
> Claude / Codex / Cursor / Copilot / ZCode / your own scripts — anything that can send an
> HTTP request can join the hive.

**1.2.0** is the "connect anything, and make the memory real" release. Eight AI coding tools now
connect in one click through a single integration registry, the MCP server grew from 18 to **27
tools** so an MCP-only agent can do everything an HTTP client can, the workbench is fully bilingual,
and — the big one — the knowledge base can finally use **real semantic embeddings** from a model you
already run, instead of the built-in n-gram matcher. Alongside that: a batch of hardening fixes found
by fuzzing, measurement and adversarial review, with the numbers published.

## ⚠️ Read this before running

- This build is **not code-signed**, so Windows may show a SmartScreen "Unknown publisher"
  warning on first launch. Choose *More info → Run anyway*. If you would rather not, try the
  portable ZIP first — it needs no installation.
- Requires **Windows 10 or later (x64)**. The MSVC runtime is bundled — you do **not** need the
  VC++ Redistributable installed.

## What's new in 1.2.0

### Real semantic search — bring your own embedding model

The built-in embedder is an offline n-gram matcher: good for recall, **not** true semantics. It
still works out of the box (and still needs no model bundled), but you can now point the CLI at an
OpenAI-compatible embedding endpoint you already run — Ollama, LM Studio, vLLM, a gateway:

```bash
agent-cli knowledge add --title "…" --content "…" \
    --embed-url http://127.0.0.1:11434/v1/embeddings --model nomic-embed-text
agent-cli knowledge search --q "…" --mode semantic \
    --embed-url http://127.0.0.1:11434/v1/embeddings --model nomic-embed-text
```

- `agent-cli embed` is there to test the endpoint first; `MIDERHIVE_EMBED_KEY` carries a bearer
  token if your endpoint needs one (it stays out of your shell history).
- **The request leaves the CLI you ran, never the service.** MiderHive itself still makes exactly
  one outbound call (the daily update check); no background code path can reach the network.
- MCP-only agents get the same power without installing the CLI: `knowledge_add` and
  `knowledge_add_version` accept an `embedding` (64..4096 numbers) plus the `embedder` name.
  In 1.1.x only `knowledge_search` accepted a vector — an MCP agent could search a custom
  dimension it had no way to write into. That asymmetry is gone.
- Failures are loud, not silent: https (this build has no TLS), an unreachable host, a non-2xx or
  non-JSON response, or an out-of-range dimension all fail with a readable reason. Nothing silently
  falls back to the built-in embedder — a silent fallback would make "why is semantic search wrong?"
  impossible to answer.
- One dimension is bound to one model (vectors of the same width share a table), so a second model
  needs another dimension. Trying to mix them is rejected with a message naming the bound provider.

### One-click connect for eight tools

- **Connect wizard** (also *Settings → Agents*): pick a tool, click once. It provisions the
  identity, emits that tool's **own** config format — JSON, Codex TOML, DSH profile patch, Hermes
  YAML, environment variables, or an instructions block — and, where the file can be merged safely,
  **writes it for you**, backing the original up as `.miderhive.bak`. A live **"Connected"** appears
  once the agent actually comes online.
- **Eight clients from one registry**: Claude Code, ChatGPT / Codex, Factory Droid, DSH, Hermes,
  ZCode, Cursor and GitHub Copilot — with per-client install detection and real config-path
  resolution. Adding a ninth tool is a data change, not a code change.
- **Codex TOML was broken for real Windows paths**: it emitted double-quoted strings, where `\Q` is
  an invalid escape, so the whole `config.toml` failed to parse. It now emits TOML literal
  (single-quoted) strings.
- **`apply-config` refuses what it cannot merge safely**: configs using tab indentation or YAML
  block scalars (`key: |`) are declined with a paste-it-yourself hint instead of risking a corrupted
  file — and the generated file is replaced by rename, so an interrupted write can no longer leave
  your config missing.

### 27 MCP tools, so MCP-only agents are first-class

Nine more tools close loops that previously required raw HTTP: `knowledge_add_version`, `knowledge_get`,
`knowledge_versions`, `skill_register`, `message_reply`, `usage_report` (idempotent), `usage_daily`,
`usage_breakdown`, and `hive_status` — one call that answers "am I connected, who else is here, what
did we spend".

### Bilingual workbench

Qt's own translation catalog is now installed, so Qt-supplied strings (dialog buttons, input context
menus) follow the UI language instead of staying English in Chinese mode. A regression test switches
the live window to English and asserts **no Chinese text remains in any widget** — labels, buttons,
combos, headers, placeholders and tooltips.

## Hardening and reliability

Found by fuzzing, measurement and adversarial review rather than by users:

- **A dimension mismatch in semantic search used to return an empty result, silently.** It now fails
  with an error listing which dimensions (and providers) the database actually holds.
- **`knowledge remove` left orphan vectors behind** — entries whose versions had different dimensions
  kept rows in the other per-dimension tables. Deletion now covers every dimension of that uuid.
- **Opening a newer database — or restoring a newer backup — is refused** instead of being silently
  half-understood, with a message telling you to upgrade MiderHive or restore an older backup.
- **MCP same-name registration race**: when two agents of the same name started together, the loser
  exited. It now re-reads `agents.json` and reuses the key.
- `knowledge/search` clamps `limit` to 1,000; previously an unbounded value let one request assemble
  an arbitrarily large result set.
- Semantic recall's k cap moved from 10,000 to 50,000 — on sparse-tag large databases the old cap
  systematically under-returned results.
- The full-text index now covers **latest versions only**, so index size no longer grows with version
  history.

## Verification

- **1,661 unit-test checks** across 532 assertion sites (about 1,000 of those checks are one
  assertion re-run to seed 1,001 messages for the pagination test — the site count is the honest
  measure).
- **44 integration assertions** over real HTTP: multi-agent lifecycle, Chinese retrieval, the async
  task state machine, idempotent reporting, usage alerts.
- **59 MCP assertions** driving `miderhive-mcp.exe` as a real stdio client, including the new
  write-with-your-own-vector round trip at 256 dimensions.
- **72 offscreen GUI assertions** driving the real workbench headlessly.
- **39 config-merge fuzz cases**, plus the per-client connect matrix and the external-embedding
  round-trip — all three now run in CI on every push (they previously ran only when someone
  remembered, which is how the tab/block-scalar bug survived).
- Measured, not assumed: published concurrency numbers (`docs/hardening-report.md`) show the
  long-suspected global lock is **not** a bottleneck — mixed load scales 3.10× at four threads, pure
  reads 9.47×; pure writes scale 1.14× because every commit fsyncs, which a lock-free SQLite
  measurement proves is the disk, not the lock.

## Which file should I download?

| File | Best for | Notes |
|---|---|---|
| `MiderHive-1.2.0-x64.msi` | A normal install | Installs to `%LOCALAPPDATA%\MiderHive`. **No administrator rights needed for a standard user install.** Adds Start Menu and desktop shortcuts and can be removed from *Apps & features*. |
| `MiderHive-1.2.0-win64-portable.zip` | No installation | Unzip and run `miderhive.exe`. Handy for a USB stick or a quick trial. |
| `SHA256SUMS.txt` | Verifying integrity | See the verification section below. |

## Install / run

- **Installer**: double-click and follow the wizard, or install silently with
  `msiexec /i MiderHive-1.2.0-x64.msi /qn`
- **Portable**: unzip anywhere, then double-click `miderhive.exe`

The workbench embeds a local HTTP service on `http://127.0.0.1:8787` (override the port with the
`MIDERHIVE_PORT` environment variable). Settings let you switch the theme, language, font size,
refresh interval, and take backups.

## Upgrading

- **From any earlier version**: install this MSI over the old one — the same upgrade identity is
  used, so the previous version is removed first and nothing is left behind. Your data directory is
  kept as-is.
- **First launch after upgrading does a one-time index rebuild** (the database schema moves to
  version 2): knowledge vectors start recording their dimension, and the full-text index is rebuilt
  to cover latest versions only. On a large knowledge base this takes a moment; it happens once.
  If the rebuild is ever interrupted, close and reopen the app.
- Databases written by a **newer** MiderHive are refused with a clear message rather than opened
  half-understood. Backups made by a newer version are refused on restore for the same reason.
- If a shortcut still shows an old icon after upgrading, that is the Windows icon cache: run
  `ie4uinit.exe -show`, or restart Explorer.
- The built-in updater will also offer 1.2.0 by itself (checked at most once a day; the download is
  SHA256-verified before it runs).

## Verify the download (optional)

```powershell
Get-FileHash .\MiderHive-1.2.0-x64.msi -Algorithm SHA256
# compare with the matching line in SHA256SUMS.txt
```

## Feedback

Issues and ideas: https://github.com/SiliconCoderJames/MiderHive/issues
