"""Verify the SHA256 manifest supplied with the release (no third-party packages)."""
import hashlib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
if not (root/'MANIFEST.sha256').is_file():
    raise SystemExit('No release manifest here. Run this tool inside an unpacked ZIP made by scripts/package.ps1.')
failures = []
count = 0
for line in (root/'MANIFEST.sha256').read_text(encoding='utf-8').splitlines():
    expected, name = line.split('  ', 1)
    path = (root/name).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        failures.append(name + ': missing or outside project')
    elif hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        failures.append(name + ': SHA256 mismatch')
    count += 1
for failure in failures:
    print('FAIL', failure)
print(f'{count-len(failures)}/{count} release files verified')
raise SystemExit(1 if failures else 0)
