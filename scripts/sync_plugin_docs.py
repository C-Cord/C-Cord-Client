#!/usr/bin/env python3
"""Sync & verify C-Cord Plugin API v1 docs between C-Cord and plugin-catalog.

Automatically audits C-source-vs-docs parity across:
  - Permissions (43) & API classes: src/ipc/perm.c vs validate.go vs web/docs/index.html
  - Capabilities (14): src/plugins/package/manifest.c vs validate.go vs index.html vs ccord.d.lua
  - Platforms (8): src/plugins/package/manifest.c vs validate.go vs index.html
  - Panel slots (19): src/plugin_host/sandbox/sandbox_v3_ui.c vs index.html vs ccord.d.lua
  - Context menu targets (12): src/plugin_host/sandbox/sandbox_v3_ui.c vs ccord.d.lua
  - Event names (32): sandbox_api_v1.c + sandbox_v3_ui.c vs index.html
  - Panel event types (17): src/plugins/core/plugins.c vs index.html vs ccord.d.lua
  - Panel widget types (23 strings / 22 rows): plugin_panel_compile.c vs index.html
  - Canvas draw commands (23 strings / 19 rows): plugin_panel_compile.c vs index.html
  - Panel patch ops (6): contrib_panel_patch.c vs index.html
  - Built-in earcons (20): src/app/desktop/earcons.c vs ccord.d.lua
  - Theme colors (41) & metrics (49) + exact values: assets/themes/c-cord-*.theme vs index.html
  - Banned stale doc patterns across docs/plugin-api/*, web/docs/index.html, and llms.txt
  - File sync: docs/plugin-api/ccord.d.lua -> web/docs/ccord.d.lua, index.html -> web/docs/llms.txt
"""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


CLASS_MAP = {
    "CCORD_PERM_CLASS_SAFE": 1,
    "CCORD_PERM_CLASS_UNSAFE": 2,
    "CCORD_PERM_CLASS_SYSTEM": 3,
}

# Known-broken doc patterns that previously caused agent/developer bugs.
BANNED_PATTERNS = [
    (r'style\s*=\s*"brand"', 'button uses variant = "primary", not style = "brand"'),
    (r'\baccent\s*=\s*"#', "message_style uses accent_color / accent_border, not accent"),
    (r'\btint\s*=\s*"#', "message_style uses tint_color / background_tint, not tint"),
    (r'when\s*=\s*\{\s*has_url\b', "ui.contextMenu when has no has_url predicate"),
    (r'ev\.kind\s*==\s*"click"', "panel/decor events use ev.type, not ev.kind"),
    (r'ev\.node_id\s*==', "panel/decor events use ev.id, not ev.node_id"),
    (r'upload_rgba\(\s*key\s*,\s*rgba', "upload_rgba arg order is (key, width, height, rgba_bytes)"),
    (r'\bdefault_value\s*=', "ui.dialog.prompt uses default, not default_value"),
    (r'messages\.schedule\(\s*cid\s*,\s*send_at', "messages.schedule arg order is (cid, content, due_unix_ms)"),
    (r'\bcol\s*/\s*column\b', 'only "col" is compiled in plugin_panel_compile.c, not "column"'),
    (r'zero artificial memory', "Lua heap is capped at 64 MiB desktop / 32 MiB Android"),
]


def parse_c_perms(perm_c_path: pathlib.Path) -> dict[str, int]:
    text = perm_c_path.read_text()
    out: dict[str, int] = {}
    for cls_sym, name in re.findall(r'(CCORD_PERM_CLASS_\w+),\s*"([^"]+)"', text):
        out[name] = CLASS_MAP[cls_sym]
    return out


def parse_go_perms(validate_go_path: pathlib.Path) -> dict[str, int]:
    text = validate_go_path.read_text()
    m = re.search(r"PermissionAPIClass\s*=\s*map\[string\]int\{(.*?)\n\t\}", text, re.S)
    if not m:
        raise RuntimeError("Could not locate PermissionAPIClass in validate.go")
    return {name: int(cls) for name, cls in re.findall(r'"([^"]+)"\s*:\s*([123])', m.group(1))}


