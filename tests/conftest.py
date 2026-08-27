"""Shared plumbing for reaching scripts/ from the tests.

``scripts/`` is a directory of standalone tools, not an installed package, so a
test that wants one has to go and get it. That was happening four different
ways -- ``sys.path.insert`` with ``os.path.join``, the same with ``pathlib``,
and two hand-rolled ``spec_from_file_location`` loaders -- and the hand-rolled
ones had a sharp edge: ``spec_from_file_location`` returns ``None`` when it
cannot place the file, and ``spec.loader`` can be ``None`` too, so renaming a
script did not fail with "that script is gone". It failed with

    AttributeError: 'NoneType' object has no attribute 'exec_module'

pointing at the loader rather than at the missing file. mypy flagged exactly
that (union-attr, arg-type) once tests/ came into scope.

So: one place that puts scripts/ on the path, and one loader that says what
went wrong. ``load_script`` is for modules that must be loaded under their own
private name -- a script with import side effects, or one a test wants a fresh
copy of. Everything else can just ``import`` now that the path is set.
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys
from types import ModuleType

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"

# Once, at collection, for every test in this directory. mypy is told the same
# thing by mypy_path in pyproject.toml, so what the tests can import and what
# mypy will follow stay the same set.
for _p in (str(ROOT), str(SCRIPTS)):
    if _p not in sys.path:
        sys.path.insert(0, _p)


def load_script(name: str, module_name: str | None = None) -> ModuleType:
    """Import ``scripts/<name>.py`` under ``module_name`` (default: ``name``).

    Raises FileNotFoundError naming the path when the script is not there, and
    ImportError when Python can build a spec but no loader for it -- which is
    the case the old inline version turned into an AttributeError.
    """
    path = SCRIPTS / f"{name}.py"
    if not path.is_file():
        raise FileNotFoundError(f"no such script: {path}")

    spec = importlib.util.spec_from_file_location(module_name or name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot build an import spec for {path}")

    module = importlib.util.module_from_spec(spec)
    # Registered before exec_module: a script that imports itself, or uses
    # dataclasses/pickle, resolves its own name through sys.modules.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module
