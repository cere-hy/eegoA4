#!/usr/bin/env python3
"""
B2: 从 english.yaml 删除摘抄/书签功能相关的 StrId 键。
string_keys 完全由 english.yaml 决定, 删这里 = 从全部 35 语言消失。
重新生成由 platformio.ini 的 pre:gen_i18n.py 完成 (不带 --strip-unused, 不影响其他键)。

前提: 代码里已不再引用这些 StrId —— 调用方需先全树 grep 确认零引用。
本脚本只删 yaml 行, 不碰生成文件, 不碰其他 i18n 键。
"""
import re
import sys

EN = "/root/.codebuddy/artifact/2f8a7d30-9c21-4f0b-1d73-ea2050a10000/rebuild/crosspoint-reader/lib/I18n/translations/english.yaml"

# 23 个摘抄/书签键 (B2 功能退役)
KEYS = {
    "STR_BOOKMARK_ADDED", "STR_BOOKMARK_OPTION", "STR_BOOKMARK_REMOVED", "STR_BOOKMARKS",
    "STR_CLIP", "STR_CLIPPING_DELETE_FAILED", "STR_CLIPPING_EXPORT_FAILED", "STR_CLIPPING_FAILED",
    "STR_CLIPPING_LIMIT_REACHED", "STR_CLIPPING_REMOVED", "STR_CLIPPINGS", "STR_CLIPPING_SAVED",
    "STR_CLIPPING_SYNC_FAILED", "STR_CLIPPING_TOO_LONG", "STR_CONFIRM_DELETE_BOOKMARK",
    "STR_DELETE_CLIPPING_CONFIRM", "STR_NO_BOOKMARKS", "STR_NO_CLIPPINGS", "STR_SAVE_CLIPPING",
    "STR_SYNC_CLIPPINGS", "STR_SYNCING_CLIPPINGS", "STR_TOGGLE_BOOKMARK", "STR_VIEW_CLIPPINGS",
}

lines = open(EN, encoding="utf-8").read().splitlines(keepends=True)
kept = []
removed = []
for ln in lines:
    m = re.match(r"\s*(STR_[A-Z0-9_]+)\s*:", ln)
    if m and m.group(1) in KEYS:
        removed.append(m.group(1))
    else:
        kept.append(ln)

missing = KEYS - set(removed)
if missing:
    print("[WARN] 未在 english.yaml 找到:", sorted(missing), file=sys.stderr)

open(EN, "w", encoding="utf-8").write("".join(kept))
print("删除 %d 个键:" % len(removed))
for k in sorted(removed):
    print("  -", k)
