#!/usr/bin/env python3
"""
B1 归档脚本 —— 四段 merge 成 factory 刷机包 + 逐字节 selfcheck。

分段布局（ESP32-S3, 16MB, 与 partitions.csv 一致）：
    bootloader  0x0000   .pio/build/eego_a4/bootloader.bin
    partitions  0x8000   .pio/build/eego_a4/partition_table.bin
    otadata     0xE000   <framework>/tools/partitions/boot_app0.bin
    app         0x10000  .pio/build/eego_a4/firmware.bin

数账：factory.bin - app.bin 恒为 65536 字节 (0x10000)。

MANIFEST.json 记录各段 sha256 / 尺寸 / 偏移与 app sha256，
selfcheck 把 merge 出的 factory 按每段偏移切回、逐字节 cmp 原始段。
归档/校验失败一律 exit!=0 并打到 stderr，绝不静默。
"""
import argparse
import hashlib
import json
import os
import sys

BOOTLOADER_OFF = 0x0000
PARTITIONS_OFF = 0x8000
OTADATA_OFF = 0xE000
APP_OFF = 0x10000
FLASH_SIZE = 16 * 1024 * 1024


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def die(msg):
    print("[archive][ERROR] " + msg, file=sys.stderr)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True, help=".pio/build/eego_a4")
    ap.add_argument("--boot-app0", required=True, help="path to boot_app0.bin")
    ap.add_argument("--out", required=True, help="output firmware.factory.bin path")
    ap.add_argument("--manifest", required=True, help="output MANIFEST.json path")
    ap.add_argument("--source-date-epoch", default=os.environ.get("SOURCE_DATE_EPOCH", ""),
                    help="reproducible build epoch (default: $SOURCE_DATE_EPOCH); recorded in MANIFEST")
    args = ap.parse_args()

    segs = {
        "bootloader": (os.path.join(args.build_dir, "bootloader.bin"), BOOTLOADER_OFF),
        "partitions": (os.path.join(args.build_dir, "partitions.bin"), PARTITIONS_OFF),
        "otadata": (args.boot_app0, OTADATA_OFF),
        "app": (os.path.join(args.build_dir, "firmware.bin"), APP_OFF),
    }
    for name, (path, _) in segs.items():
        if not os.path.isfile(path):
            die("missing segment %s: %s" % (name, path))

    # 按偏移顺序拼接，段间用 0xFF 填充（Flash 擦除态）
    blob = bytearray()
    placed = []
    for name in ("bootloader", "partitions", "otadata", "app"):
        path, off = segs[name]
        data = open(path, "rb").read()
        if len(blob) < off:
            blob.extend(b"\xff" * (off - len(blob)))
        if len(blob) != off:
            die("segment %s overlaps previous (cur=%d off=%d)" % (name, len(blob), off))
        blob.extend(data)
        placed.append({"name": name, "offset": off, "size": len(data),
                       "sha256": sha256(path), "src": path})

    factory_size = len(blob)
    app_size = os.path.getsize(segs["app"][0])
    delta = factory_size - app_size
    if delta != APP_OFF:
        die("factory-app=%d, expected %d" % (delta, APP_OFF))
    if factory_size > FLASH_SIZE:
        die("factory size %d > flash %d" % (factory_size, FLASH_SIZE))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(blob)

    # selfcheck: 从 factory 按偏移切回，逐字节 cmp 原始段
    with open(args.out, "rb") as f:
        check = f.read()
    for seg in placed:
        off, size, path = seg["offset"], seg["size"], seg["src"]
        expect = open(path, "rb").read()
        actual = check[off:off + size]
        if actual != expect:
            die("selfcheck FAILED for %s (byte cmp mismatch)" % seg["name"])
        if sha256(path) != seg["sha256"]:
            die("selfcheck FAILED for %s (sha256 changed)" % seg["name"])

    manifest = {
        "schema": "eegoA4-port/B1/factory-manifest-v1",
        "platform": "espressif32@55.03.311 (pioarduino / ESP-IDF 5.5.5 / Arduino 3.3.11)",
        "env": "eego_a4",
        "flash_size": FLASH_SIZE,
        "factory_size": factory_size,
        "app_size": app_size,
        "factory_minus_app": delta,
        "factory_sha256": sha256(args.out),
        "source_date_epoch": args.source_date_epoch,
        "segments": placed,
        "selfcheck": "PASS (byte-cmp + sha256, all 4 segments)",
    }
    with open(args.manifest, "w") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    print("[archive] OK factory=%s size=%d app=%d delta=%d" %
          (args.out, factory_size, app_size, delta))
    print("[archive] factory_sha256=%s" % manifest["factory_sha256"])
    print("[archive] selfcheck PASS, manifest=%s" % args.manifest)


if __name__ == "__main__":
    main()
