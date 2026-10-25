#!/usr/bin/env python3
"""Restore pinned patched Scudo without a separate LLVM checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


def main():
    third_party = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=third_party.parent / 'work/scudo-source')
    output = parser.parse_args().output.absolute()
    if output.exists() or output.is_symlink():
        parser.error(f'Output already exists; choose an unused directory: {output}')
    manifest = json.loads((third_party / 'scudo-source.json').read_text())
    for name, expected in manifest['files'].items():
        if hashlib.sha256((third_party / name).read_bytes()).hexdigest() != expected:
            raise SystemExit(f'SHA-256 mismatch: {name}')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.prepare-scudo-', dir=output.parent) as temp:
        stage = Path(temp) / 'source'
        stage.mkdir()
        # Trusted checksum-verified git archive; preserve captured symlinks.
        with tarfile.open(third_party / 'scudo-llvm-87f0227cb601.tar.xz') as archive:
            for entry in archive.getmembers():
                if Path(entry.name).is_absolute() or '..' in Path(entry.name).parts:
                    raise SystemExit(f'Unsafe archive path: {entry.name}')
            archive.extractall(stage, filter='fully_trusted')
        def git(*args):
            return subprocess.check_output(['git', '-C', str(stage), *args], text=True).strip()
        git('init', '-q')
        git('-c', 'core.autocrlf=false', 'add', '--all')
        git('apply', '--index', str(third_party / 'patches/scudo-llvm-87f0227cb601-delegation.patch'))
        tree = git('write-tree')
        actual = git('rev-parse', tree + ':' + manifest['source_subdirectory'])
        if actual != manifest['patched_tree']:
            raise SystemExit(f'Restored Scudo tree mismatch: {actual}')
        shutil.rmtree(stage / '.git')
        os.rename(stage, output)
    print(output / manifest['source_subdirectory'])


if __name__ == '__main__':
    main()
