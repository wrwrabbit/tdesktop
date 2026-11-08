#!/usr/bin/env python3
"""Run a build unchanged; add structurally redacted CMake syntax diagnostics.

Only the additional file excerpts are redacted. The child's existing output is
passed through unchanged. No raw log, environment, cache or compiler-file
artifact is saved. Paths outside generated compiler files are never opened.

Example (from Telegram/build):
  python3 cmake_failure_diagnostics.py --root ../.. -- python3 release_build.py mac
"""
import argparse
import pathlib
import re
import subprocess
import sys

LOCATION = re.compile(r'CMake (?:Error|Warning)(?: \([^\r\n)]*\))? at (.+\.cmake):(\d+)(?::|\s)')
COLUMN = re.compile(r'Syntax (?:Warning|Error) in cmake code at column (\d+)')
COMPILER_FILE = re.compile(
    r'out/CMakeFiles/\d+(?:\.\d+)*(?:-[A-Za-z0-9.]+)?/'
    r'CMake(?:C|CXX|OBJC|OBJCXX|Swift|ASM)Compiler\.cmake')
SYNTAX_BYTES = frozenset(b' \t"\'()[]{};\\#$=')
MAX_FILE_BYTES = 2 * 1024 * 1024
MAX_EXCERPT_BYTES = 240


class SyntaxErrors:
    def __init__(self):
        self.pending = None
        self.remaining = 0
        self.errors = []

    def consume(self, text):
        # A location and its syntax column occur on separate lines in CMake output.
        location = LOCATION.search(text)
        if location:
            self.pending = (location.group(1), int(location.group(2)))
            self.remaining = 12
        elif self.pending and self.remaining:
            self.remaining -= 1
            column = COLUMN.search(text)
            if column:
                error = (*self.pending, int(column.group(1)))
                if error not in self.errors:
                    self.errors.append(error)
                self.pending = None


def compiler_path(root, reported):
    path = pathlib.Path(reported)
    if not path.is_absolute():
        path = root / path
    # resolve() rejects escaping symlinks as well as ../ traversal below.
    path = path.resolve(strict=True)
    relative = path.relative_to(root)
    if not COMPILER_FILE.fullmatch(relative.as_posix()):
        raise ValueError('Not an allowlisted generated compiler file')
    if path.stat().st_size > MAX_FILE_BYTES:
        raise ValueError('Compiler file exceeds diagnostic size limit')
    return path, relative


def report(errors, root, stream=None):
    stream = stream or sys.stdout
    for reported, line_number, column in errors:
        print('\n[CMake syntax diagnostic: values redacted]', file=stream)
        try:
            path, relative = compiler_path(root, reported)
            lines = path.read_bytes().splitlines()
            if not 1 <= line_number <= len(lines) or column < 1:
                raise ValueError('Invalid source position')
        except (OSError, ValueError, RuntimeError):
            # Never echo a rejected path or exception: either can contain values.
            print('Source unavailable or outside the diagnostic allowlist.', file=stream)
            continue
        print(f'{relative.as_posix()}:{line_number}, column {column}', file=stream)
        print('Only CMake punctuation/whitespace is shown; every other byte is x.', file=stream)
        for number in range(max(1, line_number - 2), min(len(lines), line_number + 2) + 1):
            raw = lines[number - 1]
            start = max(0, column - 1 - MAX_EXCERPT_BYTES // 2)
            start = min(start, max(0, len(raw) - MAX_EXCERPT_BYTES))
            excerpt = raw[start:start + MAX_EXCERPT_BYTES]
            masked = bytes(value if value in SYNTAX_BYTES else ord('x') for value in excerpt).decode('ascii')
            marker = '>' if number == line_number else ' '
            print(f'{marker} {number} [bytes {start + 1}-{start + len(excerpt)}]: {masked}', file=stream)
        print('[End redacted CMake diagnostic]', file=stream)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=pathlib.Path, required=True, help='Repository root')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command
    if command[:1] == ['--']:
        command = command[1:]
    if not command:
        parser.error('Specify a build command after --')
    root = args.root.resolve(strict=True)
    errors = SyntaxErrors()
    try:
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, encoding='utf-8', errors='replace', bufsize=1) as process:
            for text in process.stdout:
                sys.stdout.write(text)
                sys.stdout.flush()
                errors.consume(text)
            result = process.wait()
    except OSError:
        print('Could not start the requested build command.', file=sys.stderr)
        return 127
    report(errors.errors, root)
    # release_build.py currently exits with os.system's wait status: 256 can
    # become exit code 0. A detected CMake syntax error must still fail this job.
    if errors.errors and result == 0:
        return 1
    return result if result >= 0 else 128 - result


if __name__ == '__main__':
    sys.exit(main())
