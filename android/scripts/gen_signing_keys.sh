#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
# Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
# Generates persistent Android APK release & F-Droid repository signing keystores once
# and prints the exact GitHub Actions `gh secret set` commands for automated releases.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="${1:-$ANDROID_DIR/.keys}"
REPO_SLUG="${CCORD_GH_REPO:-C-Cord/C-Cord-Client}"

if ! command -v keytool >/dev/null 2>&1; then
  echo "error: keytool not found (install OpenJDK 17+)" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
chmod 700 "$OUT_DIR"

APK_KS="$OUT_DIR/ccord-release.jks"
FDROID_KS="$OUT_DIR/ccord-fdroid-repo.jks"

if [[ -f "$APK_KS" || -f "$FDROID_KS" ]]; then
  echo "error: keystore(s) already exist in $OUT_DIR — refusing to overwrite existing signing keys!" >&2
  echo "Back up your existing keys safely; changing them breaks in-place updates for existing users." >&2
  exit 1
fi

gen_pass() {
  python3 -c 'import secrets; print(secrets.token_urlsafe(24))'
}

APK_PASS="$(gen_pass)"
APK_ALIAS="ccord-release"
FDROID_PASS="$(gen_pass)"
FDROID_ALIAS="ccord-fdroid-repo"

echo "==> Generating Android APK release signing keystore ($APK_KS)..."
keytool -genkeypair \
  -v \
  -keystore "$APK_KS" \
  -storetype PKCS12 \
  -storepass "$APK_PASS" \
  -keypass "$APK_PASS" \
  -alias "$APK_ALIAS" \
  -keyalg RSA \
  -keysize 4096 \
  -sigalg SHA256withRSA \
  -validity 10000 \
  -dname "CN=C-Cord Release, OU=Android, O=C-Cord, L=Internet, C=US" >/dev/null 2>&1
chmod 600 "$APK_KS"

echo "==> Generating F-Droid repository index signing keystore ($FDROID_KS)..."
keytool -genkeypair \
  -v \
  -keystore "$FDROID_KS" \
  -storetype JKS \
  -storepass "$FDROID_PASS" \
  -keypass "$FDROID_PASS" \
  -alias "$FDROID_ALIAS" \
  -keyalg RSA \
  -keysize 4096 \
  -sigalg SHA256withRSA \
  -validity 10000 \
  -dname "CN=C-Cord F-Droid Repo, OU=F-Droid, O=C-Cord, L=Internet, C=US" >/dev/null 2>&1
chmod 600 "$FDROID_KS"

extract_fp() {
  keytool -list -v -keystore "$1" -storepass "$2" -alias "$3" \
    | awk '/SHA256:/ {gsub(/:/, "", $2); print toupper($2); exit}'
}

APK_FP="$(extract_fp "$APK_KS" "$APK_PASS" "$APK_ALIAS")"
FDROID_FP="$(extract_fp "$FDROID_KS" "$FDROID_PASS" "$FDROID_ALIAS")"

cat > "$OUT_DIR/secrets.env" <<EOF
# KEEP THIS FILE PRIVATE — NEVER COMMIT TO GIT!
# Back up $OUT_DIR securely (password manager / offline encrypted backup).
CCORD_ANDROID_KEYSTORE=$APK_KS
CCORD_ANDROID_STORE_PASS=$APK_PASS
CCORD_ANDROID_KEY_ALIAS=$APK_ALIAS
CCORD_ANDROID_KEY_PASS=$APK_PASS
CCORD_ANDROID_CERT_SHA256=$APK_FP

CCORD_FDROID_KEYSTORE=$FDROID_KS
CCORD_FDROID_STORE_PASS=$FDROID_PASS
CCORD_FDROID_KEY_ALIAS=$FDROID_ALIAS
CCORD_FDROID_KEY_PASS=$FDROID_PASS
CCORD_FDROID_CERT_SHA256=$FDROID_FP
EOF
chmod 600 "$OUT_DIR/secrets.env"

cat <<EOF

========================================================================
Generated persistent C-Cord Android & F-Droid signing keys in:
  $OUT_DIR

APK Signing Certificate SHA-256:
  $APK_FP

F-Droid Repo Signing Certificate SHA-256 (Fingerprint):
  $FDROID_FP

IMPORTANT: Back up $OUT_DIR safely! Never commit it to git.
To upload these secrets to GitHub Actions ($REPO_SLUG), run:

  base64 -w0 "$APK_KS"    | gh secret set ANDROID_KEYSTORE_B64   --repo "$REPO_SLUG"
  printf '%s' "$APK_PASS" | gh secret set ANDROID_STORE_PASSWORD --repo "$REPO_SLUG"
  printf '%s' "$APK_ALIAS"| gh secret set ANDROID_KEY_ALIAS      --repo "$REPO_SLUG"
  printf '%s' "$APK_PASS" | gh secret set ANDROID_KEY_PASSWORD   --repo "$REPO_SLUG"

  base64 -w0 "$FDROID_KS"    | gh secret set FDROID_KEYSTORE_B64   --repo "$REPO_SLUG"
  printf '%s' "$FDROID_PASS" | gh secret set FDROID_STORE_PASSWORD --repo "$REPO_SLUG"
  printf '%s' "$FDROID_ALIAS"| gh secret set FDROID_KEY_ALIAS      --repo "$REPO_SLUG"
  printf '%s' "$FDROID_PASS" | gh secret set FDROID_KEY_PASSWORD   --repo "$REPO_SLUG"

========================================================================
EOF
