"""Check static Chinese UI literals against the bundled font character set."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
source = "\n".join(
    (ROOT / path).read_text(encoding="utf-8")
    for path in ("main/app_ui.c", "main/app_preview.c")
)
charset = set((ROOT / "tools/font/chars.txt").read_text(encoding="utf-8"))

# Keep string literals while skipping both C comment forms.
tokens = re.findall(r'"(?:\\.|[^"\\\n])*"|/\*.*?\*/|//[^\n]*', source, re.S)
strings = (token for token in tokens if token.startswith('"'))
used = {char for literal in strings for char in literal if ord(char) > 127}
missing = sorted(used - charset)
assert not missing, f"UI glyphs absent from font charset: {[hex(ord(c)) for c in missing]}"
print(f"Static UI font coverage: PASS ({len(used)} non-ASCII characters)")
