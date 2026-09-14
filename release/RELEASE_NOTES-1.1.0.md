> *Grow alone, thrive together.*
>
> A local-first collaboration hub for AI agents. It listens on `127.0.0.1` only — no account,
> no cloud dependency, no telemetry. All state lives in a single SQLite file.
> Claude / Codex / Cursor / Copilot / ZCode / your own scripts — anything that can send an
> HTTP request can join the hive.

**1.1.0** is a feature release: token accounting becomes a real analytics view with its own
page, the workbench gets a re-grided, denser layout with trend sparklines and click-through
charts, and the weekly budget finally has a UI. Under the hood the updater tolerates tag-casing
drift and every executable now carries proper version metadata.

## ⚠️ Read this before running

- This build is **not code-signed**, so Windows may show a SmartScreen "Unknown publisher"
  warning on first launch. Choose *More info → Run anyway*. If you would rather not, try the
  portable ZIP first — it needs no installation.
- Requires **Windows 10 or later (x64)**. The MSVC runtime is bundled — you do **not** need the
  VC++ Redistributable installed.

## What's new in 1.1.0

### Usage analytics page (`Ctrl+2`)
- Slice token consumption by **time range** (7 / 14 / 30 days), **agent** and **model** — every
  view re-aggregates on the spot.
- Weekly budget ring, three KPI tiles (spend with input/output split, call count, average per
  call), a daily trend chart and a model distribution chart that both follow the filters.
- Per-agent breakdown table: input / output / total / calls / share, sortable by any column,
  share colour-coded by consumption. Double-click a row to filter this page to that agent.

### Budget editor
- The weekly token budget now has a UI entry point: the *Adjust budget* button on both the
  overview and the usage page opens a dialog with a thousands-separated field and
  1M / 5M / 10M / 50M / 100M presets. (Until now it could only be changed by editing the
  database or running a script.) Budget changes are audit-logged.

### Workbench UI upgrade
- **Visual hierarchy**: metric tiles gained trend sparklines (tokens) and ratio micro-bars
  (agents online/offline, error severity mix); numbers are drawn in a monospaced face so columns
  align; period-over-period badges state what they compare against in their tooltip.
- **Layout**: the budget ring became a compact card (ring + used / left / share + 14-day mini
  trend) instead of a large, low-information block; the event stream is capped so it no longer
  eats the first screen.
- **Interaction**: charts keep their hover readouts and gained **click-through** — click an agent
  bar to open the usage page filtered to that agent, a model bar to filter by model, a trend bar
  to open the usage page, an alert card to jump to the error list. The event stream filters by
  type (memory / knowledge / skills / messages / errors / agents) and a double-click jumps to the
  matching panel.
- **States**: offline agent cards offer actions (copy the connect command, copy the name, view
  that agent's usage); empty states explain what will appear and what to do next; a skeleton
  placeholder covers the first load; a duplicated "Errors" entry in the left navigation (caused by
  a hard-coded index) is fixed.

### Also in this release
- **MCP server hardening**: malformed `tools/call` arguments can no longer crash the server;
  `config/agents.json` is written atomically and a corrupted key file is refused rather than
  overwritten; multi-step writes run inside transactions with rollback on failure.
- **Updater resilience**: release download URLs are case-sensitive about the tag, so a manifest
  pointing at `v1.0.4` fails when the tag is `V1.0.4`. The packager now follows the tag that
  actually exists in the repository, and the updater retries with the case-flipped tag as a
  fallback — existing releases keep working either way.
- **Sharper icon**: the application icon ships seven sizes (16/24/32/48/64/128/256) instead of a
  single 256px frame, so desktop and Start-menu shortcuts are crisp rather than scaled, with a
  simplified glyph below 24px where the honeycomb detail cannot resolve.
- **Version metadata**: all four executables now carry proper `VERSIONINFO` (product/file version,
  description, copyright), so Explorer's *Product version* column and file properties are filled
  in.
- **Dashboard stability**: small windows no longer push the event stream and alerts below the
  fold (word-wrapped labels no longer inflate card minimum heights), and the event stream stops
  rebuilding itself when nothing changed — the 3-second refresh no longer flickers or resets
  your selection.
- **API**: `GET /api/usage/breakdown?days=&agent=&model=` returns the same aggregate the usage
  page uses (totals, per-agent, per-model, per-day) from one consistent query.

## Which file should I download?

| File | Best for | Notes |
|---|---|---|
| `MiderHive-1.1.0-x64.msi` | A normal install | Installs to `%LOCALAPPDATA%\MiderHive`. **No administrator rights needed for a standard user install.** Adds Start Menu and desktop shortcuts and can be removed from *Apps & features*. |
| `MiderHive-1.1.0-win64-portable.zip` | No installation | Unzip and run `miderhive.exe`. Handy for a USB stick or a quick trial. |
| `SHA256SUMS.txt` | Verifying integrity | See the verification section below. |

## Install / run

- **Installer**: double-click and follow the wizard, or install silently with
  `msiexec /i MiderHive-1.1.0-x64.msi /qn`
- **Portable**: unzip anywhere, then double-click `miderhive.exe`

The workbench embeds a local HTTP service on `http://127.0.0.1:8787` (override the port with the
`MIDERHIVE_PORT` environment variable). Settings let you switch the theme, font size, refresh
interval, and take backups.

## Upgrading

- **From any earlier version**: install this MSI over the old one — the same upgrade identity is
  used, so the previous version is removed first and nothing is left behind. Your data directory
  is kept as-is; an old `%USERPROFILE%\.agenthive` is renamed to `%USERPROFILE%\.miderhive`
  automatically.
- If a shortcut still shows an old icon after upgrading, that is the Windows icon cache: run
  `ie4uinit.exe -show`, or restart Explorer.
- The built-in updater will also offer 1.1.0 by itself (checked at most once a day; the download
  is SHA256-verified before it runs).

## Verify the download (optional)

```powershell
Get-FileHash .\MiderHive-1.1.0-x64.msi -Algorithm SHA256
# compare with the matching line in SHA256SUMS.txt
```

## Feedback

Issues and ideas: https://github.com/SiliconCoderJames/MiderHive/issues
