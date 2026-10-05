"""Convert Prague scenes and pigeon sprites to LVGL 8.4 RGB565 assets.

This is a mechanical firmware encoding step. Requires Pillow.
Run from this directory: python convert_for_lvgl.py
"""
from pathlib import Path
from PIL import Image, ImageOps

ASSETS = Path(__file__).resolve().parent
ART = ASSETS.parent / "src" / "art"
ART.mkdir(parents=True, exist_ok=True)

items = [
    ("prague_day_clear_source.png", "prague_day", (800, 480), False, False),
    ("prague_dusk_clear_source.png", "prague_dusk", (800, 480), False, False),
    ("pigeon_stand_source.png", "pigeon_stand", (132, 116), True, True),
    ("pigeon_fly_source.png", "pigeon_fly", (156, 130), True, True),
]

header = '#pragma once\n#include "lvgl.h"\n'
for _, name, _, _, _ in items:
    header += f'extern const lv_img_dsc_t {name};\n'
(ART / "prague_images.h").write_text(header, encoding="ascii")

with (ART / "prague_images.cpp").open("w", encoding="ascii", newline="\n") as out:
    out.write('#include "prague_images.h"\n')
    for filename, name, size, alpha, contain in items:
        image = Image.open(ASSETS / filename).convert("RGBA")
        if contain:
            fitted = ImageOps.contain(image, size, method=Image.Resampling.LANCZOS)
            image = Image.new("RGBA", size, (0, 0, 0, 0))
            image.alpha_composite(fitted, ((size[0] - fitted.width) // 2,
                                           (size[1] - fitted.height) // 2))
        else:
            image = ImageOps.fit(image, size, method=Image.Resampling.LANCZOS)
        image.save(ASSETS / f"{name}_preview.png")
        pixels = list(image.getdata())
        out.write(f"static const uint8_t {name}_map[] = {{\n")
        for start in range(0, len(pixels), 12):
            row = []
            for r, g, b, a in pixels[start:start + 12]:
                rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                row.extend((rgb565 & 255, rgb565 >> 8))
                if alpha:
                    row.append(a)
            out.write("  " + ", ".join(f"0x{byte:02x}" for byte in row) + ",\n")
        out.write("};\n")
        fmt = "LV_IMG_CF_TRUE_COLOR_ALPHA" if alpha else "LV_IMG_CF_TRUE_COLOR"
        out.write(
            f"const lv_img_dsc_t {name} = {{ {{ {fmt}, 0, 0, "
            f"{size[0]}, {size[1]} }}, sizeof({name}_map), {name}_map }};\n"
        )
        print(f"{name}: {size[0]}x{size[1]}, alpha={alpha}, "
              f"source alpha range={image.getextrema()[3]}")
