"""Installs the launcher icon from your own copy of the game.

Usage: make_icon.py PATH/TO/TESV.exe   (or a .ico file)

Skyrim ships no separate .ico: the icon is a resource inside the exe.
This reads the exe's largest icon (RT_GROUP_ICON / RT_ICON) and writes
android/app/src/main/res/mipmap-*/ic_launcher.png. The repository only
carries a placeholder icon, because the real one belongs to the game: do not
commit or distribute the result. Needs Pillow and pefile.
"""
import io
import os
import struct
import sys

import pefile
from PIL import Image

RES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "android", "app", "src", "main", "res")
SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
RT_ICON, RT_GROUP_ICON = 3, 14


def ico_from_exe(path):
    """Rebuilds the exe's first icon group as .ico file bytes."""
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    icons, group = {}, None
    for typ in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        for name in typ.directory.entries:
            lang = name.directory.entries[0]
            data = pe.get_data(lang.data.struct.OffsetToData, lang.data.struct.Size)
            if typ.id == RT_ICON:
                icons[name.id] = data
            elif typ.id == RT_GROUP_ICON and group is None:
                group = data
    if group is None:
        sys.exit("no icon in " + path)
    count = struct.unpack_from("<H", group, 4)[0]
    entries = [struct.unpack_from("<BBBBHHIH", group, 6 + 14 * i) for i in range(count)]
    out = bytearray(struct.pack("<HHH", 0, 1, count))
    offset = 6 + 16 * count
    images = []
    for w, h, colors, res, planes, bits, size, icon_id in entries:
        img = icons[icon_id]
        out += struct.pack("<BBBBHHII", w, h, colors, res, planes, bits, len(img), offset)
        offset += len(img)
        images.append(img)
    for img in images:
        out += img
    return bytes(out)


src = sys.argv[1]
data = ico_from_exe(src) if src.lower().endswith(".exe") else open(src, "rb").read()
im = Image.open(io.BytesIO(data))
im.size = max(im.info.get("sizes", {im.size}), key=lambda s: s[0] * s[1])
im = im.convert("RGBA")
print("icon", im.size)
for dpi, px in SIZES.items():
    out = os.path.join(RES, "mipmap-" + dpi, "ic_launcher.png")
    im.resize((px, px), Image.LANCZOS).save(out)
    print("wrote", out)
