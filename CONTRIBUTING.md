# Contributing to MiderHive

Thanks for taking the time. Bug reports, feature ideas, docs fixes, translations and platform
patches are all welcome — **Chinese or English is fine** in issues, discussions and PRs (commit
messages in this repo happen to be Chinese; see [Commit messages](#commit-messages)).

If you are unsure whether something is a bug or just how the tool works, ask in
[Discussions](https://github.com/SiliconCoderJames/MiderHive/discussions) first.

## Ways to contribute

- **Report a bug** → [bug report template](https://github.com/SiliconCoderJames/MiderHive/issues/new?template=bug_report.yml)
- **Suggest a feature** → [feature request template](https://github.com/SiliconCoderJames/MiderHive/issues/new?template=feature_request.yml)
- **Security issue** → privately, via [SECURITY.md](SECURITY.md) — never a public issue.
- **Port it**: only Windows is verified today (CI included). Linux/macOS builds are a standard
  CMake layout but the GUI target still carries a Windows-only declaration; patches are welcome.
- **Docs & translations**: the README exists in [English](README.md) and
  [简体中文](README.zh-CN.md) — keep them in sync.

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| Windows | 10 or later, x64 | the only fully verified platform |
| Visual Studio | 2022 or newer | "Desktop development with C++" workload |
| Qt | 6.8.3 (`msvc2022_64`) | GUI only — core + CLI build without it |
| CMake | ≥ 3.25 | |
| Python | 3.8+ | for `scripts/*.py` verification and packaging helpers |
| PowerShell | 5.1+ | `scripts/*.ps1` |

## Build

```powershell
# presets (CMakePresets.json: msvc-release / msvc-debug)
cmake --preset msvc-release
cmake --build --preset release

# or explicitly
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build --config Release
```

Outputs: `build\src\gui\Release\miderhive.exe` (workbench),
`build\src\cli\Release\{platformd,agent-cli,miderhive-mcp}.exe`.

- `-DBUILD_GUI=OFF` builds core + CLI without Qt.
- Third-party dependencies (sqlite-vec, nlohmann/json, cpp-httplib) come from FetchContent. When
  GitHub is unreachable, run `powershell -File scripts\fetch-deps.ps1` first to prefetch into
  `vendor/`. The SQLite amalgamation is downloaded from sqlite.org and is *not* covered by that
  script — drop it into `vendor/` yourself for a fully offline build.

## Test

```powershell
ctest --preset release                             # unit tests
python scripts/feasibility_check.py 8787           # integration, needs a running workbench
python scripts/verify_gui_selftest.py              # offscreen GUI end-to-end, no human needed
python scripts/test_onboarding_and_safety.py       # onboarding + key rotation over HTTP
python scripts/mcp_check.py 8787 <mcp-exe> zcode <key>   # MCP stdio JSON-RPC
python scripts/verify_clients.py                   # per-client connect matrix (skips absent tools)
python scripts/fuzz_config_merge.py                # adversarial shapes against the config merger
python scripts/embed_cli_check.py                  # external embedding endpoint round-trip (mock server)
python scripts/diag_latency.py 19291               # latency-floor diagnosis (measurement, not a test)
python scripts/bench_search.py --rows 10000        # search benchmark (measurement, not a test)
python scripts/bench_concurrent.py                 # concurrency scaling, mixed/read/write (measurement)
python scripts/bench_sqlite_ceiling.py             # SQLite commit-path ceiling, no lock involved (measurement)
```

`agent-cli` can also generate and write the per-client connect configs on its own — the same
code the GUI wizard runs, useful for scripts and for wiring up headless machines:

```powershell
agent-cli connect-snippet --tool codex --command "C:\Program Files\MiderHive\miderhive-mcp.exe" --name codex --key <key>
agent-cli apply-config --tool claude-code --dir <project-root> --name claude --key <key>   # writes .mcp.json
```

What CI runs on every push and PR: the build, a check that the GUI binary actually exists, `ctest`,
the feasibility and MCP integration checks against a real `platformd` on a scratch data directory,
and **both self-tests above** — the offscreen GUI end-to-end (`gui_selftest`) and the
onboarding/key-rotation script. A regression in first-run onboarding, the health banner or key
rotation therefore turns the badge red instead of shipping.

> The offscreen GUI self-test **isolates its `QSettings`**: `gui_selftest` points the INI backend at
> a scratch directory (`QSettings::setDefaultFormat(IniFormat)` + `setPath`, see
> `src/gui/selftest_main.cpp`), so it never touches your real `HKCU\Software\miderhive` store and
> concurrent runs are safe.

## Ground rules that matter in review

- **`src/core/` stays Qt-free.** The core (database, services, HTTP API, utils) must build and be
  testable without Qt; Qt belongs in `src/gui/`.
- **Bilingual UI strings** go through `i18n::trs("中文", "English")` — no hard-coded single-language
  text in widgets.
- **HTTP responses use the envelope** `{"code":0,"message":"ok","data":…}`; failures return a
  non-zero `code` with a human-readable `message`. Keep new endpoints consistent with
  [`docs/api.md`](docs/api.md) and update the doc in the same PR.
- **Migrations must be idempotent and additive.** Existing databases upgrade in place on startup,
  and keys issued by older versions must keep authenticating.
- **No new third-party dependencies** without discussing it in an issue first — the project keeps
  its dependency surface deliberately small.
- **Nothing fails silently.** If an operation can fail, surface it (banner, translated error with a
  next step, audit entry) rather than swallowing it.

## Commit messages

Conventional-commit style with a Chinese summary, one logical change per commit:

```
feat(ui): 总览页栅格收尾——换行标签最小高度封顶、事件流防闪烁生效
fix(core): 技能调用三步写入事务化——调用记录+token 记账+审计同生共死
docs: README 补充首次接入引导与防呆设计——功能表/快速开始/FAQ/Roadmap 双语同步
```

Types in use: `feat` `fix` `docs` `test` `chore` `build` `refactor`. Mention breaking API or data
changes explicitly in the body; releases are cut from tags (see below), not from the changelog.

## Pull requests

Before opening one, please check:

- [ ] it builds on Windows (`cmake --build --preset release`) and `ctest --preset release` is green;
- [ ] GUI/onboarding/key changes also pass `python scripts/verify_gui_selftest.py` and
      `python scripts/test_onboarding_and_safety.py`;
- [ ] `docs/api.md`, `docs/mcp.md` and both READMEs are updated when behaviour or HTTP surface
      changes;
- [ ] no secrets, no personal data, no machine-specific absolute paths are committed;
- [ ] the change does not quietly break older data directories or older agent keys.

A short PR description — what hurts today, what the change does, how you verified it — is worth
more than a long one.

## Regenerating assets

The screenshots and the repository's social preview are generated, not hand-made:

```powershell
python scripts/capture_screenshots.py          # seeds a demo data home, launches the real GUI just
                                               # off-screen of your work, captures the three README
                                               # screenshots; touches nothing in your own data dir
python scripts/make_social_preview.py          # docs/assets/social-preview.png (1280x640)
```

`capture_screenshots.py` backs up and restores the two relevant `QSettings` values, and uses an
isolated data home plus a non-default port, so it is safe to run while your own workbench is open.
The brand assets (`docs/assets/logo.*`) are hand-maintained — see `docs/brand.md`.

## Packaging and releases

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 1.1.1
```

Builds, stages, self-checks, then produces the MSI (with ICE validation), the portable ZIP,
`SHA256SUMS.txt` and `latest.json` under `release/`. Pushing a `v*` tag makes CI build, test and
publish a GitHub Release using `release/RELEASE_NOTES-<version>.md` as the release body, so write
that file before tagging.

## License

By contributing you agree that your contribution is licensed under the [MIT License](LICENSE), the
same terms as the project.