def parse_go_set(validate_go_path: pathlib.Path, var_name: str) -> set[str]:
    text = validate_go_path.read_text()
    m = re.search(rf"{var_name}\s*=\s*map\[string\]struct\{{\}}\{{(.*?)\n\t\}}", text, re.S)
    if not m:
        raise RuntimeError(f"Could not locate {var_name} in validate.go")
    return set(re.findall(r'"([^"]+)"\s*:\s*\{\}', m.group(1)))


def parse_c_caps(manifest_c_path: pathlib.Path) -> set[str]:
    text = manifest_c_path.read_text()
    m = re.search(r"k_caps\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise RuntimeError("Could not locate k_caps in manifest.c")
    return set(re.findall(r'\{"([^"]+)",\s*CCORD_CAP_', m.group(1)))


def parse_c_platforms(manifest_c_path: pathlib.Path) -> set[str]:
    text = manifest_c_path.read_text()
    m = re.search(r"static ccord_status parse_platforms\b(.*?)\n\}", text, re.S)
    if not m:
        raise RuntimeError("Could not locate parse_platforms in manifest.c")
    return set(re.findall(r'strcmp\(s,\s*"([^"]+)"\)\s*==\s*0', m.group(1)))


def parse_c_str_array(path: pathlib.Path, array_name: str) -> set[str]:
    text = path.read_text()
    m = re.search(rf"{array_name}\[\]\s*=\s*\{{(.*?)\}};", text, re.S)
    if not m:
        raise RuntimeError(f"Could not locate {array_name} in {path}")
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def parse_c_events(v1_path: pathlib.Path, v3_path: pathlib.Path) -> set[str]:
    v1 = v1_path.read_text()
    v3 = v3_path.read_text()
    m1 = re.search(r"static bool event_perm_for\b(.*?)\n\}", v1, re.S)
    m3 = re.search(r"bool sb_event_perm_v3\b(.*?)\n\}", v3, re.S)
    if not m1 or not m3:
        raise RuntimeError("Could not locate event_perm_for / sb_event_perm_v3")
    return set(re.findall(r'sb_eq\(name,\s*n,\s*"([^"]+)"\)', m1.group(1) + "\n" + m3.group(1)))


def parse_c_panel_events(plugins_c_path: pathlib.Path) -> set[str]:
    text = plugins_c_path.read_text()
    m = re.search(r"static const char \*panel_event_kind_str\b(.*?)\n\}", text, re.S)
    if not m:
        raise RuntimeError("Could not locate panel_event_kind_str in plugins.c")
    return set(re.findall(r'case CCORD_PANEL_EV_\w+:\s*return "([^"]+)";', m.group(1)))


def parse_c_panel_widgets_and_cmds(compile_c_path: pathlib.Path) -> tuple[set[str], set[str]]:
    text = compile_c_path.read_text()
    m = re.search(r"pnode \*pp_compile_node_into\b(.*?)\n\}", text, re.S)
    if not m:
        raise RuntimeError("Could not locate pp_compile_node_into in plugin_panel_compile.c")
    body = m.group(1)
    widgets = set(re.findall(r'strcmp\(tstr,\s*"([^"]+)"\)\s*==\s*0', body))
    cmds = set(re.findall(r'strcmp\(cs,\s*"([^"]+)"\)\s*==\s*0', body))
    return widgets, cmds


def parse_c_patch_ops(patch_c_path: pathlib.Path) -> set[str]:
    text = patch_c_path.read_text()
    return set(re.findall(r'strcmp\(ops\[[ko]\]\.op,\s*"([^"]+)"\)\s*==\s*0', text))


def parse_c_earcons(earcons_c_path: pathlib.Path) -> set[str]:
    text = earcons_c_path.read_text()
    m = re.search(r"k_ear_names\[CCORD_EARCON_COUNT\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise RuntimeError("Could not locate k_ear_names in earcons.c")
    return set(re.findall(r'=\s*"([^"]+)"', m.group(1)))


def parse_theme_file(path: pathlib.Path) -> tuple[dict[str, str], dict[str, float]]:
    colors: dict[str, str] = {}
    metrics: dict[str, float] = {}
    section = ""
    for raw_line in path.read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            continue
        if "=" not in line:
            continue
        k, v = (part.strip() for part in line.split("=", 1))
        if section == "colors":
            colors[k] = v.lower()
        elif section == "metrics":
            metrics[k] = float(v)
    return colors, metrics


def parse_lua_alias(ccord_d_lua: pathlib.Path, alias_name: str) -> set[str]:
    text = ccord_d_lua.read_text()
    m = re.search(rf"^---@alias\s+{re.escape(alias_name)}\s+(.+)$", text, re.M)
    if not m:
        raise RuntimeError(f"Could not locate ---@alias {alias_name} in {ccord_d_lua}")
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def eval_docs_index(docs_dir: pathlib.Path) -> tuple[str, dict]:
    assets_js = docs_dir.parent / "assets" / "docs.js"
    if assets_js.is_file():
        script_src = assets_js.read_text()
    else:
        html = (docs_dir / "index.html").read_text()
        scripts = re.findall(r"<script>(.*?)</script>", html, re.S)
        if len(scripts) != 1:
            raise RuntimeError(f"Expected 1 inline <script> in docs/index.html or assets/docs.js, found {len(scripts)}")
        script_src = scripts[0]

    export_snippet = (
        "\nconst __out = {\n"
        "  llms: buildLlmsTxt() + '\\n',\n"
        "  PERMISSIONS, CAPABILITIES, PLATFORMS, EVENTS_LIST,\n"
        "  PANEL_SLOTS, PANEL_WIDGETS, CANVAS_CMDS, PATCH_OPS,\n"
        "  PANEL_EVENTS, THEME_COLORS, THEME_METRICS,\n"
        "};\n"
        "process.stdout.write(JSON.stringify(__out));\n"
    )
    if "init();" not in script_src:
        raise RuntimeError("Could not locate init(); call in docs script")
    driver = script_src.replace("init();", export_snippet, 1)
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as tf:
        tf.write(driver)
        tmp_js = pathlib.Path(tf.name)
    try:
        res = subprocess.run(["node", str(tmp_js)], capture_output=True, text=True, check=True)
        payload = json.loads(res.stdout)
        return payload["llms"], payload
    finally:
        tmp_js.unlink(missing_ok=True)


def check_set(label: str, expected: set[str], actual: set[str], errors: list[str]) -> None:
    if expected != actual:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        errors.append(f"{label}: missing={missing} extra={extra}")


def check_dict(label: str, expected: dict, actual: dict, errors: list[str]) -> None:
    if expected != actual:
        missing = sorted(set(expected) - set(actual))
        extra = sorted(set(actual) - set(expected))
        diff = {k: (expected[k], actual[k]) for k in sorted(set(expected) & set(actual)) if expected[k] != actual[k]}
        errors.append(f"{label}: missing={missing} extra={extra} value_mismatch={diff}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--catalog", default="../plugin-catalog", help="Path to plugin-catalog repo")
    ap.add_argument("--check", action="store_true", help="Verify parity without modifying files")
    args = ap.parse_args()

    ccord_root = pathlib.Path(__file__).resolve().parent.parent
    catalog_root = (ccord_root / args.catalog).resolve()
    docs_dir = catalog_root / "internal/server/web/docs"
    validate_go = catalog_root / "internal/catalog/validate.go"
    src_types = ccord_root / "docs/plugin-api/ccord.d.lua"
    dst_types = docs_dir / "ccord.d.lua"
    dst_llms = docs_dir / "llms.txt"

    llms_new, data = eval_docs_index(docs_dir)
    errors: list[str] = []

    # 1. Permissions (43) & API classes
    c_perms = parse_c_perms(ccord_root / "src/ipc/perm.c")
    go_perms = parse_go_perms(validate_go)
    html_perms = {p["name"]: int(p["cls"]) for p in data["PERMISSIONS"]}
    check_dict("Permissions perm.c vs validate.go", c_perms, go_perms, errors)
    check_dict("Permissions perm.c vs docs/index.html", c_perms, html_perms, errors)

    # 2. Capabilities (14)
    c_caps = parse_c_caps(ccord_root / "src/plugins/package/manifest.c")
    go_caps = parse_go_set(validate_go, "allowedCapabilities")
    html_caps = set(data["CAPABILITIES"])
    lua_caps = parse_lua_alias(src_types, "CcordCapability")
    check_set("Capabilities manifest.c vs validate.go", c_caps, go_caps, errors)
    check_set("Capabilities manifest.c vs docs/index.html", c_caps, html_caps, errors)
    check_set("Capabilities manifest.c vs ccord.d.lua", c_caps, lua_caps, errors)

    # 3. Platforms (8)
    c_plats = parse_c_platforms(ccord_root / "src/plugins/package/manifest.c")
    go_plats = parse_go_set(validate_go, "allowedPlatforms")
    html_plats = set(data["PLATFORMS"])
    check_set("Platforms manifest.c vs validate.go", c_plats, go_plats, errors)
    check_set("Platforms manifest.c vs docs/index.html", c_plats, html_plats, errors)

    # 4. Panel slots (19) & Context menu targets (12)
    sb_v3_ui = ccord_root / "src/plugin_host/sandbox/sandbox_v3_ui.c"
    c_slots = parse_c_str_array(sb_v3_ui, "k_slots")
    html_slots = set(data["PANEL_SLOTS"].keys())
    lua_slots = parse_lua_alias(src_types, "CcordPanelSlot")
    check_set("Panel slots sandbox_v3_ui.c vs docs/index.html", c_slots, html_slots, errors)
    check_set("Panel slots sandbox_v3_ui.c vs ccord.d.lua", c_slots, lua_slots, errors)

    c_targets = parse_c_str_array(sb_v3_ui, "k_targets")
    lua_targets = parse_lua_alias(src_types, "CcordMenuTarget")
    check_set("Context menu targets sandbox_v3_ui.c vs ccord.d.lua", c_targets, lua_targets, errors)

    # 5. Event names (32)
    c_events = parse_c_events(ccord_root / "src/plugin_host/sandbox/sandbox_api_v1.c", sb_v3_ui)
    html_events = {e["name"] for e in data["EVENTS_LIST"]}
    check_set("Events sandbox_api_v1.c+v3 vs docs/index.html", c_events, html_events, errors)

    # 6. Panel event types (17)
    c_pev = parse_c_panel_events(ccord_root / "src/plugins/core/plugins.c")
    html_pev = set(data["PANEL_EVENTS"])
    lua_pev = parse_lua_alias(src_types, "CcordPanelEventType")
    check_set("Panel events plugins.c vs docs/index.html", c_pev, html_pev, errors)
    check_set("Panel events plugins.c vs ccord.d.lua", c_pev, lua_pev, errors)

    # 7. Compiled widget types & canvas draw commands
    c_widgets, c_cmds = parse_c_panel_widgets_and_cmds(ccord_root / "src/ui/plugins/plugin_panel_compile.c")
    html_widgets = {part.strip() for row in data["PANEL_WIDGETS"] for part in row[1].split("/")}
    html_cmds = {part.strip() for row in data["CANVAS_CMDS"] for part in row[0].split("/")}
    check_set("Panel widgets plugin_panel_compile.c vs docs/index.html", c_widgets, html_widgets, errors)
    check_set("Canvas commands plugin_panel_compile.c vs docs/index.html", c_cmds, html_cmds, errors)

    # 8. Panel patch ops (6)
    c_ops = parse_c_patch_ops(ccord_root / "src/contrib/contrib_panel_patch.c")
    html_ops = {row[0] for row in data["PATCH_OPS"]}
    check_set("Panel patch ops contrib_panel_patch.c vs docs/index.html", c_ops, html_ops, errors)

    # 9. Built-in earcons (20)
    c_earcons = parse_c_earcons(ccord_root / "src/app/desktop/earcons.c")
    lua_earcons = parse_lua_alias(src_types, "CcordEarconKind")
    check_set("Earcons earcons.c vs ccord.d.lua", c_earcons, lua_earcons, errors)

    # 10. Theme colors (41) & metrics (49) + exact values
    dk_c, dk_m = parse_theme_file(ccord_root / "assets/themes/c-cord-dark.theme")
    lt_c, _ = parse_theme_file(ccord_root / "assets/themes/c-cord-light.theme")
    am_c, _ = parse_theme_file(ccord_root / "assets/themes/c-cord-amoled.theme")
    expected_colors = {k: (dk_c[k], lt_c[k], am_c[k]) for k in dk_c}
    html_colors = {row[0]: (row[1].lower(), row[2].lower(), row[3].lower()) for row in data["THEME_COLORS"]}
    html_metrics = {row[0]: float(row[1]) for row in data["THEME_METRICS"]}
    check_dict("Theme colors assets/themes vs docs/index.html", expected_colors, html_colors, errors)
    check_dict("Theme metrics assets/themes vs docs/index.html", dk_m, html_metrics, errors)

    # 11. Scan docs for banned stale patterns
    scan_targets = [
        ("C-Cord/docs/plugin-api/ccord.d.lua", src_types.read_text()),
        ("C-Cord/docs/plugin-api/api-reference.md", (ccord_root / "docs/plugin-api/api-reference.md").read_text()),
        ("C-Cord/docs/plugin-api/README.md", (ccord_root / "docs/plugin-api/README.md").read_text()),
        ("plugin-catalog/web/docs/index.html", (docs_dir / "index.html").read_text()),
        ("generated llms.txt", llms_new),
    ]
    assets_js = docs_dir.parent / "assets" / "docs.js"
    if assets_js.is_file():
        scan_targets.append(("plugin-catalog/web/assets/docs.js", assets_js.read_text()))
    for fname, content in scan_targets:
        for pat, hint in BANNED_PATTERNS:
            if re.search(pat, content):
                errors.append(f"Banned stale pattern /{pat}/ found in {fname} ({hint})")

    # 12. Optional Lua 5.5 syntax check for ccord.d.lua and all Lua examples in llms.txt
    luac = shutil.which("luac5.5")
    if luac:
        r = subprocess.run([luac, "-p", str(src_types)], capture_output=True, text=True)
        if r.returncode != 0:
            errors.append(f"ccord.d.lua failed luac5.5 -p: {r.stderr.strip()}")
        for idx, block in enumerate(re.findall(r"```lua\n(.*?)\n```", llms_new, re.S), 1):
            rb = subprocess.run([luac, "-p", "-"], input=block, capture_output=True, text=True)
            if rb.returncode != 0:
                errors.append(f"llms.txt Lua example #{idx} failed luac5.5 -p: {rb.stderr.strip()}")

    if errors:
        print("ERROR: C-Cord docs parity check failed:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    if args.check:
        if dst_types.read_text() != src_types.read_text():
            print("ERROR: web/docs/ccord.d.lua is out of sync with C-Cord/docs/plugin-api/ccord.d.lua", file=sys.stderr)
            return 1
        if dst_llms.read_text() != llms_new:
            print("ERROR: web/docs/llms.txt is out of sync with web/docs/index.html", file=sys.stderr)
            return 1
        print(
            f"OK: verified {len(c_perms)} perms, {len(c_caps)} caps, {len(c_plats)} platforms, "
            f"{len(c_events)} events, {len(c_slots)} panel slots, {len(c_widgets)} widgets, "
            f"{len(c_cmds)} canvas cmds, {len(c_pev)} panel events, {len(c_ops)} patch ops, "
            f"{len(c_earcons)} earcons, {len(dk_c)} colors, {len(dk_m)} metrics; "
            f"ccord.d.lua and llms.txt are in full parity."
        )
        return 0

    shutil.copyfile(src_types, dst_types)
    dst_llms.write_text(llms_new)
    print(
        f"Synced ccord.d.lua and llms.txt ({len(llms_new)} bytes); verified "
        f"{len(c_perms)} perms, {len(c_caps)} caps, {len(c_plats)} platforms, "
        f"{len(c_events)} events, {len(c_slots)} slots, {len(c_widgets)} widgets, "
        f"{len(c_cmds)} canvas cmds, {len(c_earcons)} earcons, {len(dk_c)} colors, {len(dk_m)} metrics."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
