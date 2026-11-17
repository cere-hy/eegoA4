#!/usr/bin/env python3
"""Generate a minimal RPD1 fixture byte-for-byte identical to build_dict.py's
serialization, for the rpd1_core host test. This is a TEST FIXTURE, not a port
of build_dict.py (which stays on the pico side as format reference only)."""
import struct, sys

MAGIC = b"RPD1\0\0\0\0"
VERSION = 1
BLOCK_LOG2 = 4          # small block so sparse index exercises multiple slots
HEADER_SIZE = 64
BLOCK = 1 << BLOCK_LOG2

# (headword_lower, definition_text) — sorted by memcmp order of utf-8 bytes.
entries_data = [
    ("apple", "/ˈæp l/\nn.\napple; a fruit\n".encode("utf-8")),
    ("banana", "/bəˈnɑːnə/\nn.\nbanana\n".encode("utf-8")),
    ("beautiful", "/ˈbjuːtɪfl/\nadj.\nbeautiful; pleasing\nvery nice\n".encode("utf-8")),
    ("book", "/bʊk/\nn.\nbook; a written work\n".encode("utf-8")),
    ("cat", "/kæt/\nn.\ncat; a feline\n".encode("utf-8")),
    ("dog", "/dɒɡ/\nn.\ndog; a canine\n".encode("utf-8")),
    ("hello", "/həˈləʊ/\nint.\nhello; a greeting\n".encode("utf-8")),
    ("test", "/test/\nn.\ntest; an examination\n".encode("utf-8")),
]
keys = sorted(entries_data, key=lambda kv: kv[0].encode("utf-8"))

keys_blob = bytearray()
recs = []
defs_blob = bytearray()
for k, def_text in keys:
    kb = k.encode("utf-8")
    keys_blob += struct.pack("<H", len(kb)) + kb
    recs.append((len(defs_blob), len(def_text)))
    defs_blob += def_text
rec_blob = b"".join(struct.pack("<IH", off, ln) for off, ln in recs)

sparse = bytearray()
off = 0
for i, (k, _) in enumerate(keys):
    if i % BLOCK == 0:
        kb = k.encode("utf-8")
        sparse += struct.pack("<IH", off, len(kb)) + kb
    off += 2 + len(k.encode("utf-8"))

keys_off = HEADER_SIZE
rec_off = keys_off + len(keys_blob)
defs_off = rec_off + len(rec_blob)
sparse_off = defs_off + len(defs_blob)

header = MAGIC + struct.pack("<II", VERSION, len(keys)) + struct.pack("<I", BLOCK_LOG2)
header += struct.pack("<II", keys_off, len(keys_blob))
header += struct.pack("<II", rec_off, len(rec_blob))
header += struct.pack("<II", defs_off, len(defs_blob))
header += struct.pack("<II", sparse_off, len(sparse))
header += b"\0" * (HEADER_SIZE - len(header))
assert len(header) == HEADER_SIZE

blob = header + keys_blob + rec_blob + defs_blob + sparse
with open(sys.argv[1], "wb") as fh:
    fh.write(blob)
print(f"wrote {sys.argv[1]}: {len(blob)}B, {len(keys)} entries, {len(sparse)}B sparse, block_log2={BLOCK_LOG2}")
