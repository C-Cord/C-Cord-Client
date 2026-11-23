# Security Architecture and Policy

C-Cord is engineered with a rigorous security architecture designed to protect user credentials, session data, voice/video streams, and host systems against malicious or faulty plugins and network adversaries.

## Core Security Mechanisms

### 1. Transport Layer Security (TLS) & SSRF Protection
- All REST, Gateway, catalog, and public plugin network communications enforce strict TLS 1.2 or TLS 1.3 certificate verification.
- Disabling certificate validation or bypassing trust stores is prohibited.
- Catalog requests and downloads enforce a strict HTTPS host allowlist and validate every redirect hop against it.
- Public plugin HTTP/WebSocket fetches apply a socket-level SSRF filter (`CURLOPT_OPENSOCKETFUNCTION`) that blocks loopback (`127.0.0.0/8`, `::1`, `0.0.0.0/8`), RFC 1918 private networks (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`), RFC 6598 CGNAT (`100.64.0.0/10`), link-local and cloud metadata addresses (`169.254.0.0/16` including `169.254.169.254`, `fe80::/10`), IPv6 Unique Local Addresses (`fc00::/7`), IPv4-mapped IPv6 private ranges, and NAT64 (`64:ff9b::/96`) private ranges.

### 2. Discord DAVE Voice and Video End-to-End Encryption (E2EE)
- Voice and video calls implement Discord's DAVE protocol utilizing pinned versions of `libdave` and `mlspp`.
- Media frames are encrypted end-to-end within the media pipeline before transmission across SFUs.

### 3. Plugin Host Isolation, OS Sandboxing & Runtime Limits
- **Process / Thread Isolation:** Lua 5.5.1 extensions execute in a dedicated out-of-process helper (`ccord-plugin-host`) on desktop and a sandboxed in-process worker thread over an IPC socketpair on Android, preventing memory faults or execution halts from crashing the main client UI.
- **OS-Level Sandbox (`ccord-plugin-host`):**
  - **Linux:** Fail-closed `PR_SET_NO_NEW_PRIVS`, Landlock filesystem sandboxing (ABI v1–v3, granting read-only access strictly to the plugin's own directory), and an expanded seccomp-BPF denylist blocking socket/network creation, `execve`/`execveat`, `fork`/`vfork`/`clone3`, `ptrace`, `io_uring`, namespaces, mounts, kernel keyrings, and filesystem mutation syscalls.
  - **macOS:** Deny-by-default Seatbelt profile restricting file reads to required system libraries and the plugin's own directory.
  - **Windows:** Job Object single-process and full UI isolation restrictions (`JOB_OBJECT_LIMIT_ACTIVE_PROCESS`, `JOB_OBJECT_UILIMIT_ALL`, plus parent-side kill-on-close) and disabling process token privileges.
- **Runtime Resource Caps:**
  - **Lua Heap Cap:** Hard ceiling of **64 MiB** on desktop (`ccord-plugin-host`, default 32 MiB) and **32 MiB** on Android (in-process worker thread, default 16 MiB). Manifest `limits.memory_bytes` (floor 1 MiB) is clamped to those ceilings and cannot exceed 64 MiB desktop / 32 MiB Android.
  - **Instruction Budget:** Default **50,000,000 instructions per event/callback**; manifest `limits.instructions` may configure a custom cap up to **100,000,000**. Exceeding memory or instruction caps immediately terminates the plugin host.

### 4. 3-Class Permission Model & Runtime Security Controls
- **Class 1 (Safe):** Low-risk UI contributions and plugin-scoped utilities (`plugin.*`, `platform.*`, `storage.*`, `settings.*`, `theme.register`, `clipboard.write`, `commands.register`, `ui.contextMenu`, `ui.actionButton`, `ui.dialog`, `ui.file`, `ui.toast`) loaded automatically. Class 1 `ui.dialog.*` modals are user-gesture-guarded and always carry a non-removable `"Plugin: <name>"` attribution badge.
- **Class 2 (C-Cord Internal):** Client customization and Discord state/action APIs (`ui.panel`, `ui.decorate`, `ui.composer`, `theme.write`, `store.read`, `messages.*`, `hooks.*`, `presence.write`, `navigation.write`, `social.write`, `voice.*`, `ui.window`, `clipboard.read`, `gateway.raw`), gated behind an **Amber warning** requiring explicit user consent.
  - **Protected Security Surfaces:** Login screens, 2FA prompts, and plugin permission/consent modals always render with C-Cord's unmutated theme and suppress all plugin wallpapers, decorators, and overlay panels.
  - **`hooks.on_link_open` Same-Host Policy:** URL rewrites must stay `http://` or `https://` on the **exact same host** as the original URL (tracking-parameter stripping and same-host path rewrites are allowed; cross-host redirects and non-HTTP(S) schemes are rejected).
  - **`gateway.raw` Redaction:** Sensitive account/session events are filtered in live transport, and the plugin dispatcher recursively strips/redacts sensitive auth/session/PII fields (`token`, `analytics_token`, `session_id`, `resume_gateway_url`, `auth_session_id_hash`, `mfa_enabled`, `email`, `phone`) from `READY`, `READY_SUPPLEMENTAL`, `SESSIONS_REPLACE`, and `AUTH_SESSION_CHANGE` payloads.
- **Class 3 (System-Level & Account):** High-privilege OS, network, and private account APIs (`fs.*`, `network.*`, `system.*`, `clipboard.image`, `account.*`), gated behind a **Red high-risk warning** requiring explicit user consent.
  - **Network Policy (`network.http`, `network.ws`, `network.local`):** `network.http` and `network.ws` are strictly for **public HTTPS/WSS** hosts: they enforce manifest `allowed_hosts` (exact domains or `*.example.com` subdomain wildcards), **cannot reach loopback** (`localhost`, `127.0.0.0/8`, `::1`, `0.0.0.0`), and apply the socket-level SSRF filter. All loopback targets (`http://127.0.0.1`, `http://localhost`, `ws://127.0.0.1`, etc.) strictly require `"network.local"`. `net.local_net.unix_send` blocks C-Cord protected paths, symlinks, abstract sockets, and high-risk host system sockets (`docker.sock`, `podman.sock`, `containerd.sock`, `dbus`, `systemd`, `ssh-agent`, `gpg-agent`, `discord-ipc-*`, `ccord-*`).
  - **Filesystem & Subprocess Hardening (`fs.*`, `system.exec`):** On POSIX, `fs.*` uses `O_NOFOLLOW`/`lstat` to reject symlinks and blocks C-Cord config/data/plugin directories, the running C-Cord executable/AppImage, `~/.ssh`, `~/.gnupg`, `~/.config/autostart`, and shell startup rc files (`~/.bashrc`, `~/.profile`, `~/.zshrc`, `~/.bash_profile`). `system.exec` / `system.spawn` launch binaries via direct `argv` arrays (no shell), scrub sensitive environment variables (`*TOKEN*`, `*SECRET*`, `*PASSWORD*`, `*API_KEY*`, `*AUTH*`, `CCORD_*`, etc.), and close inherited file descriptors `>= 3` before `execvp`, while still warning users in the consent modal that external binaries run with broad OS-user access.
  - **Account APIs (`account.*`):** `account.read` masks PII in Streamer Mode and never exposes auth tokens, passwords, TOTP secrets, or backup codes; `account.write` is strictly Human-in-the-Loop (HITL) via a native Protected Confirmation Modal.
- **Inter-Plugin RPC Anti-Escalation (`plugin.expose` / `plugin.call`):** Enforces **restricted-permission subset matching** — callers must hold every Class 2/3 restricted permission held by the target plugin (not merely the same coarse security class) — unless the provider explicitly opts in with `{ allow_lower_class = true }`.
- **Undeclared Calls & Consent Re-Prompting:** Calling any undeclared Class 2 or Class 3 API immediately terminates the plugin host (`[undeclared]`) and is re-verified server-side in the main process. Expanding restricted permissions or `allowed_hosts` (adding a domain or removing `allowed_hosts` to become unrestricted) across reloads or `.cce` updates always re-prompts user consent.

### 5. Plugin Catalog Supply-Chain Security
- **Ed25519 Signature Verification:** When pinned public keys are configured (`-DCCORD_CATALOG_PUBKEY=...` at build time or `CCORD_CATALOG_PUBKEY` at runtime, supporting comma-separated key rotation of up to 4 keys), C-Cord verifies detached Ed25519 signatures via `libsodium` on catalog plugin metadata (`ccord-plugin-v2`) and revocations feeds (`ccord-revocations-v2`).
- **Archive Integrity & Manifest Cross-Checks:** Downloaded `.cce` packages are SHA-256 verified before zip extraction, and extracted `plugin.json` metadata (plugin ID, version, permissions, and `allowed_hosts`) is verified against the signed catalog metadata. Sideloaded `.cce` installs also record their SHA-256 hash so revocation checks cover both catalog and manual installs.
- **Revocations & Consent-Gated Auto-Updates:** Revocation lists enforce both plugin-ID and package SHA-256 revocations, blocking installs and disabling already-installed revoked plugins. Auto-updates automatically pause and require explicit user review whenever an update expands restricted permissions, adds a new domain to or removes `allowed_hosts` (becoming unrestricted), carries `needs_tier_review`, or carries a danger warning.

### 6. Local File Permissions
- Authentication tokens, session files, credential caches, plugin consent records, catalog metadata, and local configuration stores are created and maintained with strict `0600` permissions (readable and writable exclusively by the owner user).

### 7. Local Unix Socket Verification (`SO_PEERCRED`)
- Inter-process communication between the main client and auxiliary local processes utilizes local Unix domain sockets secured via `SO_PEERCRED` socket peer credential verification to authenticate process identity and prevent unauthorized local connection attempts.

---

## Reporting Security Vulnerabilities

If you discover a security vulnerability within C-Cord, please **do not open public GitHub issues or pull requests**. 

Instead, report vulnerabilities privately via **GitHub Security Advisories** in this repository:
1. Navigate to the repository's **Security** tab on GitHub.
2. Click on **Advisories** and select **Report a vulnerability**.
3. Provide a clear description of the vulnerability, reproduction steps, and potential impact.

We appreciate responsible disclosure and will coordinate with reporters to address valid security concerns promptly.
