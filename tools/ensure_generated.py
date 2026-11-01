"""Bootstrap generated sources and the cached MSVC object for build scripts."""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def ensure_generated(root=ROOT, sources_only=False):
    root = Path(root).resolve()
    output = root / 'out'
    output.mkdir(parents=True, exist_ok=True)
    sources = [output / 'gen.cpp', output / 'gen.h']

    def complete():
        return all(path.is_file() and path.stat().st_size > 0 for path in sources)

    generated = not complete()
    if generated:
        print('Generated sources missing: running tools/recomp.py', flush=True)
        subprocess.run([sys.executable, str(root / 'tools/recomp.py')], cwd=root, check=True)
        if not complete():
            raise RuntimeError('tools/recomp.py did not produce out/gen.cpp and out/gen.h')
    if sources_only:
        return

    obj = root / 'gen.obj'
    dependencies = sources + [root / 'runtime/nes.h']
    newest = max(path.stat().st_mtime_ns for path in dependencies if path.is_file())
    if generated or not obj.is_file() or obj.stat().st_mtime_ns < newest:
        print('Compiling generated code into gen.obj', flush=True)
        subprocess.run([
            'cl', '/nologo', '/O2', '/utf-8', '/std:c++17', '/EHsc', '/W3',
            '/bigobj', '/D_CRT_SECURE_NO_WARNINGS', '/I', str(root / 'runtime'),
            '/c', str(sources[0]), '/Fo:' + str(obj),
        ], cwd=root, check=True)
    else:
        print('Generated sources and gen.obj are ready', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources-only', action='store_true')
    args = parser.parse_args()
    ensure_generated(sources_only=args.sources_only)
