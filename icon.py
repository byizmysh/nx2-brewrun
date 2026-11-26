import sys

from PIL import Image


with Image.open(sys.argv[1]) as master:
    if master.width != master.height or master.width < 256:
        sys.exit("The launcher icon source must be square and at least 256 pixels wide.")
    rgb = master.convert("RGB")
    icon = rgb.resize((256, 256), Image.Resampling.LANCZOS)
    icon.save(sys.argv[2], quality=95, subsampling=0, optimize=True)
    for size, output in zip((70, 47), sys.argv[3:]):
        rgb.resize((size, size), Image.Resampling.LANCZOS).save(output, optimize=True)
