#!/usr/bin/env python3
"""The generators listed in platformio.ini must actually run as SCons pre-scripts.

PlatformIO runs `extra_scripts = pre:...` entries through SCons's SConscript(), which
exec()s the file against SCons.Script's globals. That means:

  * `__name__` is "SCons.Script", never "__main__"
  * `__file__` is deleted from the globals (SCons/Script/SConscript.py)
  * `sys.argv` belongs to SCons
  * the working directory stays wherever `pio` was launched (firmware/arduino)

Every generator guarded its entry point with `if __name__ == "__main__"` alone, so
none of them ran during a build: a clean checkout failed to compile with
`fatal error: generated_config.h: No such file or directory`. These tests reproduce
SCons's calling convention against a throwaway copy of the repo, so the regression is
caught without PlatformIO installed.
"""

import os
from pathlib import Path
import shutil

import pytest

ROOT = Path(__file__).resolve().parents[1]
PLATFORMIO_INI = ROOT / "firmware" / "arduino" / "platformio.ini"

# Generators PlatformIO invokes, and the file each one must produce.
PRESCRIPTS = {
    "gen_device_header.py": "firmware/arduino/src/generated_config.h",
    "gen_ui.py": "firmware/arduino/src/ui_ops_generated.cpp",
}

# Inputs a generator reads, copied into the throwaway repo.
FIXTURE_FILES = [
    "config/device.sample.yaml",
    "config/ui_spec.json",
    "config/display_geometry.json",
    "firmware/arduino/platformio.ini",
]


def prescripts_in_ini() -> list[str]:
    """Pre-script basenames declared in platformio.ini, in declaration order."""
    names: list[str] = []
    in_block = False
    for raw in PLATFORMIO_INI.read_text().splitlines():
        line = raw.strip()
        if line.startswith(";"):
            continue
        if line.startswith("extra_scripts"):
            in_block = True
            # Also handle a value on the same line as the key.
            line = line.split("=", 1)[1].strip() if "=" in line else ""
        elif in_block and not raw.startswith((" ", "\t")):
            in_block = False
        if in_block and line.startswith("pre:"):
            names.append(Path(line[len("pre:") :]).name)
    return names


def make_fake_repo(tmp_path: Path) -> Path:
    """A throwaway repo layout the generators write into instead of the real tree.

    Only the inputs are copied. The generators themselves run from their real
    location (see run_as_scons_prescript) so `pytest --cov=scripts` attributes the
    lines it executes to scripts/, not to a temporary copy.
    """
    fake = tmp_path / "repo"
    (fake / "scripts").mkdir(parents=True)
    (fake / "firmware" / "arduino" / "src").mkdir(parents=True)
    (fake / "config").mkdir(parents=True)
    for rel in FIXTURE_FILES:
        shutil.copy2(ROOT / rel, fake / rel)
    return fake


def run_as_scons_prescript(name: str, cwd: Path, module_name: str = "SCons.Script") -> None:
    """Execute scripts/<name> the way SCons executes an SConscript.

    No __file__ in the globals, __name__ set to "SCons.Script", cwd left at the
    PlatformIO project directory. SCons also prepends the script's directory to
    sys.path; that detail does not affect these generators.

    The real scripts/<name> is compiled under its real path: without __file__ the
    generators derive the repo root from the cwd, so passing a cwd inside the
    throwaway repo is what keeps writes out of the real tree (asserted by
    test_prescript_resolves_repo_root_without_file). Compiling the real path rather
    than a copy is what makes these executed lines count toward coverage.
    """
    script = ROOT / "scripts" / name
    globals_dict = {"__name__": module_name, "__builtins__": __builtins__}
    assert "__file__" not in globals_dict
    prev = os.getcwd()
    os.chdir(cwd)
    try:
        exec(compile(script.read_text(), str(script), "exec"), globals_dict)  # noqa: S102
    finally:
        os.chdir(prev)


