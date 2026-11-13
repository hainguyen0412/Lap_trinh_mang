"""Give each CTest network invocation its own evidence directory, including reruns."""
from pathlib import Path
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[1]
name, script, *arguments = sys.argv[1:]
output = root / 'output/test_results' / f'{name}_{time.time_ns()}'
result = subprocess.call([sys.executable, '-B', script, '--output', str(output), *arguments], cwd=root)
print(f'Evidence: {output}', flush=True)
raise SystemExit(result)
