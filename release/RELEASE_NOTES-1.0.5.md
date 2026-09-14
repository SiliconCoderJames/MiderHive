> *Grow alone, thrive together.*
>
> A local-first collaboration hub for AI agents. It listens on `127.0.0.1` only — no account,
> no cloud dependency, no telemetry. All state lives in a single SQLite file.
> Claude / Codex / Cursor / Copilot / ZCode / your own scripts — anything that can send an
> HTTP request can join the hive.

**1.0.5** turns token accounting into a real analytics view and gives the weekly budget a
place in the UI at last. It also makes the updater resilient to tag-casing drift and ships
a sharper icon across all Windows icon sizes.

## ⚠️ Read this before running

- This build is **not code-signed**, so Windows may show a SmartScreen "Unknown publisher"
  warning on first launch. Choose *More info → Run anyway*. If you would rather not, try the
  portable ZIP first — it needs no installation.
- Requires **Windows 10 or later (x64)**. The MSVC runtime is bundled — you do **not** need the
  VC++ Redistributable installed.

## What's new in 1.0.5

- **Workbench UI upgrade**: metric tiles gained trend sparklines and period-over-period badges
  (with the reference frame in the tooltip), numbers are drawn in a monospaced face so columns
  align, and the layout was re-grided — the budget ring is now a compact card with used / left /
  share figures plus a 14-day mini trend, and the event stream is capped so it no longer eats the
  first screen. Charts support hover readouts and **click-through**: click an agent bar to open the
  Usage panel filtered to that agent, a model bar to filter by model, an alert card to jump to the
  error list. The event stream filters by type (memory / knowledge / skills / messages / errors /
  agents). Offline agent cards now offer actions (copy the connect command, copy the name, view
  that agent's usage). Empty states explain what will appear and what to do next, and a skeleton
  placeholder covers the first load. A duplicated "Errors" entry in the left navigation (caused by
  a hard-coded index) is fixed.
- **Usage panel (new page, `Ctrl+2`)**: slice token consumption by time range (7 / 14 / 30 days),
  agent and model — every view re-aggregates on the spot. Includes a weekly budget ring, three
  KPI tiles (spend with input/output split, call count, average per call), a daily trend chart
  and a model distribution chart that both follow the filters, and a per-agent breakdown table
  (input / output / total / calls / share, sortable, share colour-coded by consumption).
- **Budget editor**: the weekly token budget finally has a UI entry point — the *Adjust budget*
  button on both the overview and the usage page opens a dialog with a thousands-separated
  field and 1M / 5M / 10M / 50M / 100M presets. (Until now it could only be changed by editing
  the database or running a script.) Budget changes are audit-logged.
- **Updater resilience**: release download URLs are case-sensitive about the tag, so a manifest
  pointing at `v1.0.4` fails when the tag is `V1.0.4`. The packager now follows the tag that
  actually exists in the repository, and the updater retries with the case-flipped tag as a
  fallback — existing releases keep working either way.
- **Sharper icon**: the application icon now ships seven sizes (16/24/32/48/64/128/256) instead
  of a single 256px frame, so desktop and Start-menu shortcuts are crisp rather than scaled,
  with a simplified glyph below 24px where the honeycomb detail cannot resolve.
- **API**: `GET /api/usage/breakdown?days=&agent=&model=` returns the same aggregate the panel
  uses (totals, per-agent, per-model, per-day) from one consistent query.

## Which file should I download?

| File | Best for | Notes |
|---|---|---|
| `MiderHive-1.0.5-x64.msi` | A normal install | Installs to `%LOCALAPPDATA%\MiderHive`. **No administrator rights needed for a standard user install.** Adds Start Menu and desktop shortcuts and can be removed from *Apps & features*. (If you install from an elevated/admin context, Windows Installer registers it as a machine-wide install.) |
| `MiderHive-1.0.5-win64-portable.zip` | No installation | Unzip and run `miderhive.exe`. Handy for a USB stick or a quick trial. |
| `SHA256SUMS.txt` | Verifying integrity | See the verification section below. |

## Install / run

- **Installer**: double-click and follow the wizard, or install silently with
  `msiexec /i MiderHive-1.0.5-x64.msi /qn`
- **Portable**: unzip anywhere, then double-click `miderhive.exe`

The workbench embeds a local HTTP service on `http://127.0.0.1:8787` (override the port with the
`MIDERHIVE_PORT` environment variable). Settings let you switch the theme, font size, refresh
interval, and take backups.

## Upgrading

- **From any earlier version**: just install this MSI over the old one — it uses the same
  upgrade identity, so the previous version is removed first and nothing is left behind. Your
  data directory is kept as-is; an old `%USERPROFILE%\.agenthive` is renamed to
  `%USERPROFILE%\.miderhive` automatically.
- If the desktop shortcut still shows the old icon after upgrading, that is the Windows icon
  cache: run `ie4uinit.exe -show`, or restart Explorer. This release is the first one whose
  installer carries properly sized icons.
- **Portable**: unzip into a fresh folder and run. Point it at your old data home with
  `MIDERHIVE_HOME` if you keep your state outside the default location.
- The built-in updater will also offer 1.0.5 by itself (checked at most once a day; the
  download is SHA256-verified before it runs).

## Verify the download (optional)

```powershell
Get-FileHash .\MiderHive-1.0.5-x64.msi -Algorithm SHA256
# compare with the matching line in SHA256SUMS.txt
```

## Feedback

Issues and ideas: https://github.com/SiliconCoderJames/MiderHive/issues
