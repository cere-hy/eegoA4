#!/usr/bin/env python3
# S3c: augment ui-hanzi.bin (PIF1) with the common CJK punctuation that neither
# builtin.ttf nor the shipped bin covers (book-title marks 《》, brackets 【】〈〉,
# etc.), so ordinary Chinese prose has ZERO SD fallback. Rendering uses the exact
# same stbtt 24px / 96-threshold / 77-byte record path as Pico's gen_ui_hanzi.py,
# with Noto Sans CJK SC (same family as builtin.ttf) as the punctuation source.
import struct, zlib, subprocess, hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'assets/fonts/ui-hanzi.bin'
BASE, BLOCK, RECORD = 24, 32, 77

# Common CJK punctuation absent from builtin.ttf / shipped bin but frequent in
# pushed Chinese prose. (builtin.ttf already has ，。！？：；“”（）、—…「」etc.)
PUNCT = [
    0x300A, 0x300B,  # 《 》 book title marks
    0x3008, 0x3009,  # 〈 〉 single angle
    0x3010, 0x3011,  # 【 】 lenticular
    0x3014, 0x3015,  # 〔 〕 torsion
    0x3016, 0x3017,  # 〖 〗
    0x3006, 0x3007,  # 〆 〇
    0x00B7,          # · middle dot
    0x2027,          # ‧ hyphenation point
    0x30FB,          # ・ katakana middle dot
]

def main():
    from fontTools.ttLib import TTFont
    data = BIN.read_bytes()
    magic, count, base, block = struct.unpack('<4sIII', data[:16])
    assert magic == b'PIF1' and base == BASE and block == BLOCK
    chars = list(struct.unpack('<' + 'H' * count, data[16:16 + 2 * count]))
    existing = set(chars)

    builtin = set(TTFont(ROOT / 'assets/fonts/builtin.ttf').getBestCmap())
    need = sorted(c for c in PUNCT if c not in existing and c not in builtin)
    print('punct to add:', ' '.join(f'U+{c:04X}' for c in need))
    if not need:
        print('nothing to do'); return

    # Extract Noto Sans CJK SC (TTC index 2) as a standalone TTF for stbtt.
    work = ROOT / 'build/push-punct'; work.mkdir(parents=True, exist_ok=True)
    src = work / 'sc.ttf'
    ttc = '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'
    if not src.is_file():
        f = TTFont(ttc, fontNumber=2)
        f.flavor = None
        f.save(src); f.close()

    helper = work / 'glyph.c'
    helper.write_text('''#include <stdint.h>
#include <string.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
static stbtt_fontinfo font;
int init(const unsigned char *data){return stbtt_InitFont(&font,data,stbtt_GetFontOffsetForIndex(data,0));}
int render(uint32_t cp,uint8_t *out){
 if(!stbtt_FindGlyphIndex(&font,cp))return 0;
 float s=stbtt_ScaleForPixelHeight(&font,24);int x0,y0,x1,y1,advance,left;
 stbtt_GetCodepointBitmapBox(&font,cp,s,s,&x0,&y0,&x1,&y1);
 stbtt_GetCodepointHMetrics(&font,cp,&advance,&left);
 int w=x1-x0,h=y1-y0;if(w<1||h<1||w>24||h>24)return 0;
 uint8_t pixels[576]={0};stbtt_MakeCodepointBitmap(&font,pixels,w,h,w,s,s,cp);
 memset(out,0,77);out[0]=(uint8_t)x0;out[1]=(uint8_t)y0;out[2]=w;out[3]=h;out[4]=(int)(advance*s+0.5f);
 for(int y=0;y<h;y++)for(int x=0;x<w;x++)if(pixels[y*w+x]>=96)out[5+(y*24+x)/8]|=1u<<((y*24+x)%8);
 return 1;
}
''')
    stb_inc = ROOT.parent / 'pico-plus/main/font'
    libpath = work / 'glyph.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-I' + str(stb_inc), str(helper), '-o', str(libpath)], check=True)
    import ctypes
    lib = ctypes.CDLL(str(libpath))
    buf = ctypes.create_string_buffer(src.read_bytes())
    assert lib.init(buf), 'stbtt init failed'

    # Decode ALL existing records so we can re-pack the whole bin losslessly.
    off = 16 + 2 * count
    nblocks = (count + BLOCK - 1) // BLOCK
    offsets = list(struct.unpack('<' + 'I' * (nblocks + 1), data[off:off + 4 * (nblocks + 1)]))
    blobs = data[off + 4 * (nblocks + 1):]
    raw_by_cp = {}
    rec = (ctypes.c_uint8 * RECORD)()
    for bi in range(nblocks):
        block_raw = zlib.decompress(blobs[offsets[bi]:offsets[bi + 1]])
        entries = min(BLOCK, count - bi * BLOCK)
        for j in range(entries):
            raw_by_cp[chars[bi * BLOCK + j]] = block_raw[j * RECORD:(j + 1) * RECORD]

    for cp in need:
        assert lib.render(cp, rec), f'cannot render U+{cp:04X}'
        raw_by_cp[cp] = bytes(rec)

    all_chars = sorted(raw_by_cp)
    raw = [raw_by_cp[c] for c in all_chars]
    new_offsets = [0]
    packed = b''
    for start in range(0, len(raw), BLOCK):
        packed += zlib.compress(b''.join(raw[start:start + BLOCK]), 9)
        new_offsets.append(len(packed))
    out = struct.pack('<4sIII', b'PIF1', len(all_chars), BASE, BLOCK)
    out += struct.pack('<' + 'H' * len(all_chars), *all_chars)
    out += struct.pack('<' + 'I' * len(new_offsets), *new_offsets)
    out += packed
    BIN.write_bytes(out)
    print(f'ui-hanzi.bin: {count} -> {len(all_chars)} chars, {len(data)} -> {len(out)} bytes')

if __name__ == '__main__':
    main()
