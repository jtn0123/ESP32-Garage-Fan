#!/usr/bin/env python3
"""Check for banned C functions in firmware code."""

from pathlib import Path
import re
import sys

BANNED_FUNCTIONS = {
    "strcpy": "Use safe_strcpy() instead",
    "strcat": "Use safe_strcat() or snprintf() instead",
    "sprintf": "Use snprintf() or safe_snprintf() instead",
    "gets": "Use fgets() instead",
    "strncpy": "Use safe_strcpy() for null-termination guarantee",
}

# Files/patterns to skip
SKIP_PATTERNS = [
    "*.backup",
    "test_*.cpp",
    ".pio/*",
    "safe_strings.h",  # Implements the safe alternatives using raw functions
]


def check_file(filepath: Path) -> list[tuple[int, str, str]]:
    """Check a file for banned functions. Returns list of (line_num, function, suggestion)."""
    issues: list[tuple[int, str, str]] = []

    try:
        content = filepath.read_text()
    except Exception:
        return issues

    for line_num, line in enumerate(content.splitlines(), 1):
        # Skip comments
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("/*"):
            continue

        for func, suggestion in BANNED_FUNCTIONS.items():
            # Match function call (word boundary, followed by parenthesis)
            pattern = rf"\b{func}\s*\("
            if re.search(pattern, line):
                # Skip if it's the safe_ version
                if f"safe_{func}" in line:
                    continue
                issues.append((line_num, func, suggestion))

    return issues


def main() -> None:
    firmware_dir = Path("firmware/arduino/src")

    if not firmware_dir.exists():
        print(f"Directory not found: {firmware_dir}")
        sys.exit(1)

    all_issues = []

    for cpp_file in firmware_dir.rglob("*.cpp"):
        # Skip test files and backups
        if any(cpp_file.match(pattern) for pattern in SKIP_PATTERNS):
            continue

        issues = check_file(cpp_file)
        for line_num, func, suggestion in issues:
            all_issues.append(f"{cpp_file}:{line_num}: {func}() - {suggestion}")

    for h_file in firmware_dir.rglob("*.h"):
        if any(h_file.match(pattern) for pattern in SKIP_PATTERNS):
            continue

        issues = check_file(h_file)
        for line_num, func, suggestion in issues:
            all_issues.append(f"{h_file}:{line_num}: {func}() - {suggestion}")

    if all_issues:
        print("❌ Banned function usage detected:\n")
        for issue in all_issues:
            print(f"  {issue}")
        print(f"\nTotal: {len(all_issues)} issues")
        sys.exit(1)
    else:
        print("✅ No banned functions found")
        sys.exit(0)


if __name__ == "__main__":
    main()
