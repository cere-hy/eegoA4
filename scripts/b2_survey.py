#!/usr/bin/env python3
"""
B2 开工前置 —— 路径普查令 (审计台点核要求)。
对 §2 裁剪清单的 30 删 + 14 引用清理 + 5 保留项, 在现树 HEAD 做:
  1) 存在性核对 (EXISTS / MISSING / RENAMED?)
  2) 引用面统计 (被多少非清单文件 #include / 提及)
  3) 图纸分类计数 vs 实树枚举的差异如实标注
strings 四计数为裁剪后验收判据, 本脚本只记录裁剪前基线。
只读, 不改任何文件。
"""
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---- §2 实删 30 (按组; 图纸分类计数在注释, 枚举为准) ----
DELETE = {
    "存储层(图纸4)": [
        "src/ClippingStore.h", "src/ClippingStore.cpp",
        "src/clippings/ClippingsManager.h", "src/clippings/ClippingsManager.cpp",
    ],
    "同步预览(图纸3)": [
        "src/clippings/ClippingSync.h", "src/clippings/ClippingSync.cpp",
        "src/clippings/ClippingPreview.h",
    ],
    "书签数据(图纸4)": [
        "src/BookmarkEntry.h",
        "src/util/BookmarkFile.h", "src/util/BookmarkFile.cpp",
        "src/util/BookmarkUtil.h", "src/util/BookmarkUtil.cpp",
    ],
    "界面(图纸4)": [
        "src/activities/reader/EpubReaderBookmarksActivity.h",
        "src/activities/reader/EpubReaderBookmarksActivity.cpp",
        "src/activities/reader/EpubReaderClippingListActivity.h",
        "src/activities/reader/EpubReaderClippingListActivity.cpp",
    ],
    "图标(图纸1)": ["src/components/icons/bookmark.h"],
    "测试(图纸14)": [
        "test/clipping_preview/ClippingPreviewTest.cpp", "test/clipping_preview/CMakeLists.txt",
        "test/clipping_store/ClippingStoreTest.cpp", "test/clipping_store/CMakeLists.txt",
        "test/clipping_store/stubs/esp_random.h",
        "test/clipping_sync/ClippingSyncTest.cpp", "test/clipping_sync/run.sh",
        "test/clipping_sync/stubs/HalMemory.h", "test/clipping_sync/stubs/KOReaderCredentialStore.h",
        "test/clipping_sync/stubs/Memory.h", "test/clipping_sync/stubs/SecureHttpClient.h",
        "test/test_clipping_highlight_planes.py", "test/test_clipping_selection_text.py",
    ],
}

# ---- §2 引用清理 14 ----
REF_CLEAN = [
    "src/activities/reader/EpubReaderActivity.h", "src/activities/reader/EpubReaderActivity.cpp",
    "src/activities/reader/EpubReaderMenuActivity.h", "src/activities/reader/EpubReaderMenuActivity.cpp",
    "src/activities/reader/ClipSelectionActivity.h", "src/activities/reader/ClipSelectionActivity.cpp",
    "src/activities/ActivityResult.h",
    "src/components/themes/BaseTheme.cpp", "src/components/themes/lyra/LyraTheme.cpp",
    "src/activities/home/FileBrowserActivity.cpp",
    "src/network/CrossPointWebServer.cpp", "src/network/WebDAVHandler.cpp",
    "src/util/BookCacheUtils.cpp",
    "src/activities/reader/KOReaderSyncActivity.cpp",
    "lib/KOReaderSync/KOReaderCredentialStore.cpp",
    "src/SettingsList.cpp", "src/activities/settings/KOReaderSettingsActivity.cpp",
]

# ---- §2 保留 5 (存在性 + 引用面; 不删) ----
KEEP = [
    ("clippings/ClippingText.h", "src/clippings/ClippingText.h"),
    ("clippings/SelectionGeometry.h", "src/clippings/SelectionGeometry.h"),
    ("HomeButtonAction::Bookmark/CreateClipping 枚举", "src/HomeButtonSettings.h"),
    ("UIIcon::Bookmark 枚举", "src/components/UITheme.h"),
    ("KOReader syncClippings JSON 键", "src/activities/reader/KOReaderSyncActivity.cpp"),
]

def exists(rel):
    return os.path.isfile(os.path.join(ROOT, rel))

def grep_refs(basename, exclude_set):
    """统计 basename 被多少文件引用 (#include 或字符串提及), 排除清单内文件。"""
    out = subprocess.run(
        ["grep", "-rl", "--include=*.h", "--include=*.cpp", "--include=*.py",
         basename, "src", "lib", "test"],
        cwd=ROOT, capture_output=True, text=True).stdout.splitlines()
    return sorted(set(p for p in out if p not in exclude_set))

def main():
    all_del = [f for grp in DELETE.values() for f in grp]
    all_del_set = set(all_del)
    ref_clean_set = set(REF_CLEAN)
    print("=" * 70)
    print("B2 路径普查 — 现树 HEAD")
    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    print("HEAD =", head)
    print("=" * 70)

    print("\n### 一、实删 30 存在性 + 引用面")
    total_exists = 0
    for grp, files in DELETE.items():
        m = re.search(r"(\d+)", grp)
        print("\n[%s] 图纸计 %s, 实列 %d" % (grp, m.group(1) if m else "?", len(files)))
        for f in files:
            base = os.path.basename(f)
            ex = exists(f)
            total_exists += 1 if ex else 0
            refs = grep_refs(base, all_del_set | ref_clean_set)
            print("  %-6s %-58s refs=%d" % ("EXISTS" if ex else "MISSING", f, len(refs)))
            if refs:
                for r in refs[:6]:
                    print("           <- %s" % r)
                if len(refs) > 6:
                    print("           <- ... 另 %d 处" % (len(refs) - 6))
    print("\n实删存在: %d/%d" % (total_exists, len(all_del)))

    print("\n### 二、引用清理 14 存在性")
    rc_exists = 0
    for f in REF_CLEAN:
        ex = exists(f)
        rc_exists += 1 if ex else 0
        print("  %-6s %s" % ("EXISTS" if ex else "MISSING", f))
    print("引用清理存在: %d/%d" % (rc_exists, len(REF_CLEAN)))

    print("\n### 三、保留 5 存在性")
    for name, f in KEEP:
        print("  %-6s %-45s (%s)" % ("EXISTS" if exists(f) else "MISSING", name, f))

    print("\n### 四、strings 四计数基线 (裁剪前, 应 >0)")
    fw = open("/tmp/b2_strings_baseline.txt", "w")
    for kw in ("clipping", "Clippings", "My Clippings", "Bookmark"):
        r = subprocess.run(["grep", "-ro", kw, "src", "lib"], cwd=ROOT, capture_output=True, text=True)
        n = len(r.stdout.splitlines())
        print("  %-14s %d" % (kw, n))
        fw.write("%s %d\n" % (kw, n))
    fw.close()
    print("\n(裁剪后四计数须全 0; 本基线供对照)")

if __name__ == "__main__":
    main()
