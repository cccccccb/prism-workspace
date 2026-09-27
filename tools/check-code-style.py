#!/usr/bin/env python3
"""Check owned C/C++ formatting, goto tokens and the production 800-line limit."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
OWNED_ROOTS = {"prism", "prism-desktop", "prism-topbar", "prism-dock", "demos", "tests"}
SUFFIXES = {".cpp", ".hpp", ".c", ".h"}
MAX_PRODUCTION_LINES = 800
# Match raw literals before ordinary strings so their delimiters cannot expose
# quoted shader text or comments as C++ tokens. Blank matched text preserves lines.
NON_CODE = re.compile(
    r'R"(?P<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delimiter)"'
    r'|//[^\n]*|/\*[\s\S]*?\*/|"(?:\\[\s\S]|[^"\\])*"'
    r"|(?<![\w])(?:u8|u|U|L)?'(?:\\[^\n]|[^'\\\n])*'"
)


def source_files():
    result = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT, check=True, capture_output=True,
    )
    paths = set()
    for name in result.stdout.decode().split("\0"):
        path = Path(name)
        if path.parts and path.parts[0] in OWNED_ROOTS and path.suffix in SUFFIXES:
            if (ROOT / path).is_file():
                paths.add(path)

    return sorted(paths)


def blank_non_code(match):
    return "".join("\n" if char == "\n" else " " for char in match.group())


def structural_errors(paths):
    errors = []
    maximum = (0, None)
    production_count = 0
    test_count = 0
    for path in paths:
        text = (ROOT / path).read_text()
        lines = len(text.splitlines())
        if path.parts[0] != "tests":
            production_count += 1
            if lines > maximum[0]:
                maximum = (lines, path)
            if lines > MAX_PRODUCTION_LINES:
                errors.append(f"{path}: {lines} lines; production limit is {MAX_PRODUCTION_LINES}")
        else:
            test_count += 1

        code = NON_CODE.sub(blank_non_code, text)
        for match in re.finditer(r"\bgoto\b", code):
            line = code.count("\n", 0, match.start()) + 1
            errors.append(f"{path}:{line}: goto is prohibited")

    print(f"Sources: {production_count} production, {test_count} tests")
    print(f"Largest production file: {maximum[1]} ({maximum[0]} lines)")

    return errors


def formatter_path(requested):
    formatter = requested or shutil.which("clang-format-19") or shutil.which("clang-format")
    if not formatter:
        raise RuntimeError("Install clang-format-19 or pass --clang-format PATH")

    version = subprocess.run([formatter, "--version"], check=True, capture_output=True, text=True)
    if not re.search(r"(?:clang-format version|version) 19\.", version.stdout):
        raise RuntimeError(f"Expected clang-format 19, found: {version.stdout.strip()}")

    return formatter


def format_sources(formatter, paths, fix):
    failed = False
    flags = ["-i"] if fix else ["--dry-run", "--Werror"]
    for offset in range(0, len(paths), 40):
        batch = [str(path) for path in paths[offset:offset + 40]]
        result = subprocess.run([formatter, "--style=file", *flags, *batch], cwd=ROOT)
        failed = failed or result.returncode != 0

    return not failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--format", action="store_true", help="Apply formatting before checking")
    parser.add_argument("--skip-format", action="store_true", help="Only check goto and line limits")
    parser.add_argument("--clang-format", help="Path to clang-format 19")
    args = parser.parse_args()
    if args.format and args.skip_format:
        parser.error("--format and --skip-format cannot be combined")

    paths = source_files()
    format_ok = True
    if not args.skip_format:
        format_ok = format_sources(formatter_path(args.clang_format), paths, args.format)

    errors = structural_errors(paths)
    for error in errors:
        print(error, file=sys.stderr)
    if errors or not format_ok:
        return 1

    print("Code style checks passed.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
