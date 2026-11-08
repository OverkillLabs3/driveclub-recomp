# Writes files/postfx/lut_grade.png: a 32x32x32 colour lookup table laid out as a strip of 32 squares (1024 x 32), the
# layout lut_grade.frag reads. Edit grade() and run: python make_lut.py (needs Pillow). Any 8 bit PNG strip of this shape
# works, for example one made in a photo editor with its LUT export.
from PIL import Image
N = 32
def smooth(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)
def grade(r, g, b):
    # filmic contrast: an S curve
    def curve(x): return x * x * (3 - 2 * x) * 0.55 + x * 0.45
    r, g, b = curve(r), curve(g), curve(b)
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    # teal in the shadows, orange in the highlights
    w = smooth(0.15, 0.85, luma)
    r *= 0.90 + 0.22 * w; g *= 1.0 + 0.0 * w; b *= 1.08 - 0.28 * w
    # lifted blacks and a touch less saturation
    r, g, b = [0.02 + 0.98 * v for v in (r, g, b)]
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    r, g, b = [luma + (v - luma) * 0.92 for v in (r, g, b)]
    return [min(max(v, 0.0), 1.0) for v in (r, g, b)]
img = Image.new('RGB', (N * N, N))
for bi in range(N):
    for gi in range(N):
        for ri in range(N):
            r, g, b = grade(ri / (N - 1), gi / (N - 1), bi / (N - 1))
            img.putpixel((bi * N + ri, gi), (round(r * 255), round(g * 255), round(b * 255)))
img.save('files/postfx/lut_grade.png')
print('written')
