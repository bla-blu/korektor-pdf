# Tworzy assets/korektor.icns (ikona aplikacji macOS) z logo PNG:
#   python3 tools/make_icns.py assets/icons/logo.png assets/korektor.icns   (wymaga Pillow)
import io, struct, sys
from PIL import Image
src = Image.open(sys.argv[1]).convert('RGBA')
# typ elementu -> rozmiar w pikselach (PNG w środku; obsługiwane od macOS 10.7)
entries = [('icp4', 16), ('icp5', 32), ('icp6', 64), ('ic07', 128), ('ic08', 256),
           ('ic11', 32), ('ic12', 64), ('ic13', 256)]
chunks = b''
for t, s in entries:
    im = src if s == src.width else src.resize((s, s), Image.LANCZOS)
    b = io.BytesIO(); im.save(b, 'PNG', optimize=True); data = b.getvalue()
    chunks += t.encode() + struct.pack('>I', len(data) + 8) + data
open(sys.argv[2], 'wb').write(b'icns' + struct.pack('>I', len(chunks) + 8) + chunks)
