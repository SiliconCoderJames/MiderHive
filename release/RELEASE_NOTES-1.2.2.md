## MiderHive 1.2.2

Local-first collaboration hub for AI agents, Windows x64 build.

* `MiderHive-1.2.2-x64.msi` - per-user install (no administrator rights)
* `MiderHive-1.2.2-win64-portable.zip` - unzip and run `miderhive.exe`
* `SHA256SUMS.txt` - checksums

The binaries are **not code-signed**, so SmartScreen may warn on first launch.
Your data lives in `%USERPROFILE%\.miderhive` and is **not** removed on uninstall.

## API behavior changes in 1.2.2

This release fixes a batch of latent defects found by a full-tree audit. Three fixes tighten
input/visibility contracts — scripts that previously "worked" against the buggy behavior will
now get an explicit error. If your own scripts hit one of these, the error message says exactly
what to change:

1. **`embedding` now requires `embedder`.** Posting a knowledge entry with a raw `embedding`
   array but no `embedder` field used to be stored under a fake `ngram-hash-v2` label, silently
   polluting semantic search across models. Such requests now return **400**
   (`embedder is required when embedding is provided`). The MCP tools report the same error
   instead of defaulting to `"agent"`, and `agent-cli knowledge add --embedding-file` requires
   `--embedder`.

2. **`POST /api/messages/{uuid}/reply` checks visibility.** Any authenticated agent used to be
   able to reply to any message (and flip someone else's private message to read as a side
   effect). Now only the sender, recipient, `user` or a manager may reply; broadcasts stay open
   to everyone. Empty `body` is rejected as well. Failures return **400** (previously 404).

3. **`GET /api/audit` is scoped per agent.** Audit details carry point-to-point recipients and
   subjects, so a full dump effectively bypassed message visibility. Regular agents now see only
   their own audit rows; `zcode` (manager) and the workbench still see everything.

Also fixed in this release (45 findings total, see the CHANGELOG): audit rows now record the
object they describe; an interrupted backup restore self-heals on the next startup instead of
silently running on an empty database; two cross-process races are closed; LIKE wildcards in
searches are escaped; credential randomness comes from the OS CSPRNG; agent-cli `--port` works
and error paths exit non-zero; environment variables are read as UTF-8 on Windows (Chinese agent
names no longer fail with 401); the config writer can no longer produce unparseable TOML/YAML;
the updater is single-flight and reports disk failures honestly; panels no longer show read
failures as empty states; and the deploy/release/installer pipelines fail loudly instead of
silently.

## 关于升级 / Upgrading

Per-user MSI upgrades in place. If `platformd.exe` is running, the installer closes it first
(new in this release).
