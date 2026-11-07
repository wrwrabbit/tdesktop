import argparse
import fnmatch
import os
from pathlib import Path


KEPT_SUFFIXES = {'.lib', '.a', '.exe', '.h', '.hpp', '.inc', '.cmake', '.pc'}
RUST_SOURCES = {'tlottie', 'wallet-engine', 'tdesktop_rust'}


def keep_file(relative):
    parts = relative.parts
    if parts[0] in RUST_SOURCES:
        if 'target' in parts[1:-1] or '.git' in parts[1:-1]:
            return False
        return True
    return (
        relative.suffix in KEPT_SUFFIXES
        or any(part in {'include', 'cache_keys', 'patches', 'nv-codec-headers'}
               or fnmatch.fnmatchcase(part, 'objects-*') for part in parts[:-1])
    )


def cleanup(libraries):
    removed = 0
    for directory, directories, files in os.walk(libraries, topdown=False):
        directory = Path(directory)
        for name in files:
            path = directory / name
            if not path.is_symlink() and not keep_file(path.relative_to(libraries)):
                path.unlink()
                removed += 1
        for name in directories:
            path = directory / name
            if not path.is_symlink() and not any(path.iterdir()):
                path.rmdir()
    print(f'Removed {removed} dependency files from {libraries}.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Clean prepared Windows libraries for CI caching.')
    parser.add_argument('libraries', type=Path)
    args = parser.parse_args()
    if not args.libraries.is_dir():
        parser.error(f'Libraries directory does not exist: {args.libraries}')
    cleanup(args.libraries.resolve())
