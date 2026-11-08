# Writes files/postfx/hdr_lens_dirt.png: a 1024 x 512 grey picture of smudges and specks, the layout hdr_lens_dirt.frag reads.
# Edit or replace it with any grey PNG (white is dirty glass) and run again. Needs Pillow.
import random, math
from PIL import Image, ImageDraw, ImageFilter
random.seed(7)
W, H = 1024, 512
img = Image.new('L', (W, H), 0)
d = ImageDraw.Draw(img)
for _ in range(70):   # smudges
    x, y, r = random.randint(0, W), random.randint(0, H), random.randint(25, 110)
    d.ellipse((x - r, y - r * 0.7, x + r, y + r * 0.7), fill=random.randint(25, 90))
img = img.filter(ImageFilter.GaussianBlur(14))
d = ImageDraw.Draw(img)
for _ in range(260):  # specks
    x, y, r = random.randint(0, W), random.randint(0, H), random.randint(1, 5)
    d.ellipse((x - r, y - r, x + r, y + r), fill=random.randint(120, 255))
for _ in range(40):   # a few hairs
    x, y = random.randint(0, W), random.randint(0, H)
    a, l = random.random() * 6.28, random.randint(30, 120)
    d.line((x, y, x + math.cos(a) * l, y + math.sin(a) * l * 0.5), fill=140, width=1)
img = img.filter(ImageFilter.GaussianBlur(1.2))
img.convert('RGB').save('files/postfx/hdr_lens_dirt.png')
print('written')
