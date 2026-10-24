#!/usr/bin/env python3
"""Stable ZIP metadata/order; only the static player's runtime and notices."""
from pathlib import Path
import sys
import zipfile

root, target = map(Path, sys.argv[1:])
files = ['index.html', 'style.css', 'app.js', 'canvas.js', 'engine.js',
         'worklet.js', 'ntrk.wasm', 'demo.json']
if (root / 'demo.ntrk').exists():
    files.append('demo.ntrk')
with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    contents = {name: (root / name).read_bytes() for name in files}
    contents['HOSTING.txt'] = (Path(__file__).parent / 'HOSTING.txt').read_bytes()
    contents['LICENSE.txt'] = b'Browser host, ntrk library and generated example: public domain / CC0.\nhttps://creativecommons.org/publicdomain/zero/1.0/\nAn optional host-supplied example retains its own rights; obtain permission before distributing it.\n'
    for name, data in sorted(contents.items()):
        info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        archive.writestr(info, data)
print(f'{target}: {target.stat().st_size} bytes')
