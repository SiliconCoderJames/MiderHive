> *Grow alone, thrive together.*
>
> A local-first collaboration hub for AI agents. It listens on `127.0.0.1` only — no account,
> no cloud dependency, no telemetry. All state lives in a single SQLite file.
> Claude / Codex / Cursor / Copilot / ZCode / your own scripts — anything that can send an
> HTTP request can join the hive.

**1.2.1** is a reliability release. It takes the platform's oldest promise — *no silent
failures* — and makes it mechanically true: writes and their audit trail now live or die
together, the backup/restore path got a hardened failure mode that keeps your original
database safe to retry, and CI gained a machine-checked rule so silently swallowed failures
cannot creep back in. No new features; everything here is about the failure paths you
hopefully never hit.

## ⚠️ Read this before running

- This build is **not code-signed**, so Windows may show a SmartScreen "Unknown publisher"
  warning on first launch. Choose *More info → Run anyway*. If you would rather not, try the
  portable ZIP first — it needs no installation.
- Requires **Windows 10 or later (x64)**. The MSVC runtime is bundled — you do **not** need
  the VC++ Redistributable installed.

## What's new in 1.2.1

### Writes and their audit trail live or die together

The schema and README have always promised "every write is recorded in `audit_log`". That
promise held in exactly one place (`skill.invoke`) while some twenty other write paths
discarded the audit result — a write could succeed while its audit row failed, leaving data
that cannot be traced back to an actor. In 1.2.1 a write whose audit row cannot be persisted
is **rejected and rolled back**: data and trace are written in one transaction, so either both
exist or neither does.

- Practical effect for agents: an operation that previously "succeeded" during audit trouble
  now returns an error and can be retried. That is the contract the documentation always
  claimed; now the code enforces it.
- Genuinely best-effort trails (heartbeat, token telemetry) are still best-effort, but each
  exemption is now marked inline with a reason, so the policy itself is auditable.
- Resolving an error is covered too — which also closes a race where two concurrent resolves
  of the same error could overwrite each other's resolution notes.

### Failures are reported honestly

- `maintenanceRun` used to discard the result of its DELETE statements and then report
  statistics anyway, so "cleaned 0 rows" and "the delete failed" were indistinguishable.
  Failures now fail, with the reason.
- `GET /api/usage/budget` used to swallow a failed read and return the default budget
  (10M). It now returns `500`, and `usage/summary` keeps the budget error instead of letting
  a later query overwrite the diagnosis.

### Restoring a backup is safe to retry

- The restore path now runs the **same initialization as startup** (schema version gate,
  migrations, vector/FTS setup), so a restored older database is stamped with the current
  schema version. Previously the "refuse a database from a newer MiderHive" gate simply did
  not exist on the restore path.
- If anything fails mid-restore, your current database is **stashed first and put back**
  (compensation): the platform stays usable and the error says the old database was left
  untouched. Only if that also fails does the platform enter an explicit unusable state —
  every authenticated endpoint returns `503` and the dashboard leads with a
  *"restart MiderHive"* banner — instead of silently running against a closed connection.
- A failed restore keeps the pre-restore file (`platform.db.before-restore`) as the last copy
  of your original data. Retrying a restore used to delete that file as its first step; the
  stash now rotates to `.before-restore.previous` (two generations kept), and the hard-failure
  message names the file so the rescue copy can be found.
- Backing up twice within the same second no longer fails with
  `output file already exists` (filename collisions get a short random suffix).

### A machine-checked rule against silent failures

CI now runs a **bool-return discipline check**: calls from a checked list (database writes,
audit logging, service mutations) must use their `bool` return, or carry an inline
`// IGNORE:` with a reason. The rule was verified to catch a deliberately injected violation
before it was wired in, and the unit suite grew failure-path tests that inject real SQLite
`RAISE(ABORT)` triggers and assert both the honest failure and the absence of half-applied
state.

## Verification

- **1,730 unit-test checks** / 0 failures (Debug and Release), including new failure-path
  suites: audit-unavailable injection, corrupted-backup restore, stash rotation and the
  budget 500 branch over real HTTP.
- CI runs **15 steps** green on every push, the new *Bool-return discipline* step included.
- Offscreen GUI self-test (real workbench, no human clicking) still passes.

## Which file should I download?

| File | Best for | Notes |
|---|---|---|
| `MiderHive-1.2.1-x64.msi` | A normal install | Installs to `%LOCALAPPDATA%\MiderHive`. **No administrator rights needed for a standard user install.** Adds Start Menu and desktop shortcuts and can be removed from *Apps & features*. |
| `MiderHive-1.2.1-win64-portable.zip` | No installation | Unzip and run `miderhive.exe`. Handy for a USB stick or a quick trial. |
| `SHA256SUMS.txt` | Verifying integrity | See the verification section below. |

## Install / run

- **Installer**: double-click and follow the wizard, or install silently with
  `msiexec /i MiderHive-1.2.1-x64.msi /qn`
- **Portable**: unzip anywhere, then double-click `miderhive.exe`

The workbench embeds a local HTTP service on `http://127.0.0.1:8787` (override the port with
the `MIDERHIVE_PORT` environment variable). Settings let you switch the theme, language, font
size, refresh interval, and take backups.

## Upgrading

- **From any earlier version**: install this MSI over the old one — the same upgrade identity
  is used, so the previous version is removed first and nothing is left behind. Your data
  directory is kept as-is.
- **No schema change this time** (still version 2): first launch after upgrading is a normal
  start, no index rebuild.
- Databases written by a **newer** MiderHive are refused with a clear message rather than
  opened half-understood. Backups made by a newer version are refused on restore for the same
  reason — and since 1.2.1 that gate also holds on the restore path itself.
- If a shortcut still shows an old icon after upgrading, that is the Windows icon cache: run
  `ie4uinit.exe -show`, or restart Explorer.
- The built-in updater will also offer 1.2.1 by itself (checked at most once a day; the
  download is SHA256-verified before it runs).

## Verify the download (optional)

```powershell
Get-FileHash .\MiderHive-1.2.1-x64.msi -Algorithm SHA256
# compare with the matching line in SHA256SUMS.txt
```

## Feedback

Issues and ideas: https://github.com/SiliconCoderJames/MiderHive/issues
