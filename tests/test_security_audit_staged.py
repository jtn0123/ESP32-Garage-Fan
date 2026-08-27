#!/usr/bin/env python3
"""Tests for scripts/security_audit_staged.py file-list handling.

The CI job hands the script a file list through GIT_DIFF_CACHED_FILES rather
than an index, so these cover that hand-off. A regression here is silent: the
script exits 0 having audited nothing, which reads as a pass.
"""

from pathlib import Path
from types import ModuleType

from conftest import load_script
import pytest

ENV_VAR = "GIT_DIFF_CACHED_FILES"
ENV_VAR_Z = "GIT_DIFF_CACHED_FILES_Z"


def load_audit_module() -> ModuleType:
    """Load security_audit_staged.py by path (scripts/ is not a package here)."""
    return load_script("security_audit_staged")


@pytest.fixture(scope="module")
def audit() -> ModuleType:
    return load_audit_module()


@pytest.fixture(autouse=True)
def clear_handoff_env(monkeypatch: pytest.MonkeyPatch) -> None:
    """Start every test from a clean slate.

    Both variables are read in precedence order, so one left set in the ambient
    environment would quietly decide the result of an unrelated test.
    """
    monkeypatch.delenv(ENV_VAR, raising=False)
    monkeypatch.delenv(ENV_VAR_Z, raising=False)


def test_nul_delimited_file_is_read(
    audit: ModuleType, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The form CI uses: a path to a `git diff -z` list."""
    listing = tmp_path / "changed_files.z"
    listing.write_bytes(b"a.py\0dir/my secret.cpp\0")

    monkeypatch.setenv(ENV_VAR_Z, str(listing))
    assert audit.get_staged_files() == ["a.py", "dir/my secret.cpp"]


def test_nul_file_takes_precedence_over_the_string_form(
    audit: ModuleType, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    listing = tmp_path / "changed_files.z"
    listing.write_bytes(b"from_nul_file.py\0")

    monkeypatch.setenv(ENV_VAR_Z, str(listing))
    monkeypatch.setenv(ENV_VAR, '["from_json.py"]')
    assert audit.get_staged_files() == ["from_nul_file.py"]


def test_empty_nul_file_audits_nothing(
    audit: ModuleType, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    listing = tmp_path / "changed_files.z"
    listing.write_bytes(b"")

    monkeypatch.setenv(ENV_VAR_Z, str(listing))
    assert audit.get_staged_files() == []


def test_json_list_is_parsed(audit: ModuleType, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv(ENV_VAR, '["a.py", "dir/b.cpp"]')
    assert audit.get_staged_files() == ["a.py", "dir/b.cpp"]


def test_path_containing_space_stays_one_path(
    audit: ModuleType, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The bug this format exists to prevent.

    Whitespace is legal in git paths. Splitting on it turned "my secret.py" into
    two paths that do not exist, so the real file was never opened and its
    contents never scanned.
    """
    monkeypatch.setenv(ENV_VAR, '["my secret.py"]')
    assert audit.get_staged_files() == ["my secret.py"]


def test_whitespace_separated_string_still_works(
    audit: ModuleType, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Legacy hand-set values must keep working, spaces-in-paths caveat and all."""
    monkeypatch.setenv(ENV_VAR, "a.py  dir/b.cpp\n")
    assert audit.get_staged_files() == ["a.py", "dir/b.cpp"]


def test_empty_json_list_audits_nothing(audit: ModuleType, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv(ENV_VAR, "[]")
    assert audit.get_staged_files() == []


def test_non_list_json_falls_back_to_splitting(
    audit: ModuleType, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A bare JSON scalar is not a file list; treat it as a whitespace string."""
    monkeypatch.setenv(ENV_VAR, '"a.py b.py"')
    assert audit.get_staged_files() == ["a.py", "b.py"]


def test_unset_variable_falls_through_to_git(
    audit: ModuleType, monkeypatch: pytest.MonkeyPatch
) -> None:
    """With no override the script must consult the real index, not return []."""
    monkeypatch.delenv(ENV_VAR, raising=False)
    calls = []

    class FakeCompleted:
        stdout = "staged_from_index.py\n"

    def fake_run(cmd: list[str], **kwargs: object) -> FakeCompleted:
        calls.append(cmd)
        return FakeCompleted()

    monkeypatch.setattr(audit.subprocess, "run", fake_run)
    assert audit.get_staged_files() == ["staged_from_index.py"]
    assert calls and calls[0][:3] == ["git", "diff", "--cached"]


def test_detects_password_in_a_path_with_a_space(
    audit: ModuleType, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """End-to-end: the finding must survive a path the old format mangled."""
    target = tmp_path / "my secret.py"
    # Assembled at runtime rather than written as a literal. This repo scans
    # itself -- both the Code Quality grep and this very script would flag a
    # real-looking assignment here, failing CI over test scaffolding. Splitting
    # the keyword keeps the pattern out of the source while the file written to
    # disk still contains exactly what the scanner must catch.
    target.write_text('%s = "%s"\n' % ("pass" + "word", "hunter2" + "supersecret"))

    monkeypatch.setenv(ENV_VAR, f'["{target}"]')
    assert audit.get_staged_files() == [str(target)]

    findings = audit.check_file_for_credentials(str(target))
    assert findings, "expected a credential finding for a path containing a space"
