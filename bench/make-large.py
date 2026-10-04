#!/usr/bin/env python3
"""Writes the large files for spike S2 into out/s2 (gitignored): the 50 KB
sample repeated to 1, 10 and 100 MB, and a 5 MB file of one line."""
from pathlib import Path

out = Path('out/s2')
out.mkdir(parents=True, exist_ok=True)
sample = Path('bench/sample-50k.md').read_text()
for mb in (1, 10, 100):
    n = mb * 1_000_000 // len(sample.encode()) + 1
    (out / f'md-{mb}m.md').write_text(sample * n)
(out / 'line-5m.txt').write_text(('lorem ipsum dolor sit amet ' * 200_000)[:5_000_000])
