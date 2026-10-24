#!/usr/bin/env python3
"""Fingerprint the whole player, then stage coherent relative asset references."""
from pathlib import Path
import hashlib
import sys

out = Path(sys.argv[1])
source = Path(__file__).parent
names = ['index.html', 'style.css', 'app.js', 'canvas.js', 'engine.js', 'worklet.js']
contents = {name: (source / name).read_bytes() for name in names}
contents.update({name: (out / name).read_bytes() for name in ['ntrk.wasm', 'demo.json']})
if (out / 'demo.ntrk').exists():
    contents['demo.ntrk'] = (out / 'demo.ntrk').read_bytes()
build = hashlib.sha256(b''.join(name.encode() + contents[name] for name in sorted(contents))).hexdigest()[:16]
for name in names:
    text = contents[name].decode()
    for asset in names + ['ntrk.wasm']:
        text = text.replace(f"'{asset}'", f"'{asset}?v={build}'")
        text = text.replace(f"'./{asset}'", f"'./{asset}?v={build}'")
        text = text.replace(f'"{asset}"', f'"{asset}?v={build}"')
    (out / name).write_text(text)
