#!/usr/bin/env python3
"""Hand the compiler the difference between our code and the code we vendor.

The problem this solves: -Wshadow is worth having on the device build. A
parameter that quietly wins over a member of the same name is a real bug class,
and it had already produced one here -- CycleDetector::flips_in() took a
`polls` parameter shadowing the `polls` member, where the two mean different
things. But turning the flag on drowned it: ~128 warnings from 70 distinct
third-party HEADERS (Adafruit EPD's panel and driver headers, and the Arduino
Network library) against one of ours.

The wrong fix is to suppress the warning. The right one is to stop claiming
those headers are ours. GCC already draws this line: a warning whose origin is
a *system* header is not reported, on the reasoning that you cannot fix code
you do not own. PlatformIO passes every include directory as -I, which asserts
the opposite -- that the vendored Adafruit tree is first-party source we are
choosing not to fix.

So this re-offers every include directory that is not ours as -isystem, on the
compilation of our own sources. Nothing is silenced by name and no warning is
turned off: src/ is still compiled with exactly the flags platformio.ini lists,
and a shadow, a sign compare or a format truncation in our code still fails the
build under CI's -Werror. What changes is only which tree the compiler
attributes a warning to.

Three things worth knowing if this ever misbehaves:

  * It APPENDS -isystem rather than rewriting CPPPATH. When a directory is
    named by both, -isystem wins and -I is dropped, so the vendored headers
    become system headers without disturbing PlatformIO's search order or
    risking a wrong-header pickup. Rewriting CPPPATH was tried first and is
    both riskier and, as it turns out, unnecessary.
  * It uses `projenv`, not `env`. `env` is the environment the framework and
    libraries build under; `projenv` is the one PlatformIO clones for project
    sources and where build_src_flags lands. Appending to `env` compiles our
    sources with no -isystem at all -- verified by counting the flag on the
    real command line, which was zero.
  * The proof that it changes no code generation is identical .text/.data/.bss
    before and after (1048888/236053/146995 at the time of writing). NOT a
    matching firmware.bin: this build is not reproducible -- ESP-IDF stamps a
    build time into the image, so two runs of the SAME tree already differ.
    Compare sections, not hashes, if the platform package is ever bumped.
"""

from __future__ import annotations

import os
from typing import Any

# SCons builds `Import` into this script's globals and then exec()s it, so the
# name is real at run time and absent at import time. Reaching it through
# globals() and binding the results to ordinary module variables says that
# honestly -- and keeps every other name in this file subject to the undefined-
# name check, which a file-wide noqa or a ruff `builtins` entry would not.
_scons: dict[str, Any] = globals()
_scons["Import"]("env", "projenv")

# `env` is what the framework and libraries build under; `projenv` is the clone
# PlatformIO uses for project sources, and where build_src_flags lands.
env: Any = _scons["env"]
projenv: Any = _scons["projenv"]

# Untyped past this point on purpose: SCons environments are dynamic
# dictionaries with no stubs, so Any is the truthful annotation rather than a
# guess dressed up as a type.
SconsEnv = Any


def _ours(build_env: SconsEnv) -> tuple[str, ...]:
    """The project's own trees, resolved.

    Deliberately a prefix test against the directories PlatformIO names rather
    than a pattern match on "libdeps" or ".platformio": a dependency that lands
    in some new location should default to being someone else's code, not ours.
    """
    dirs = (build_env.subst("$PROJECT_SRC_DIR"), build_env.subst("$PROJECT_INCLUDE_DIR"))
    return tuple(os.path.realpath(d) for d in dirs if d)


def vendored_include_dirs() -> list[str]:
    """Every include directory in the build that is not ours, deduplicated.

    Both environments are read: `env` carries the framework and the resolved
    library paths, `projenv` what our own sources see. A directory reachable
    from either is a directory a project source can include from.
    """
    ours = _ours(env)
    seen: dict[str, None] = {}
    for build_env in (env, projenv):
        for entry in build_env.get("CPPPATH", []):
            path = os.path.realpath(build_env.subst(str(entry)))
            if not any(path.startswith(d) for d in ours):
                seen.setdefault(path, None)
    return list(seen)


dirs = vendored_include_dirs()
# A ("-isystem", dir) tuple is the SCons spelling for a two-word flag: it
# survives into argv as two entries, so a directory with a space in its name
# (there is one -- "Adafruit EPD") does not come apart.
projenv.Append(CCFLAGS=[("-isystem", d) for d in dirs])

print(f"vendor include dirs re-offered as -isystem: {len(dirs)}")
