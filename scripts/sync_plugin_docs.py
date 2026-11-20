#!/usr/bin/env python3
"""Sync & verify C-Cord Plugin API v1 docs between C-Cord and plugin-catalog.

Checks parity across:
  - C-Cord/src/ipc/perm.c vs plugin-catalog/internal/catalog/validate.go
  - C-Cord/docs/plugin-api/ccord.d.lua -> plugin-catalog/internal/server/web/docs/ccord.d.lua
  - plugin-catalog/internal/server/web/docs/index.html -> web/docs/llms.txt
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


def parse_c_perms(perm_c_path: pathlib.Path) -> set[str]:
    text = perm_c_path.read_text()
    return set(re.findall(r'CCORD_PERM_CLASS_\w+,\s*"([^"]+)"', text))


def parse_go_perms(validate_go_path: pathlib.Path) -> set[str]:
    text = validate_go_path.read_text()
    m = re.search(r"PermissionAPIClass\s*=\s*map\[string\]int\{(.*?)\n\t\}", text, re.S)
    if not m:
        raise RuntimeError("Could not locate PermissionAPIClass in validate.go")
    return set(re.findall(r'"([^"]+)"\s*:\s*[123]', m.group(1)))


def regen_llms_txt(docs_dir: pathlib.Path) -> str:
    html = (docs_dir / "index.html").read_text()
    scripts = re.findall(r"<script>(.*?)</script>", html, re.S)
    if len(scripts) != 1:
        raise RuntimeError(f"Expected 1 <script> in docs/index.html, found {len(scripts)}")
    gen_js = scripts[0].replace("init();", "") + "\nprocess.stdout.write(buildLlmsTxt() + '\\n');\n"
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as tf:
        tf.write(gen_js)
        tmp_js = pathlib.Path(tf.name)
    try:
        res = subprocess.run(["node", str(tmp_js)], capture_output=True, text=True, check=True)
        return res.stdout
    finally:
        tmp_js.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--catalog", default="../plugin-catalog", help="Path to plugin-catalog repo")
    ap.add_argument("--check", action="store_true", help="Verify parity without modifying files")
    args = ap.parse_args()

    ccord_root = pathlib.Path(__file__).resolve().parent.parent
    catalog_root = (ccord_root / args.catalog).resolve()
    docs_dir = catalog_root / "internal/server/web/docs"

    c_perms = parse_c_perms(ccord_root / "src/ipc/perm.c")
    go_perms = parse_go_perms(catalog_root / "internal/catalog/validate.go")
    if c_perms != go_perms:
        missing_go = sorted(c_perms - go_perms)
        extra_go = sorted(go_perms - c_perms)
        print(f"ERROR: Permission drift between perm.c and validate.go! missing={missing_go} extra={extra_go}", file=sys.stderr)
        return 1

    src_types = ccord_root / "docs/plugin-api/ccord.d.lua"
    dst_types = docs_dir / "ccord.d.lua"
    llms_new = regen_llms_txt(docs_dir)
    dst_llms = docs_dir / "llms.txt"

    if args.check:
        if dst_types.read_text() != src_types.read_text():
            print("ERROR: web/docs/ccord.d.lua is out of sync with C-Cord/docs/plugin-api/ccord.d.lua", file=sys.stderr)
            return 1
        if dst_llms.read_text() != llms_new:
            print("ERROR: web/docs/llms.txt is out of sync with web/docs/index.html", file=sys.stderr)
            return 1
        print(f"OK: {len(c_perms)} permissions, ccord.d.lua, and llms.txt are in full parity.")
        return 0

    shutil.copyfile(src_types, dst_types)
    dst_llms.write_text(llms_new)
    print(f"Synced ccord.d.lua and llms.txt ({len(llms_new)} bytes); verified {len(c_perms)} permissions match perm.c.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
