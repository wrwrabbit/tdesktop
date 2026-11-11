import argparse
import fnmatch
import os
import stat
from pathlib import Path


KEPT_SUFFIXES = {'.lib', '.a', '.exe', '.h', '.hpp', '.inc', '.cmake', '.pc', '.patch', '.diff'}
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
    skipped = 0

    def warn(error):
        nonlocal skipped
        skipped += 1
        print(f'Warning: cleanup skipped an operation: {error}')

    for directory, directories, files in os.walk(libraries, topdown=False, onerror=warn):
        directory = Path(directory)
        for name in files:
            path = directory / name
            try:
                if path.is_symlink() or keep_file(path.relative_to(libraries)):
                    continue
                try:
                    path.unlink()
                except PermissionError:
                    mode = path.stat().st_mode
                    if mode & stat.S_IWRITE:
                        raise
                    path.chmod(mode | stat.S_IWRITE)
                    path.unlink()
                removed += 1
            except OSError as error:
                warn(error)
        for name in directories:
            path = directory / name
            try:
                if not path.is_symlink() and not any(path.iterdir()):
                    path.rmdir()
            except OSError as error:
                warn(error)
    print(f'Removed {removed} dependency files from {libraries}; skipped {skipped} operations.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Clean prepared Windows libraries for CI caching.')
    parser.add_argument('libraries', type=Path)
    args = parser.parse_args()
    if not args.libraries.is_dir():
        parser.error(f'Libraries directory does not exist: {args.libraries}')
    cleanup(args.libraries.resolve())
