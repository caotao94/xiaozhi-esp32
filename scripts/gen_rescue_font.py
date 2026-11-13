#!/usr/bin/env python3
"""Generate the rescue UI Chinese subset font (LVGL fmt_txt C file).

Extracts every non-ASCII character used in rescue UI string literals, instantiates
a static Noto Sans SC Regular from the variable font, and runs lv_font_conv to
produce main/rescue/rescue_font.c (font name font_rescue_20_4, 20px / 4bpp).

Requires: fontTools (system python), node/npx (for lv_font_conv@1.5.3), and a
Noto Sans SC source font (OFL). Run with the system python that has fontTools:
    python scripts/gen_rescue_font.py
"""
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "main" / "rescue" / "rescue_ui.cc"
OUT = ROOT / "main" / "rescue" / "rescue_font.c"
FONT_NAME = "font_rescue_20_4"
LV_FONT_CONV_VERSION = "1.5.3"
# 主程序通用汉字集（6845 码点）。救援仓库的固件名来自 manifest（运行时动态内容），
# 仅靠 UI 固定文案提取的子集会缺字，故并入这套通用字符集保证任意常见固件名可渲染。
COMMON_CHARSET = ROOT / "managed_components" / "78__xiaozhi-fonts" / "charsets" / "common.json"

# Candidate Noto Sans SC sources (OFL licensed). Variable font preferred.
FONT_CANDIDATES = [
    Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf"),
    Path(r"C:\Windows\Fonts\Noto Sans SC (TrueType).otf"),
]


def extract_codepoints() -> set:
    text = UI.read_text(encoding="utf-8")
    # Match double-quoted string literals (handle escaped quotes).
    literals = re.findall(r'"((?:[^"\\]|\\.)*)"', text)
    cps = set()
    for lit in literals:
        for ch in lit:
            if ord(ch) > 0x7E:
                cps.add(ord(ch))
    # 并入通用汉字集，覆盖运行时固件名等动态内容（仅 UI 文案子集会缺字）。
    if COMMON_CHARSET.exists():
        data = json.loads(COMMON_CHARSET.read_text(encoding="utf-8"))
        cps.update(c for c in data.get("codepoints", []) if c > 0x7E)
    else:
        print(f"WARNING: common charset not found: {COMMON_CHARSET}")
    return cps


def static_font(tmp: Path) -> Path:
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    src = next((p for p in FONT_CANDIDATES if p.exists()), None)
    if src is None:
        raise SystemExit("No Noto Sans SC source font found; install one (OFL).")
    out = tmp / "NotoSansSC-Regular.ttf"
    font = TTFont(str(src))
    if "fvar" in font:
        font = instancer.instantiateVariableFont(font, {"wght": 400}, inplace=False)
    font.save(str(out))
    return out


def main() -> int:
    cps = extract_codepoints()
    symbols = "".join(chr(c) for c in sorted(cps))
    print(f"non-ASCII codepoints used in rescue UI: {len(cps)}")
    if not cps:
        print("WARNING: no CJK characters found; did the UI get translated?")
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        font = static_font(tmp)
        npx = shutil.which("npx") or "npx"
        cmd = [
            npx, "--yes", f"lv_font_conv@{LV_FONT_CONV_VERSION}",
            "--no-compress", "--no-prefilter", "--force-fast-kern-format",
            "--font", str(font),
            "--format", "lvgl", "--bpp", "4", "--size", "20",
            "-r", "0x20-0x7E",
            "--symbols", symbols,
            "-o", str(OUT),
            "--lv-include", "lvgl.h",
            "--lv-font-name", FONT_NAME,
        ]
        print("running lv_font_conv ...")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            return r.returncode
    print(f"generated {OUT} ({OUT.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