@pytest.mark.parametrize("name,produces", sorted(PRESCRIPTS.items()))
def test_prescript_generates_its_output_under_scons(name, produces, tmp_path):
    """The failure this pins: the generator ran, printed nothing, wrote nothing."""
    fake = make_fake_repo(tmp_path)
    target = fake / produces
    assert not target.exists()

    run_as_scons_prescript(name, cwd=fake / "firmware" / "arduino")

    assert target.exists(), f"{name} produced no {produces} when run as a pre-script"
    assert target.read_text().strip(), f"{name} wrote an empty {produces}"


@pytest.mark.parametrize("name,produces", sorted(PRESCRIPTS.items()))
def test_prescript_resolves_repo_root_without_file(name, produces, tmp_path):
    """Without __file__ the root comes from the cwd walk, not the real checkout."""
    fake = make_fake_repo(tmp_path)
    real = ROOT / produces
    before = real.read_bytes() if real.exists() else None

    run_as_scons_prescript(name, cwd=fake / "firmware" / "arduino")

    # Assert it ran, so this cannot pass just because the generator did nothing.
    assert (fake / produces).exists(), f"{name} produced no {produces}"
    after = real.read_bytes() if real.exists() else None
    assert after == before, f"{name} wrote into the real repo instead of the cwd's root"


def test_every_declared_prescript_is_covered():
    """A new pre:... entry in platformio.ini must be exercised here too."""
    declared = prescripts_in_ini()
    assert declared, "no pre-scripts parsed from platformio.ini"
    assert set(declared) == set(PRESCRIPTS), (
        f"platformio.ini declares {declared}, tests cover {sorted(PRESCRIPTS)}; "
        "add the new generator to PRESCRIPTS"
    )


def test_gen_layout_header_is_not_a_prescript():
    """It writes display_layout.h with unprefixed rect names; gen_ui.py owns that
    file with the RECT_-prefixed names display_layout_aliases.h expects, so two
    writers for one file made compilation depend on pre-script ordering."""
    assert "gen_layout_header.py" not in prescripts_in_ini()


@pytest.mark.parametrize("name", sorted(PRESCRIPTS))
def test_prescript_entry_point_accepts_scons_name(name):
    """Guard against someone dropping the SCons arm of the entry point again.

    Only the name is pinned, not the shape of the guard: gen_device_header.py runs
    the same body for both names so it uses `if __name__ in (...)`, while gen_ui.py
    passes an empty argv under SCons and needs a separate arm.
    """
    source = (ROOT / "scripts" / name).read_text()
    entry = source.split("\nif __name__", 1)[-1]
    assert (
        '"SCons.Script"' in entry
    ), f"scripts/{name} must run under SCons, which sets __name__ to 'SCons.Script'"


@pytest.mark.parametrize("name", sorted(PRESCRIPTS))
def test_importing_a_generator_writes_nothing(name, tmp_path):
    """Tests import these modules to call individual functions; that must stay
    side-effect free, or a plain pytest run regenerates committed files in place."""
    fake = make_fake_repo(tmp_path)
    run_as_scons_prescript(name, cwd=fake / "firmware" / "arduino", module_name="a_test_module")
    produced = fake / PRESCRIPTS[name]
    assert not produced.exists(), f"importing scripts/{name} wrote {PRESCRIPTS[name]}"


def test_repo_root_reports_where_it_looked_when_outside_a_checkout(tmp_path, monkeypatch):
    """With no __file__ and a cwd outside any checkout there is nothing to fall back
    on, so fail with the path searched rather than writing to a guessed root."""
    empty = tmp_path / "not-a-checkout"
    empty.mkdir()
    monkeypatch.chdir(empty)
    source = (ROOT / "scripts" / "gen_device_header.py").read_text()
    globals_dict = {"__name__": "a_test_module", "__builtins__": __builtins__}
    with pytest.raises(RuntimeError, match="cannot locate repo root"):
        exec(  # noqa: S102
            compile(source, str(ROOT / "scripts" / "gen_device_header.py"), "exec"), globals_dict
        )
