#!/usr/bin/env python3
"""Stage an optional host-owned demo; never commit a duplicate song in ntrk."""
import hashlib
import json
from pathlib import Path
import shutil
import sys

out, source, name = sys.argv[1:]
out = Path(out)
out.mkdir(parents=True, exist_ok=True)
config = None
if source:
    source = Path(source)
    shutil.copyfile(source, out / 'demo.ntrk')
    config = {'url': 'demo.ntrk', 'name': name or source.stem,
              'sha256': hashlib.sha256((out / 'demo.ntrk').read_bytes()).hexdigest()}
else:
    (out / 'demo.ntrk').unlink(missing_ok=True)
(out / 'demo.json').write_text(json.dumps(config, ensure_ascii=False) + '\n', encoding='utf-8')
