"""Create a source or Windows release without build caches, local config or logs."""
import argparse
from datetime import datetime
import hashlib
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRECTORIES = {'apps', 'include', 'src', 'config', 'tests', 'tools', 'examples',
                      'scripts', 'third_party', 'docs', '.github'}
SOURCE_ROOT_FILES = {'README.md', 'CMakeLists.txt', 'CMakePresets.json', '.gitignore', '.gitattributes'}


def source_files():
    for file in sorted(ROOT.rglob('*')):
        if not file.is_file():
            continue
        relative = file.relative_to(ROOT)
        if relative.parts[0] not in SOURCE_DIRECTORIES and relative.as_posix() not in SOURCE_ROOT_FILES:
            continue
        if any(part in ('__pycache__', '.venv', '.git') for part in relative.parts):
            continue
        if file.suffix.lower() in ('.pyc', '.exe', '.dll', '.log', '.obj', '.o', '.a', '.lib'):
            continue
        if file.name == '.env' or (file.name.startswith('.env.') and file.name != '.env.example'):
            continue
        if relative.parts[0] == 'config' and (file.name.endswith('.local.json') or file.name == 'local.json'):
            continue
        yield file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT/'build/releases')
    parser.add_argument('--with-binaries', action='store_true')
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    files = list(source_files())
    if args.with_binaries:
        binaries = sorted((ROOT/'build/bin').glob('*.exe'))
        expected = {'gateway.exe', 'c_client.exe', 'fake_gateway.exe', 'replay_udp.exe', 'b_demo.exe', 'fixture_generator.exe'}
        if not expected.issubset({file.name for file in binaries}):
            parser.error('Missing executables in build/bin; build the project first')
        files += binaries
    kind = 'windows' if args.with_binaries else 'source'
    stamp = datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    archive = output/f'{ROOT.name}-{kind}-{stamp}.zip'
    manifest = ''.join(hashlib.sha256(file.read_bytes()).hexdigest()+'  '+file.relative_to(ROOT).as_posix()+'\n' for file in files)
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as release:
        for file in files:
            release.write(file, ROOT.name+'/'+file.relative_to(ROOT).as_posix())
        release.writestr(ROOT.name+'/MANIFEST.sha256', manifest)
    with zipfile.ZipFile(archive) as release:
        if release.testzip() is not None:
            raise RuntimeError('ZIP verification failed')
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix('.zip.sha256').write_text(checksum+'  '+archive.name+'\n', encoding='ascii')
    print(f'Package: {archive}\nFiles: {len(files)}\nSHA256: {checksum}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
