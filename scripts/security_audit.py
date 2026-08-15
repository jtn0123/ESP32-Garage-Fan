#!/usr/bin/env python3
"""
Security audit script to check for credential leaks and security issues.
Run this before committing or as a pre-commit hook.
"""

from pathlib import Path
import re
import subprocess
import sys

# Patterns that might indicate credentials
CREDENTIAL_PATTERNS = [
    # Common password patterns
    r'password\s*[=:]\s*["\'](?!your_|placeholder|example|changeme|<.*>|\$\{)',
    r'passwd\s*[=:]\s*["\'](?!your_|placeholder|example|changeme|<.*>|\$\{)',
    r'pwd\s*[=:]\s*["\'](?!your_|placeholder|example|changeme|<.*>|\$\{)',
    # API keys and tokens
    r'api[_-]?key\s*[=:]\s*["\'][a-zA-Z0-9]{20,}',
    r'token\s*[=:]\s*["\'][a-zA-Z0-9]{20,}',
    r'secret\s*[=:]\s*["\'][a-zA-Z0-9]{8,}',
    # WiFi/Network credentials
    r'ssid\s*[=:]\s*["\'](?!YOUR_|Example|TestNetwork|<.*>|\$\{)',
    r'wifi_pass\s*[=:]\s*["\'](?!YOUR_|password|<.*>|\$\{)',
    # MQTT credentials
    r'mqtt[_-]?user\s*[=:]\s*["\'](?!your_|mqtt_user|<.*>|\$\{)',
    r'mqtt[_-]?pass\s*[=:]\s*["\'](?!your_|mqtt_pass|<.*>|\$\{)',
    # IP addresses (potential internal network exposure)
    r"\b(?:10|192\.168|172\.(?:1[6-9]|2[0-9]|3[01]))\.\d{1,3}\.\d{1,3}\b",
]

# Files that are allowed to have example credentials
ALLOWED_FILES = [
    ".env",  # This SHOULD have real credentials
    ".env.example",
    "device.sample.yaml",
    "secrets.example.yaml",
    "README.md",
    "security_audit.py",
    "generated_config.h",  # Generated from .env, expected to have them
]

# Patterns in paths that indicate test/example files
ALLOWED_PATH_PATTERNS = [
    "test_",
    "tests/",
    "examples/",
    "fixtures/",
    ".example",
    ".sample",
]

# Directories to skip
SKIP_DIRS = [
    ".git",
    ".pio",
    "__pycache__",
    "node_modules",
    ".mypy_cache",
    ".pytest_cache",
    "build",
    "dist",
]


def check_file_for_credentials(filepath: Path) -> list[dict[str, str | int]]:
    """Check a single file for potential credential leaks."""
    issues: list[dict[str, str | int]] = []

    # Skip allowed files
    if filepath.name in ALLOWED_FILES:
        return issues

    # Skip test/example files based on path patterns
    filepath_str = str(filepath)
    if any(pattern in filepath_str for pattern in ALLOWED_PATH_PATTERNS):
        return issues

    # Skip binary files
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()
    except OSError:
        return issues

    # Check each pattern
    for pattern in CREDENTIAL_PATTERNS:
        matches = re.finditer(pattern, content, re.IGNORECASE | re.MULTILINE)
        for match in matches:
            line_num = content[: match.start()].count("\n") + 1
            issues.append(
                {
                    "file": str(filepath),
                    "line": line_num,
                    "pattern": pattern[:30] + "..." if len(pattern) > 30 else pattern,
                    "match": (
                        match.group()[:50] + "..." if len(match.group()) > 50 else match.group()
                    ),
                }
            )

    return issues


def check_git_history() -> list[str]:
    """Check if sensitive files were ever committed to git."""
    sensitive_files = [
        "config/device.yaml",
        ".env",
        "secrets.yaml",
        "credentials.json",
    ]

    issues: list[str] = []
    for file in sensitive_files:
        try:
            # Check if file exists in git history
            result = subprocess.run(
                ["git", "log", "--oneline", "--follow", "--", file],
                capture_output=True,
                text=True,
                check=False,
            )
            if result.stdout.strip():
                issues.append(f"WARNING: {file} exists in git history!")
        except Exception:
            pass

    return issues


