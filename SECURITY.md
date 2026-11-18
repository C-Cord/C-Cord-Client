# Security Architecture and Policy

C-Cord is engineered with a rigorous security architecture designed to protect user credentials, session data, voice/video streams, and host systems against malicious or faulty plugins and network adversaries.

## Core Security Mechanisms

### 1. Transport Layer Security (TLS)
- All REST and Gateway communications enforce strict TLS 1.2 or TLS 1.3 certificate verification.
- Disabling certificate validation or bypassing trust stores is prohibited.

### 2. Discord DAVE Voice and Video End-to-End Encryption (E2EE)
- Voice and video calls implement Discord's DAVE protocol utilizing pinned versions of `libdave` and `mlspp`.
- Media frames are encrypted end-to-end within the media pipeline before transmission across SFUs.

### 3. Out-of-Process Plugin Host Isolation & 3-Class Permission Model
- Lua 5.5 extensions execute in a dedicated, unprivileged out-of-process daemon (`ccord-plugin-host`), ensuring memory faults or execution halts cannot crash the main client UI.
- **3-Class Permission & Consent Model**:
  - **Class 1 (Safe)**: Low-risk UI contributions and plugin-scoped utilities (`ui.*`, `storage.*`, `clipboard.write`, `commands.register`) loaded automatically.
  - **Class 2 (C-Cord Internal)**: Intermediate privileges accessing or mutating live client state (`store.*`, `messages.*`, `session.*`, `theme.*`), gated behind an **Amber warning** requiring explicit user consent.
  - **Class 3 (System-Level)**: High-privilege host access (`fs.read`, `fs.write`, `system.exec`, `system.env`), gated behind a **Red warning** requiring explicit user consent. Once authorized, Class 3 extensions have full host filesystem/system access with no artificial Lua memory or instruction caps; privilege escalation across plugin reloads or `.cce` updates always re-prompts for consent.

### 4. Local File Permissions
- Authentication tokens, session files, credential caches, and local configuration stores are created and maintained with strict `0600` permissions (readable and writable exclusively by the owner user).

### 5. Local Unix Socket Verification (`SO_PEERCRED`)
- Inter-process communication between the main client and plugin/auxiliary helper processes utilizes local Unix domain sockets secured via `SO_PEERCRED` socket peer credential verification to authenticate process identity and prevent unauthorized local connection attempts.

---

## Reporting Security Vulnerabilities

If you discover a security vulnerability within C-Cord, please **do not open public GitHub issues or pull requests**. 

Instead, report vulnerabilities privately via **GitHub Security Advisories** in this repository:
1. Navigate to the repository's **Security** tab on GitHub.
2. Click on **Advisories** and select **Report a vulnerability**.
3. Provide a clear description of the vulnerability, reproduction steps, and potential impact.

We appreciate responsible disclosure and will coordinate with reporters to address valid security concerns promptly.
