"""Turn out/*.pbm into 4x-scaled PNGs that look like the OLED, plus one contact sheet."""
import glob
import os
import sys

from PIL import Image

d = sys.argv[1] if len(sys.argv) > 1 else 'out'
frames = []
for path in sorted(glob.glob(os.path.join(d, '*.pbm'))):
    lines = open(path).read().split('\n')
    w, h = map(int, lines[1].split())
    img = Image.new('RGB', (w, h))
    for y in range(h):
        for x in range(w):
            img.putpixel((x, y), (120, 200, 255) if lines[2 + y][x] == '1' else (8, 10, 14))
    big = img.resize((w * 4, h * 4), Image.NEAREST)
    big.save(path[:-4] + '.png')
    frames.append((os.path.basename(path)[:-4], big))
if frames:
    cols = 2
    W, H = frames[0][1].size
    sheet = Image.new('RGB', (cols * (W + 16), ((len(frames) + cols - 1) // cols) * (H + 16)), (40, 40, 40))
    for i, (_, f) in enumerate(frames):
        sheet.paste(f, ((i % cols) * (W + 16) + 8, (i // cols) * (H + 16) + 8))
    sheet.save(os.path.join(d, 'sheet.png'))
print(len(frames), 'frames')
