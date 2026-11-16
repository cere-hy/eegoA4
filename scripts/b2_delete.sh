#!/usr/bin/env bash
# B2 阶段1: 删除 §2 清单的 30 个摘抄/书签文件。
# 精确路径来自 B2 路径普查 (30/30 存在)。保留 5 项不在本清单。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

FILES=(
  # 存储层 4
  src/ClippingStore.h src/ClippingStore.cpp
  src/clippings/ClippingsManager.h src/clippings/ClippingsManager.cpp
  # 同步与预览 3
  src/clippings/ClippingSync.h src/clippings/ClippingSync.cpp
  src/clippings/ClippingPreview.h
  # 书签数据 5 (图纸写4, 实列5)
  src/BookmarkEntry.h src/util/BookmarkFile.h src/util/BookmarkFile.cpp
  src/util/BookmarkUtil.h src/util/BookmarkUtil.cpp
  # 界面 4
  src/activities/reader/EpubReaderBookmarksActivity.h src/activities/reader/EpubReaderBookmarksActivity.cpp
  src/activities/reader/EpubReaderClippingListActivity.h src/activities/reader/EpubReaderClippingListActivity.cpp
  # 图标 1
  src/components/icons/bookmark.h
  # 测试 13 (图纸写14, 实列13)
  test/clipping_preview/ClippingPreviewTest.cpp test/clipping_preview/CMakeLists.txt
  test/clipping_store/ClippingStoreTest.cpp test/clipping_store/CMakeLists.txt test/clipping_store/stubs/esp_random.h
  test/clipping_sync/ClippingSyncTest.cpp test/clipping_sync/run.sh
  test/clipping_sync/stubs/HalMemory.h test/clipping_sync/stubs/KOReaderCredentialStore.h
  test/clipping_sync/stubs/Memory.h test/clipping_sync/stubs/SecureHttpClient.h
  test/test_clipping_highlight_planes.py test/test_clipping_selection_text.py
)

n=0
for f in "${FILES[@]}"; do
  if [ -f "$f" ]; then
    rm -f "$f" && echo "[del] $f" && n=$((n+1))
  else
    echo "[skip] 不存在: $f" >&2
  fi
done
echo "===== 实删 $n 文件 ====="
# 清理空目录
rmdir test/clipping_preview test/clipping_preview/stubs test/clipping_store/stubs test/clipping_store \
      test/clipping_sync/stubs test/clipping_sync 2>/dev/null
echo "===== 保留项确认 (应仍存在) ====="
for f in src/clippings/ClippingText.h src/clippings/SelectionGeometry.h; do
  [ -f "$f" ] && echo "[keep] $f" || echo "[ALERT] 保留项丢失: $f" >&2
done
echo "done."
