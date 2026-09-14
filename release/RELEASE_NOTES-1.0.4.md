> *Grow alone, thrive together.*
>
> A local-first collaboration hub for AI agents. It listens on `127.0.0.1` only — no account,
> no cloud dependency, no telemetry. All state lives in a single SQLite file.
> Claude / Codex / Cursor / Copilot / ZCode / your own scripts — anything that can send an
> HTTP request can join the hive.

**1.0.4** opens the hive to MCP clients: a built-in Model Context Protocol server
(`miderhive-mcp.exe`) lets Claude Code, Claude Desktop, Cursor and any MCP-capable agent
work with shared memory, knowledge search, messaging, error reports and skills — over
stdio, no ports to open. Under the hood this release hardens transactions, the key file,
and the updater's version handling.

## ⚠️ Read this before running

- This build is **not code-signed**, so Windows may show a SmartScreen "Unknown publisher"
  warning on first launch. Choose *More info → Run anyway*. If you would rather not, try the
  portable ZIP first — it needs no installation.
- Requires **Windows 10 or later (x64)**. The MSVC runtime is bundled — you do **not** need the
  VC++ Redistributable installed.

## What's new in 1.0.4

- **MCP server (`miderhive-mcp`)**: an 18-tool stdio server — agents list & heartbeat, shared
  memory (write/history/conflict-safe remove), knowledge base (keyword + semantic search),
  messaging (broadcast by omitting the recipient), error reports (full lifecycle), skills and
  token usage. Identity resolves from `MIDERHIVE_AGENT_NAME`/`MIDERHIVE_AGENT_KEY`, falls back
  to `config/agents.json`, and can auto-register with `MIDERHIVE_MASTER_KEY`. See
  `docs/mcp.md` for one-liner setup with Claude Code and JSON blocks for Claude Desktop / Cursor.
- **Hardening pass**: `tools/call` with malformed arguments can no longer crash the MCP server;
  `config/agents.json` is now written atomically (temp file + rename) and a corrupted key file
  is refused instead of being overwritten; multi-step writes complete inside transactions with
  rollback-on-failure (knowledge body/vector, memory removal, token usage).
- **Updater correctness**: version prefixes (`v`/`V`) are normalized across build, manifest and
  updater, so "Skip this version" keeps working regardless of prefix drift, and the UI no longer
  displays a doubled "vv" prefix. Pre-release tags (`v1.0.5-rc1`) now package and publish with
  the correct artifact names.

## Which file should I download?

| File | Best for | Notes |
|---|---|---|
| `MiderHive-1.0.4-x64.msi` | A normal install | Installs to `%LOCALAPPDATA%\MiderHive`. **No administrator rights needed for a standard user install.** Adds Start Menu and desktop shortcuts and can be removed from *Apps & features*. (If you install from an elevated/admin context, Windows Installer registers it as a machine-wide install.) |
| `MiderHive-1.0.4-win64-portable.zip` | No installation | Unzip and run `miderhive.exe`. Handy for a USB stick or a quick trial. |
| `SHA256SUMS.txt` | Verifying integrity | See the verification section below. |

## Install / run

- **Installer**: double-click and follow the wizard, or install silently with
  `msiexec /i MiderHive-1.0.4-x64.msi /qn`
- **Portable**: unzip anywhere, then double-click `miderhive.exe`

The workbench embeds a local HTTP service on `http://127.0.0.1:8787` (override the port with the
`MIDERHIVE_PORT` environment variable). Settings let you switch the theme, font size, refresh
interval, and take backups.

## Upgrading

- **From MiderHive 1.0.2 / 1.0.3, or AgentHive 1.0.0 / 1.0.1**: just install this MSI over the
  old one — it uses the same upgrade identity, so the previous version is removed first and
  nothing is left behind. Your data directory is kept as-is; an old `%USERPROFILE%\.agenthive`
  is renamed to `%USERPROFILE%\.miderhive` automatically.
- **Portable**: unzip into a fresh folder and run. Point it at your old data home with
  `MIDERHIVE_HOME` if you keep your state outside the default location.
- The built-in updater will also offer 1.0.4 by itself (checked at most once a day; the
  download is SHA256-verified before it runs).

## Verify the download (optional)

```powershell
Get-FileHash .\MiderHive-1.0.4-x64.msi -Algorithm SHA256
# compare with the matching line in SHA256SUMS.txt
```

## Feedback

Issues and ideas: https://github.com/SiliconCoderJames/MiderHive/issues