def check_gitignore() -> list[str]:
    """Verify that sensitive files are properly ignored."""
    should_be_ignored = [
        "config/device.yaml",
        ".env",
        "*.env.local",
        "secrets.yaml",
        "credentials.json",
        "generated_config.h",
    ]

    issues: list[str] = []

    # Read .gitignore
    gitignore_path = Path(".gitignore")
    if not gitignore_path.exists():
        issues.append("ERROR: No .gitignore file found!")
        return issues

    with open(gitignore_path, "r") as f:
        gitignore_content = f.read()

    for pattern in should_be_ignored:
        # Simple check - could be improved with proper gitignore parsing
        if pattern not in gitignore_content and pattern.replace("*", "") not in gitignore_content:
            issues.append(f"WARNING: '{pattern}' might not be in .gitignore")

    return issues


def scan_directory(root_path: Path) -> list[dict[str, str | int]]:
    """Scan directory tree for credential leaks."""
    issues: list[dict[str, str | int]] = []

    for filepath in root_path.rglob("*"):
        # Skip directories
        if filepath.is_dir():
            continue

        # Skip ignored directories
        if any(skip_dir in filepath.parts for skip_dir in SKIP_DIRS):
            continue

        # Skip non-text files
        if filepath.suffix not in [
            ".py",
            ".cpp",
            ".h",
            ".hpp",
            ".c",
            ".js",
            ".ts",
            ".json",
            ".yaml",
            ".yml",
            ".txt",
            ".md",
            ".ini",
            ".conf",
            ".config",
            ".env",
            ".sh",
            ".bash",
        ]:
            continue

        file_issues = check_file_for_credentials(filepath)
        issues.extend(file_issues)

    return issues


def main() -> int:
    """Run security audit."""
    print("🔒 Security Audit Starting...")
    print("-" * 60)

    root_path = Path.cwd()
    all_issues: list[dict[str, str | int] | str] = []

    # Check for credentials in code
    print("Scanning for credentials in code...")
    code_issues = scan_directory(root_path)
    if code_issues:
        print(f"  ⚠️  Found {len(code_issues)} potential credential leaks")
        for issue in code_issues[:10]:  # Show first 10
            print(f"    - {issue['file']}:{issue['line']} - {issue['match']}")
        if len(code_issues) > 10:
            print(f"    ... and {len(code_issues) - 10} more")
        all_issues.extend(code_issues)
    else:
        print("  ✅ No credentials found in code")

    # Check git history
    print("\nChecking git history...")
    git_issues = check_git_history()
    if git_issues:
        print(f"  ⚠️  Found {len(git_issues)} files in git history")
        for git_issue in git_issues:
            print(f"    - {git_issue}")
        all_issues.extend(git_issues)
    else:
        print("  ✅ No sensitive files in git history")

    # Check .gitignore
    print("\nChecking .gitignore configuration...")
    gitignore_issues = check_gitignore()
    if gitignore_issues:
        print(f"  ⚠️  Found {len(gitignore_issues)} .gitignore issues")
        for gitignore_issue in gitignore_issues:
            print(f"    - {gitignore_issue}")
        all_issues.extend(gitignore_issues)
    else:
        print("  ✅ .gitignore properly configured")

    # Check for common security files
    print("\nChecking for security files...")
    if Path(".env.example").exists():
        print("  ✅ .env.example exists")
    else:
        print("  ⚠️  No .env.example found")
        all_issues.append("Missing .env.example file")

    if Path("config/device.sample.yaml").exists():
        print("  ✅ device.sample.yaml exists")
    else:
        print("  ⚠️  No device.sample.yaml found")
        all_issues.append("Missing device.sample.yaml file")

    # Summary
    print("\n" + "=" * 60)
    if all_issues:
        print(f"🚨 SECURITY AUDIT FAILED: {len(all_issues)} issues found")
        print("\nRecommended actions:")
        print("1. Remove any real credentials from code")
        print("2. Use environment variables or .env files")
        print("3. If credentials were committed, consider rotating them")
        print("4. To remove from git history, use git filter-branch or BFG")
        return 1
    else:
        print("✅ SECURITY AUDIT PASSED: No issues found")
        return 0


if __name__ == "__main__":
    sys.exit(main())
