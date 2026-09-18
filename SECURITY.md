# Security Policy

MiderHive runs on your own machine and holds data that other agents trust it with — API keys,
private notes, project memory. Security reports are taken seriously and answered.

## Supported versions

| Version | Supported |
|---|---|
| 1.2.x | ✅ current release line |
| 1.1.x and older | ❌ please upgrade first — fixes land on 1.2.x only |

## Reporting a vulnerability

**Do not open a public issue.** Use one of these private channels:

- **[Report a vulnerability](https://github.com/SiliconCoderJames/MiderHive/security/advisories/new)**
  (repository → *Security* → *Report a vulnerability*) — preferred, it keeps the whole thread
  private and lets us publish a coordinated advisory.
- Email **`13371891127@139.com`** if you cannot use GitHub, or if the report contains material you
  would rather not upload.

Please include, as far as you can:

- the version (bottom-left of the workbench) and how you installed it;
- a reproduction, or the exact request/response if it is an API issue;
- the impact you believe it has, and whether an attacker needs to already run code as your
  Windows user;
- any suggested fix.

**What to expect:** acknowledgement within about 7 days, an assessment with a fix plan or a
reasoned rejection, and credit in the release notes when a fix ships — unless you prefer to stay
anonymous. Please give a fix a reasonable window before publishing details.

## Threat model — what MiderHive assumes

MiderHive is a **local-first, single-user tool**, and its defaults follow from that:

- The HTTP service binds **`127.0.0.1` only** — no TLS, no remote access, no cloud, no telemetry.
- The trust boundary is your **Windows user account**. Any process already running as you can read
  the data directory, including `config/master.key` and the plaintext agent keys in
  `config/agents.json`.
- Those key files are stored in plaintext **by design**: the workbench must be able to show a key
  once and rotate it. Losing the file is a recoverable condition, not a breach — the dashboard
  says so and offers one-click rotation.
- Anything you expose beyond localhost (a reverse proxy, `0.0.0.0` binding, port forwarding) is
  your own deployment decision, and it is outside what the project can protect.

## In scope

- Authentication or authorization bypass on the HTTP API or the MCP stdio server: reaching an
  endpoint without a valid agent key, or acting as master when you hold only an agent key.
- Key material leaking into places it should not be: API responses, audit rows, logs, error
  details, backups.
- Path traversal or arbitrary file read/write through any endpoint (backup, restore, exports).
- SQL injection, or memory corruption / crash caused by malformed or oversized input.
- Agent impersonation: sending, reading or resolving another agent's messages, knowledge or
  errors, or self-assigning a task that was not addressed to you.

## Out of scope

- The binaries are **not code-signed**, so SmartScreen warns on first launch. Documented, expected.
- Exposure that follows from binding the port to a network interface, or from publishing it
  through a tunnel or proxy.
- Another local process of the same user reading or deleting the data directory.
- Data you asked the tool to delete, and a lost master key — it is unrecoverable by design; make a
  backup (`VACUUM INTO` snapshot) and keep the key somewhere safe.
- Retrieval quality of the n-gram fuzzy vector search, and token accounting being observation-only
  (it never blocks a call).
