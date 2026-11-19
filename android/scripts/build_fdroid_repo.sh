#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
# Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
# Builds a signed official F-Droid repository (Index v1 + v2) from signed C-Cord APKs and
# packs lightweight metadata (without heavy .apk binaries) into ccord-fdroid-repo.tar.gz.
# c-cord.org serves the signed metadata at /repo/fdroid/repo/ and 302-redirects *.apk to GitHub Releases.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$ANDROID_DIR/.." && pwd)"

APKS_DIR=""
OUT_TAR=""
REPO_URL="${CCORD_FDROID_REPO_URL:-https://c-cord.org/repo/fdroid/repo}"
KEYSTORE="${CCORD_FDROID_KEYSTORE:-}"
STORE_PASS="${CCORD_FDROID_STORE_PASS:-}"
KEY_ALIAS="${CCORD_FDROID_KEY_ALIAS:-ccord-fdroid-repo}"
KEY_PASS="${CCORD_FDROID_KEY_PASS:-}"
SDK_DIR="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
if [[ ! -d "$SDK_DIR/build-tools" && -d "$HOME/Android/Sdk/build-tools" ]]; then
  SDK_DIR="$HOME/Android/Sdk"
fi

for arg in "$@"; do
  case "$arg" in
    --apks-dir=*)   APKS_DIR="${arg#--apks-dir=}" ;;
    --out=*)        OUT_TAR="${arg#--out=}" ;;
    --repo-url=*)   REPO_URL="${arg#--repo-url=}" ;;
    --keystore=*)   KEYSTORE="${arg#--keystore=}" ;;
    --store-pass=*) STORE_PASS="${arg#--store-pass=}" ;;
    --key-alias=*)  KEY_ALIAS="${arg#--key-alias=}" ;;
    --key-pass=*)   KEY_PASS="${arg#--key-pass=}" ;;
    *)
      echo "usage: $0 --apks-dir=<dir> --out=<ccord-fdroid-repo.tar.gz> [--keystore=<jks> ...]" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$APKS_DIR" || ! -d "$APKS_DIR" ]]; then
  echo "error: --apks-dir is required and must be a directory containing signed .apk files" >&2
  exit 1
fi
if [[ -z "$OUT_TAR" ]]; then
  echo "error: --out=<path/to/ccord-fdroid-repo.tar.gz> is required" >&2
  exit 1
fi
if [[ -z "$KEYSTORE" || ! -f "$KEYSTORE" ]]; then
  echo "error: F-Droid signing keystore not found (set --keystore or CCORD_FDROID_KEYSTORE)" >&2
  exit 1
fi
if [[ -z "$KEY_PASS" ]]; then
  KEY_PASS="$STORE_PASS"
fi

if ! command -v fdroid >/dev/null 2>&1; then
  echo "error: fdroidserver ('fdroid' CLI) not found in PATH (install via pip install fdroidserver)" >&2
  exit 1
fi
if ! command -v keytool >/dev/null 2>&1; then
  echo "error: keytool not found in PATH" >&2
  exit 1
fi

KEYSTORE_ABS="$(cd "$(dirname "$KEYSTORE")" && pwd)/$(basename "$KEYSTORE")"
OUT_ABS="$(mkdir -p "$(dirname "$OUT_TAR")" && cd "$(dirname "$OUT_TAR")" && pwd)/$(basename "$OUT_TAR")"

if [[ -d "$SDK_DIR/build-tools" ]]; then
  BT_DIR="$(ls -d "$SDK_DIR"/build-tools/* 2>/dev/null | sort -V | tail -n 1 || true)"
  if [[ -n "$BT_DIR" && -d "$BT_DIR" ]]; then
    export PATH="$BT_DIR:$PATH"
  fi
  export ANDROID_HOME="$SDK_DIR"
  export ANDROID_SDK_ROOT="$SDK_DIR"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ccord-fdroid-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/repo" "$WORK/metadata/org.ccord.android/en-US/images"

# Copy Fastlane metadata and official 512px icon.
cp -a "$ANDROID_DIR/fdroid/metadata/." "$WORK/metadata/"
if [[ -f "$ROOT_DIR/assets/icons/ccord-512.png" ]]; then
  cp -f "$ROOT_DIR/assets/icons/ccord-512.png" "$WORK/metadata/org.ccord.android/en-US/images/icon.png"
  cp -f "$ROOT_DIR/assets/icons/ccord-512.png" "$WORK/repo-icon.png"
fi

# Stage all signed APKs into repo/ for fdroidserver indexing.
APK_COUNT=0
shopt -s nullglob
for apk in "$APKS_DIR"/*.apk; do
  cp -f "$apk" "$WORK/repo/$(basename "$apk")"
  APK_COUNT=$((APK_COUNT + 1))
done
shopt -u nullglob

if [[ "$APK_COUNT" -eq 0 ]]; then
  echo "error: no .apk files found in $APKS_DIR" >&2
  exit 1
fi

cat > "$WORK/config.yml" <<EOF
repo_url: "$REPO_URL"
repo_name: "C-Cord Official F-Droid Repo"
repo_icon: "repo-icon.png"
repo_description: >-
  Official F-Droid repository for C-Cord, the fast, lightweight native C23 Discord client.
archive_older: 0
keystore: "$KEYSTORE_ABS"
repo_keyalias: "$KEY_ALIAS"
keystorepass: "$STORE_PASS"
keypass: "$KEY_PASS"
use_date_from_apk: true
EOF
chmod 600 "$WORK/config.yml"

echo "==> Running fdroid update on $APK_COUNT signed APK(s)..."
(
  cd "$WORK"
  fdroid update --pretty
)

# Extract the 64-hex uppercase SHA-256 fingerprint of the F-Droid repo signing certificate.
FINGERPRINT="$(keytool -list -v -keystore "$KEYSTORE_ABS" -storepass "$STORE_PASS" -alias "$KEY_ALIAS" \
  | awk '/SHA256:/ {gsub(/:/, "", $2); print toupper($2); exit}')"
if [[ "${#FINGERPRINT}" -ne 64 ]]; then
  echo "error: failed to extract 64-char SHA-256 fingerprint from $KEYSTORE_ABS" >&2
  exit 1
fi
printf '%s\n' "$FINGERPRINT" > "$WORK/fingerprint.txt"
printf '%s\n' "$FINGERPRINT" > "$WORK/repo/fingerprint.txt"

# Strip heavy APK binaries from the repo metadata archive — GitHub Releases stores all APKs,
# and c-cord.org 302-redirects /repo/fdroid/repo/*.apk straight to GitHub Releases.
rm -f "$WORK/repo/"*.apk "$WORK/repo/"*.apk.idsig

if [[ ! -f "$WORK/repo/entry.jar" && ! -f "$WORK/repo/index-v1.jar" ]]; then
  echo "error: fdroid update did not produce entry.jar or index-v1.jar" >&2
  exit 1
fi

tar --hard-dereference -czf "$OUT_ABS" -C "$WORK" repo fingerprint.txt
echo "==> Built F-Droid repo metadata archive: $OUT_ABS ($(du -h "$OUT_ABS" | cut -f1))"
echo "==> F-Droid Repo Fingerprint: $FINGERPRINT"
