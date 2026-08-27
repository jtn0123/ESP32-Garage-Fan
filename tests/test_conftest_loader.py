"""The loader every other test leans on, and the errors it exists to give.

Worth its own file because a broken load_script does not fail loudly -- it
fails as whatever the *calling* test was asserting, and the real cause (a
renamed script) is nowhere in the message. These pin the three behaviours the
callers depend on.
"""

from __future__ import annotations

import sys

from conftest import SCRIPTS, load_script
import pytest


def test_it_loads_a_script_by_name() -> None:
    mod = load_script("mock_state")
    assert hasattr(mod, "PLUG_BASELINE"), "loaded the wrong module"


def test_a_missing_script_names_the_path_it_looked_for() -> None:
    """The regression this replaces: a renamed script used to surface as
    "'NoneType' object has no attribute 'exec_module'", which points at the
    loader instead of at the file that moved."""
    with pytest.raises(FileNotFoundError) as e:
        load_script("no_such_script_at_all")
    assert "no_such_script_at_all.py" in str(e.value)
    assert str(SCRIPTS) in str(e.value)


def test_a_private_name_does_not_collide_with_the_real_module() -> None:
    """Two tests loading one script under one name would share its globals --
    and these scripts keep state at module level (mock_device's STATE, for
    one), so that sharing is exactly the cross-test leakage the e2e harness
    starts a whole process per worker to avoid."""
    a = load_script("mock_state", module_name="_probe_a")
    b = load_script("mock_state", module_name="_probe_b")
    assert a is not b
    assert sys.modules["_probe_a"] is a
    assert sys.modules["_probe_b"] is b
    for name in ("_probe_a", "_probe_b"):
        del sys.modules[name]


def test_the_module_is_registered_before_it_executes() -> None:
    """A script that reaches for its own name during import -- dataclasses and
    pickle both do -- needs to find itself in sys.modules already."""
    mod = load_script("mock_state", module_name="_probe_c")
    assert sys.modules.get("_probe_c") is mod
    del sys.modules["_probe_c"]
