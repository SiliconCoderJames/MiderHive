## MiderHive 1.6.1

Local-first collaboration hub for AI agents, Windows x64 build.

* `MiderHive-1.6.1-x64.msi` - per-user install (no administrator rights)
* `MiderHive-1.6.1-win64-portable.zip` - unzip and run `miderhive.exe`
* `SHA256SUMS.txt` - checksums

The binaries are **not code-signed**, so SmartScreen may warn on first launch.
Your data lives in `%USERPROFILE%\.miderhive` and is **not** removed on uninstall.
Upgrading from 1.6.0 keeps all data; there is no database migration in this release.

This is a patch release for 1.6.0. Update if the ZCode tool could not be connected, or if you use
auto digest.

## Fixed: the ZCode tool could not be connected

1.6.0 shipped with a regression: the default identity that the onboarding wizard issues for the
ZCode tool (`zcode-agent`) had been made a reserved name, so connecting it failed with
`reserved agent name: zcode-agent`. 1.6.1 reverts that. A test now checks that every identity the
wizard offers can be issued.

## Fixed: auto digest could retry every 15 minutes and spend outside the budget

Auto digest (Usage panel, "Auto digest") had several defects in 1.6.0. All of them are closed
here.

* **Retry loop.** The scheduler checked "is a run due?" against the time of the last *successful*
  digest. After a failed run it was still due, so the 15-minute check called the model again,
  ignoring the interval you set — up to 96 times a day. The provider bills a failed call, but its
  tokens were never recorded, so the weekly budget did not stop it. Runs now back off for a full
  interval after every attempt, and three failures in a row suspend auto digest.
* **Suspension.** While auto digest is suspended nothing calls the model: the scheduler stays
  quiet, and "Run now", `agent-cli digest run` and `POST /api/digest/run` are refused with the
  reason. The Usage panel shows **Auto digest suspended** and why. It resumes when you restart
  MiderHive or save the LLM / digest settings again.
* **"Written to knowledge base ✓" when nothing was written.** If the knowledge-base write failed,
  1.6.0 still reported success and marked the activity as done. A failed save is now a failure,
  and the activity is summarized again next time.
* **Silently skipped activity.** If reading the activity (or the "summarized up to" mark) failed,
  1.6.0 carried on with what it could read and marked everything as done. Any read failure now
  stops the run and leaves the mark untouched.
* **The same batch summarized again and again.** With more activity than one run covers (200
  audit rows, 100 messages, 50 errors), 1.6.0 summarized nearly the same batch on every run. A
  run now covers the newest activity and reports `activity_truncated: true` when older activity
  was left out.
* **Not going over budget.** With a token budget, a run is skipped when what is left cannot cover
  it ("not enough budget left to finish one run"). 1.6.0 only checked whether the budget was
  already used up, so a digest could be the thing that pushed you over. Request budgets were
  already exact.

Skip messages now say why in words instead of printing JSON.

## Also in 1.6.1

* Developer tooling: `scripts/ci_local.ps1` runs every CI check against the local build before
  pushing; `scripts/deploy_dev.ps1` deploys the current build to `dist\` as a runnable tree.
* `POST /api/digest/run` is documented in `docs/api.md` (it was missing).
* A UI/UX test harness: 46 scripted scenes compared against checked-in baselines. It is new and
  still observation-only; it is not yet a regression gate.

## Known issues

* Digest entries are ordinary knowledge entries tagged `digest`; nothing prunes them.
* The provider's token usage is not available when a model call fails, so a failed call cannot be
  recorded against the budget. The back-off and suspension above limit that to three attempts.
* The remaining limitations of the UI harness are listed in the CHANGELOG.

## Upgrading

Per-user MSI upgrades in place. No database migration.
