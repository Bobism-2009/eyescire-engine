"""Build icons/eyescire.ico from icons/eyescire.png for the Windows .exe icon.

The PNG is the same mark the editor window loads at run time. Explorer and the
taskbar read the .ico resource baked into editor.exe (and the other Eyescire
exes) at link time. Entries are PNG-compressed (Vista+), one per size.
"""
import io
import os
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PNG = os.path.join(HERE, "eyescire.png")
ICO = os.path.join(HERE, "eyescire.ico")
SIZES = [16, 32, 48, 64, 128, 256]


def main():
    src = Image.open(PNG).convert("RGBA")
    entries = []
    for s in SIZES:
        im = src.resize((s, s), Image.Resampling.LANCZOS)
        buf = io.BytesIO()
        im.save(buf, format="PNG")
        entries.append((s, buf.getvalue()))

    out = bytearray()
    out += struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    data = bytearray()
    for s, png in entries:
        w = 0 if s >= 256 else s
        h = 0 if s >= 256 else s
        out += struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(png), offset)
        data += png
        offset += len(png)
    out += data

    with open(ICO, "wb") as f:
        f.write(out)
    print("wrote", ICO, "(%d bytes, %d sizes)" % (len(out), len(SIZES)))


if __name__ == "__main__":
    main()
