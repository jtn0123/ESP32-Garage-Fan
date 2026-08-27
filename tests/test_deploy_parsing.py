"""deploy.sh's JSON field extraction, run through the real sed.

This exists because of a live failure on 2026-08-11. The confirm check was

    sed -n 's/.*"confirmed":\\(true\\|false\\).*/\\1/p'

and `\\|` alternation in a BRE is a GNU extension. BSD/macOS sed matches
nothing, silently: `confirmed` came back empty, the verify looped to its
timeout, and the script announced DEPLOY UNCONFIRMED -- "it could not reach the
MQTT broker ... check the broker before power-cycling" -- for a board that had
confirmed three seconds after boot. The neighbouring `fw` extraction is plain
BRE, so the version read fine and only the confirmation went missing, which is
what made it look like a device fault rather than a script bug.

The expressions are read OUT of deploy.sh rather than copied here, so this
tests what the script will actually run, and it runs them through the system
sed, so it fails on the machine whose sed cannot cope.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEPLOY = ROOT / "scripts" / "deploy.sh"

# A real /api/state body, captured from the device on 1.14.29.
LIVE_STATE = (
    '{"speed":3,"auto":true,"auto_max":9,"auto_min":0,"on_f":2.5,"off_f":1.5,'
    '"outside_f":76.4,"toff":-8.0,"offc":-8.0,"offi":-8.0,"fw":"1.14.29",'
    '"slot":"ota_1","confirmed":true,"unhealthy_boots":0,"sensor":true,'
    '"last_reset":"sw_reset","boots":41,"prev_death":"sw_reset","sd_q":false,'
    '"sd_total_mb":28887,"sd_used_mb":0,"sd_free_mb":28886,'
    '"batt":{"v":4.198,"pct":100,"chg":true,"eta_h":null,"mvh":null},'
    '"rssi":-59,"drops":0,"mqtt":true,"uptime_s":468,"ip":"192.0.2.187"}'
)


def sed_exprs() -> dict[str, str]:
    """Pull each `name=$(echo "$state" | sed -n '...')` expression out of deploy.sh."""
    text = DEPLOY.read_text()
    found = {}
    for name, expr in re.findall(r'(\w+)=\$\(echo "\$state" \| sed -n \'([^\']+)\'\)', text):
        found[name] = expr
    assert found, "no `echo \"$state\" | sed -n '...'` extractions found in deploy.sh"
    return found


def run_sed(expr: str, payload: str) -> str:
    out = subprocess.run(
        ["sed", "-n", expr], input=payload, capture_output=True, text=True, check=True
    )
    return out.stdout.strip()


def test_every_state_field_extracts_on_this_sed() -> None:
    """Each extraction must return something from a real payload.

    An expression that yields "" is the exact failure mode: deploy.sh treats
    empty as "not confirmed yet" / "health field missing" and reports a failure
    that did not happen.
    """
    exprs = sed_exprs()
    empty = {n: e for n, e in exprs.items() if not run_sed(e, LIVE_STATE)}
    assert not empty, (
        f"these deploy.sh extractions matched nothing on this system's sed: "
        f"{sorted(empty)}. A BRE that only works under GNU sed (\\| alternation, "
        f"\\+, \\?) silently yields empty here, and deploy.sh reads empty as a "
        f"failed deploy."
    )


def test_extracted_values_are_correct() -> None:
    exprs = sed_exprs()
    expected = {
        "fw": "1.14.29",
        "confirmed": "true",
        "cause": "sw_reset",
        "sd_mb": "28887",
        "drops": "0",
    }
    for name, want in expected.items():
        assert name in exprs, f"deploy.sh no longer extracts {name}"
        assert run_sed(exprs[name], LIVE_STATE) == want, f"{name} extracted wrongly"


def test_confirmed_false_is_distinguished_from_missing() -> None:
    """A false must read as "false", not as empty.

    deploy.sh branches on `confirmed = true`; if false and missing both came
    back empty it could never tell "still booting" from "rolled back".
    """
    expr = sed_exprs()["confirmed"]
    assert run_sed(expr, LIVE_STATE.replace('"confirmed":true', '"confirmed":false')) == "false"


def test_no_gnu_only_bre_in_state_extractions() -> None:
    """Static guard, so this cannot regress on a GNU-sed developer machine.

    The runtime tests above only fail where sed is BSD. Someone working on
    Linux could reintroduce \\| and see everything pass.
    """
    offenders = {n: e for n, e in sed_exprs().items() if re.search(r"\\\||\\\+|\\\?", e)}
    assert not offenders, (
        f"GNU-only BRE syntax in {sorted(offenders)}. \\| \\+ \\? are GNU "
        f"extensions; BSD/macOS sed matches nothing and deploy.sh reads that as "
        f"a failed deploy. Use a character class, e.g. \\([a-z]*\\)."
    )


def test_truncated_payload_yields_empty_so_the_smoke_check_fails_closed() -> None:
    """deploy.sh requires cause/sd_mb/drops to be non-empty and aborts otherwise.

    That guard only works if a truncated body really does produce empty rather
    than a partial match, so pin it.
    """
    exprs = sed_exprs()
    truncated = LIVE_STATE[: LIVE_STATE.index('"last_reset"')]
    assert run_sed(exprs["fw"], truncated) == "1.14.29"  # early fields survive
    for name in ("cause", "drops"):
        assert run_sed(exprs[name], truncated) == "", f"{name} should be empty when cut off"
