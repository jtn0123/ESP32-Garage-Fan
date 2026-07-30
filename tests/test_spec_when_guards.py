#!/usr/bin/env python3
"""Contract tests for ui_spec.json `when` guards.

An op may carry `"when": "has(<field>)"`, meaning "only draw this when <field> has a
value". The web simulator honored that from the start; the firmware pipeline did not,
so the device permanently drew "-- hPa" in OUT_PRESSURE. These tests pin every hop of
the guard: spec -> gen_ui.py -> UiOpHeader.s1 -> the renderers.
"""

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
UI_SPEC = ROOT / "config" / "ui_spec.json"
OPS_CPP = ROOT / "firmware" / "arduino" / "src" / "ui_ops_generated.cpp"
OPS_H = ROOT / "firmware" / "arduino" / "src" / "ui_ops_generated.h"
MAIN_CPP = ROOT / "firmware" / "arduino" / "src" / "main.cpp"
SIM_JS = ROOT / "web" / "sim" / "sim.js"
GEN_UI = ROOT / "scripts" / "gen_ui.py"

WHEN_HAS_RE = re.compile(r"^has\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)$")
# Trailing s1 slot of a generated UiOpHeader row: NULL or a string literal.
OP_ROW_S1_RE = re.compile(r'^\s*\{ OP_.*,\s*(NULL|"([^"]*)")\s*\},\s*$')


def load_spec() -> dict:
    return json.loads(UI_SPEC.read_text())


def spec_guard_fields() -> list[str]:
    """Guard fields for every guarded op in the spec, in component/op order."""
    fields: list[str] = []
    for ops in load_spec()["components"].values():
        for op in ops:
            when = op.get("when")
            if when is None:
                continue
            m = WHEN_HAS_RE.match(str(when).strip())
            assert m, f"unsupported when clause {when!r} (only has(<field>) is supported)"
            fields.append(m.group(1))
    return fields


def generated_ops_s1() -> list[str | None]:
    """The s1 slot of every generated op row, in file order."""
    out: list[str | None] = []
    for line in OPS_CPP.read_text().splitlines():
        m = OP_ROW_S1_RE.match(line)
        if m:
            out.append(None if m.group(1) == "NULL" else m.group(2))
    return out


def test_spec_when_clauses_use_supported_form():
    """Fails loudly if a new clause form is added without teaching the pipeline."""
    fields = spec_guard_fields()
    assert fields, "expected at least one guarded op (e.g. OUT_PRESSURE)"
    assert "outside_pressure_hpa" in fields
    assert "pressure_hpa" in fields


def test_generated_ops_carry_guard_fields():
    """gen_ui.py must emit each guard field into UiOpHeader.s1, not drop it."""
    emitted = [s for s in generated_ops_s1() if s is not None]
    assert emitted == spec_guard_fields()


def test_unguarded_ops_have_null_s1():
    rows = generated_ops_s1()
    assert rows, "no generated op rows parsed"
    assert rows.count(None) == len(rows) - len(spec_guard_fields())


def test_ops_header_documents_s1_contract():
    assert "when: has(" in OPS_H.read_text(), "UiOpHeader should document the s1 guard slot"


