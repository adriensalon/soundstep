from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent.parent
FONT_PATH = ROOT / "font" / "astral delight.ttf"
PLATFORM_DIR = ROOT / "platform"
RESAMPLE = Image.Resampling.LANCZOS


def _font_for_height(text: str, target_height: int) -> ImageFont.FreeTypeFont:
    low = 1
    high = target_height * 3
    while low < high:
        size = (low + high + 1) // 2
        font = ImageFont.truetype(str(FONT_PATH), size)
        box = font.getbbox(text)
        if box[3] - box[1] <= target_height:
            low = size
        else:
            high = size - 1
    return ImageFont.truetype(str(FONT_PATH), low)


def _draw_centered_glyph(image: Image.Image, target_height: int) -> None:
    draw = ImageDraw.Draw(image)
    font = _font_for_height("S", target_height)
    box = draw.textbbox((0, 0), "S", font=font)
    x = (image.width - (box[2] - box[0])) / 2 - box[0]
    y = (image.height - (box[3] - box[1])) / 2 - box[1]
    draw.text((x, y), "S", font=font, fill=(255, 255, 255, 255))


def _render_master(size: int = 1024) -> Image.Image:
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    inset = round(size * 0.0625)
    radius = round((size - inset * 2) * 0.20)
    draw.rounded_rectangle(
        (inset, inset, size - inset - 1, size - inset - 1),
        radius=radius,
        fill=(0, 0, 0, 255),
    )
    _draw_centered_glyph(image, round((size - inset * 2) * 0.56))
    return image


def _render_adaptive_foreground(size: int) -> Image.Image:
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    _draw_centered_glyph(image, round(size * 0.52))
    return image


def _save_png(image: Image.Image, path: Path, size: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.resize((size, size), RESAMPLE).save(path, optimize=True)


def main() -> None:
    if not FONT_PATH.is_file():
        raise FileNotFoundError(f"Astral Delight font not found: {FONT_PATH}")

    master = _render_master()
    common_path = PLATFORM_DIR / "common" / "soundstep.png"
    common_path.parent.mkdir(parents=True, exist_ok=True)
    master.save(common_path, optimize=True)

    windows_icon = PLATFORM_DIR / "win32" / "soundstep.ico"
    windows_icon.parent.mkdir(parents=True, exist_ok=True)
    master.resize((256, 256), RESAMPLE).save(
        windows_icon,
        format="ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
               (64, 64), (128, 128), (256, 256)],
    )

    linux_sizes = (16, 24, 32, 48, 64, 128, 256, 512)
    for size in linux_sizes:
        _save_png(
            master,
            PLATFORM_DIR / "linux" / "icons" / "hicolor"
            / f"{size}x{size}" / "apps" / "soundstep.png",
            size,
        )

    android_res = PLATFORM_DIR / "android" / "src" / "main" / "res"
    android_sizes = {
        "mdpi": 48,
        "hdpi": 72,
        "xhdpi": 96,
        "xxhdpi": 144,
        "xxxhdpi": 192,
    }
    adaptive_sizes = {
        "mdpi": 108,
        "hdpi": 162,
        "xhdpi": 216,
        "xxhdpi": 324,
        "xxxhdpi": 432,
    }
    for density, size in android_sizes.items():
        directory = android_res / f"mipmap-{density}"
        _save_png(master, directory / "ic_launcher.png", size)
        _save_png(master, directory / "ic_launcher_round.png", size)
    for density, size in adaptive_sizes.items():
        foreground = _render_adaptive_foreground(size)
        _save_png(
            foreground,
            android_res / f"mipmap-{density}" / "ic_launcher_foreground.png",
            size,
        )


if __name__ == "__main__":
    main()
