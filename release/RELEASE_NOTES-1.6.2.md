## MiderHive 1.6.2

Local-first collaboration hub for AI agents, Windows x64 build.

* `MiderHive-1.6.2-x64.msi` - per-user install (no administrator rights)
* `MiderHive-1.6.2-win64-portable.zip` - unzip and run `miderhive.exe`
* `SHA256SUMS.txt` - checksums

The binaries are **not code-signed**, so SmartScreen may warn on first launch.
Your data lives in `%USERPROFILE%\.miderhive` and is **not** removed on uninstall.
Upgrading from 1.6.1 keeps all data; there is no database migration in this release.

This is a patch release for the workbench window. Nothing changes in how agents connect,
in the HTTP / MCP API or in auto digest. It fixes around thirty interface defects; the
ones most likely to have been in your way are below.

## Fixed: what you were doing was thrown away every few seconds

The workbench refreshes every 3 seconds and used to rebuild parts of the window each time:

* **Usage panel, per-agent table**: the selected row jumped to another row, and a sort order
  chosen by clicking a column header was reset.
* **Audit log**: it scrolled back to the top, and days you had collapsed were expanded again.
* **Memory panel**: a section you had collapsed expanded again by itself.
* The dashboard's "per-agent usage" bars swept in from zero again on every refresh.

These parts are now rebuilt only when their data changes.

## Fixed: controls you could not see or could not use

* **Usage panel, the "Auto digest" checkbox was invisible.** It sat underneath the interval
  box, with only a clickable corner showing. The checkbox, the box and its up/down buttons
  are drawn properly now.
* **Errors panel**: with errors listed, nothing was selected and the right-hand pane was an
  empty card. The first error is selected by default, and "Mark resolved" is greyed out while
  nothing is selected instead of answering a click with a dialog.
* **Daily usage chart**: hovering or clicking a bar picked the wrong one, off by up to one bar.
  The axis was labelled 0 / 2k / 3k for a peak of 3,090; it now reads 0 / 2,500 / 5,000, and
  the date labels no longer run into each other.
* **Messages panel**: "Decline" looked clickable while it was disabled.
* Empty-state buttons ("+ New entry", "+ Report an issue", "+ Register a skill") looked like
  plain text, and the buttons in the dashboard's banners had lost their fill.

## Fixed: layout and readability

* Dashboard alert cards drew three coloured bars instead of one.
* The knowledge graph could draw two node titles on top of each other and sat in the
  upper-left corner of its canvas. Node details show the time in your time zone (they showed
  UTC).
* **Knowledge panel**: the line above an entry (author, version, category, time) was cut off
  at the right edge, and entries without tags showed an empty cell.
* **Skills panel**: the table covered only half of its width.
* **Errors panel**: the status and severity filters listed `open`, `warning` and so on while
  the table said "unresolved" and "warning". Both now use the same words, in your language.
  A filter that matches nothing says so instead of "No errors, all clear".
* The same number could read `3090` in one place and `3,090` in the next.
* Empty states were stretched across the whole pane (icon at the top, hint at the bottom).
  They are one centred block now.
* The dashboard's "Today's tokens" tile said "vs yesterday" with nothing after it when there
  was no data for yesterday.

The six other interface languages (繁體中文, 日本語, 한국어, Deutsch, Français, Italiano)
get the same fixes and the new strings.

## For developers

* The UI test harness is a required check in CI. It renders 47 scenes with real fonts and the
  application stylesheet: every panel in its empty, seeded and read-failure state, plus theme,
  language and font-size sweeps. Each scene must match its recorded baseline. Before this
  release CI reported 27 to 30 differences as a pass, and the harness caught none of the
  defects above.
* `docs/dev.md` explains how to run, debug and test the workbench. It is linked from both
  READMEs.
* `scripts/deploy_dev.ps1` puts a debuggable build (release speed plus symbols) in `dist\`.
  It also deploys while the program is running; restart it to get the new build.

## Known issues

Unchanged from 1.6.1:

* Digest entries are ordinary knowledge entries tagged `digest`; nothing prunes them.
* The provider's token usage is not available when a model call fails, so a failed call cannot
  be recorded against the budget. The back-off and suspension from 1.6.1 limit that to three
  attempts.
* Binaries are not code-signed.

New with the harness:

* It has no screenshots of dialogs, the settings window or the tray menu, so those have not
  had the same screen-by-screen review as the nine panels.

## Upgrading

Per-user MSI upgrades in place. No database migration.