def test_committed_generated_ops_are_up_to_date(tmp_path):
    """The committed table must match a fresh run, or the device uses stale guards."""
    result = subprocess.run(
        [sys.executable, str(GEN_UI), "--output", str(tmp_path)],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert result.returncode == 0, result.stderr or result.stdout
    for path in (OPS_CPP, OPS_H):
        fresh = tmp_path / path.relative_to(ROOT)
        assert fresh.read_text() == path.read_text(), f"{path.name} is stale; rerun gen_ui.py"


def test_gen_ui_rejects_unsupported_when_clause(tmp_path):
    """A clause gen_ui.py cannot express must fail the build, not vanish."""
    spec = {
        "schema": "ui-spec@1",
        "canvas": {"w": 250, "h": 122},
        "fonts": {"tokens": {"small": {"px": 10}}},
        "rects": {"BOX": [0, 0, 100, 20]},
        "components": {
            "body": [{"op": "text", "rect": "BOX", "text": "hi", "when": "!has(some_field)"}]
        },
        "variants": {"v1": ["body"]},
    }
    spec_path = tmp_path / "ui_spec.json"
    spec_path.write_text(json.dumps(spec))
    result = subprocess.run(
        [sys.executable, str(GEN_UI), "--spec", str(spec_path), "--output", str(tmp_path / "out")],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert result.returncode != 0, "unsupported when clause should fail generation"
    assert "when" in result.stdout + result.stderr


def test_firmware_evaluates_every_guard_field():
    """spec_field_has() reports unknown fields absent, so a missing case hides a row."""
    txt = MAIN_CPP.read_text()
    assert "static bool spec_field_has(const char* field)" in txt
    # The guard is applied once for every op kind, before the switch.
    assert "if (op.s1 && !spec_field_has(op.s1))" in txt
    for field in set(spec_guard_fields()):
        assert f'strcmp(field, "{field}") == 0' in txt, f"spec_field_has() ignores {field}"


def test_firmware_formats_outside_pressure():
    """Without a formatter the guarded row would render "-- hPa" once data arrives."""
    txt = MAIN_CPP.read_text()
    assert 'key.startsWith("outside_pressure_hpa")' in txt
    assert "o.pressureHPa" in txt


def test_sim_applies_guard_to_every_op():
    """The guard must sit before the op switch, matching the firmware."""
    txt = SIM_JS.read_text()
    assert "if (op.when && !specFieldHas(op.when, data)) continue;" in txt
    switch_at = txt.index("switch(op.op){")
    guard_at = txt.index("if (op.when && !specFieldHas(op.when, data)) continue;")
    assert guard_at < switch_at, "guard should run before the op switch"


def _run_node(script: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
        f.write(script)
        path = f.name
    try:
        result = subprocess.run(["node", path], capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stderr
        return result.stdout.strip()
    finally:
        Path(path).unlink(missing_ok=True)


def extract_sim_function(name: str) -> str:
    """Pull a top-level `function name(...)` body out of sim.js by brace matching."""
    txt = SIM_JS.read_text()
    start = txt.index(f"function {name}(")
    depth = 0
    i = txt.index("{", start)
    for j in range(i, len(txt)):
        if txt[j] == "{":
            depth += 1
        elif txt[j] == "}":
            depth -= 1
            if depth == 0:
                return txt[start : j + 1]
    raise AssertionError(f"unbalanced braces extracting {name}")


def extract_sim_guard_source() -> str:
    """The simulator's guard predicate plus the sentinel list it depends on."""
    txt = SIM_JS.read_text()
    m = re.search(r"^\s*const UNAVAILABLE = \[[^\]]*\];", txt, re.M)
    assert m, "UNAVAILABLE sentinel list not found in sim.js"
    return "\n".join(
        [
            m.group(0).strip(),
            extract_sim_function("specFieldHasValue"),
            extract_sim_function("specFieldHas"),
        ]
    )


@pytest.mark.skipif(
    subprocess.run(["which", "node"], capture_output=True).returncode != 0,
    reason="node not installed",
)
def test_sim_spec_field_has_semantics():
    """Absent in both data and DEFAULTS -> skip; present in either -> draw."""
    script = f"""
global.window = {{ DEFAULTS: {{ pressure_hpa: 1013 }} }};
{extract_sim_guard_source()}
const cases = [
  specFieldHas('has(outside_pressure_hpa)', {{ outside_pressure_hpa: 1016 }}),
  specFieldHas('has(outside_pressure_hpa)', {{}}),
  specFieldHas('has(outside_pressure_hpa)', {{ outside_pressure_hpa: null }}),
  specFieldHas('has(pressure_hpa)', {{}}),
  specFieldHas('has( outside_pressure_hpa )', {{ outside_pressure_hpa: 0 }}),
];
console.log(cases.map(String).join(','));
"""
    assert _run_node(script) == "true,false,false,true,true"


@pytest.mark.skipif(
    subprocess.run(["which", "node"], capture_output=True).returncode != 0,
    reason="node not installed",
)
def test_sim_guard_rejects_non_finite_and_unavailable():
    """The firmware guards on isfinite(), so the sim must not treat NaN, Infinity,
    an empty string, or Home Assistant's unavailable sentinels as a value -- it
    would render a row the device leaves blank."""
    script = f"""
global.window = {{}};
{extract_sim_guard_source()}
const absent = ['', '   ', NaN, 'NaN', Infinity, -Infinity, 'unavailable', 'unknown', 'none'];
const present = [1016, '1016', '1016.5', 0, '0', -3];
const bad = absent.filter(v => specFieldHas('has(f)', {{ f: v }}) !== false);
const missed = present.filter(v => specFieldHas('has(f)', {{ f: v }}) !== true);
// A non-numeric string is a real value for a non-numeric field (e.g. weather).
const str = specFieldHas('has(f)', {{ f: 'cloudy' }});
console.log(JSON.stringify({{ bad: bad.map(String), missed: missed.map(String), str }}));
"""
    assert json.loads(_run_node(script)) == {"bad": [], "missed": [], "str": True}
