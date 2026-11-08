# Writes files/postfx/hdr_lens_dirt.png: a 1024 x 512 grey picture of lens grime: broad smudges and wipe streaks, a few
# fingerprint-like rings, dust, fine scratches. hdr_lens_dirt.frag reads it (white is dirty glass). Edit it or replace it with
# any grey PNG and run again. Needs Pillow and numpy.
import math
import random
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

random.seed(11)
rng = np.random.default_rng(11)
W, H = 1024, 512


def fractal(w, h, octaves, seed):
    r = np.random.default_rng(seed)
    out = np.zeros((h, w), np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        n = 2 ** (o + 2)
        small = r.random((max(2, n * h // w), n)).astype(np.float32)
        img = Image.fromarray((small * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)
        out += np.asarray(img, np.float32) / 255.0 * amp
        total += amp
        amp *= 0.55
    return out / total


# broad grime: thresholded fractal noise, so it comes in cloudy patches with soft edges
grime = fractal(W, H, 5, 3)
grime = np.clip((grime - 0.52) * 3.2, 0, 1) ** 1.4 * 0.55

# wipe streaks: noise stretched along one direction
streak = fractal(W // 8, H, 4, 9)
streak = np.asarray(Image.fromarray((streak * 255).astype(np.uint8)).resize((W, H), Image.BICUBIC), np.float32) / 255.0
streak = np.clip((streak - 0.55) * 3.0, 0, 1) * 0.35

img = Image.fromarray((np.clip(grime + streak, 0, 1) * 255).astype(np.uint8))
d = ImageDraw.Draw(img)
for _ in range(7):   # fingerprint like rings
    x, y = random.randint(80, W - 80), random.randint(60, H - 60)
    for k in range(4, 26, 3):
        d.ellipse((x - k * 1.6, y - k, x + k * 1.6, y + k), outline=random.randint(40, 90), width=1)
img = img.filter(ImageFilter.GaussianBlur(2.2))
d = ImageDraw.Draw(img)
for _ in range(190):  # dust: tiny, irregular, mostly dim
    x, y = random.randint(0, W), random.randint(0, H)
    r = random.choice([1, 1, 1, 1, 2, 2])
    d.ellipse((x - r, y - r, x + r, y + r), fill=random.randint(50, 150))
for _ in range(9):    # fine scratches
    x, y = random.randint(0, W), random.randint(0, H)
    a, l = random.uniform(-0.5, 0.5) + random.choice([0, math.pi / 2]), random.randint(40, 200)
    d.line((x, y, x + math.cos(a) * l, y + math.sin(a) * l), fill=random.randint(70, 150), width=1)
img = img.filter(ImageFilter.GaussianBlur(0.8))
img.convert('RGB').save('files/postfx/hdr_lens_dirt.png')
print('written')
