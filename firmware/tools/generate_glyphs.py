"""Generate the alarm UI's bitmap subset from an OFL Noto Sans TC font.
Usage: python generate_glyphs.py path/to/NotoSansTC.ttf
Font source: https://github.com/google/fonts/tree/main/ofl/notosanstc
"""
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont
root=Path(__file__).resolve().parents[1]
chars=sorted({c for c in (root/'src/main.cpp').read_text() if ord(c)>127})
font=ImageFont.truetype(sys.argv[1],12)
font.set_variation_by_axes([500])
rows=['// Alarm UI bitmap subset, derived from Noto Sans TC. See ../fonts/OFL.txt.', '#pragma once', '#include <stdint.h>', 'struct ZhGlyph { uint32_t code; uint16_t rows[12]; };', 'static const ZhGlyph ZH_GLYPHS[] = {']
for c in chars:
    image=Image.new('L',(12,12)); ImageDraw.Draw(image).text((0,0),c,font=font,fill=255,anchor='lt')
    bits=[sum((1<<(11-x)) for x in range(12) if image.getpixel((x,y))>=80) for y in range(12)]
    rows.append('{%d,{%s}},'%(ord(c),','.join(hex(x) for x in bits)))
rows.append('};')
(root/'src/zh_glyphs.h').write_text('\n'.join(rows)+'\n')
print(f'Generated {len(chars)} glyphs')
