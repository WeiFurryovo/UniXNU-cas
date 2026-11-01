#!/usr/bin/env python3
"""Run build-arm-driver.py with clang 24's -Wmissing-designated-field-initializers off.

build-arm-driver.py compiles with -Wextra -Werror. In clang 24 -Wextra includes -Wmissing-designated-field-initializers,
which fires inside XNU's own headers (kalloc.h, vm_kern.h, IOLib.h, OSMetaClass.h) as soon as a driver defines an OSObject
class with the kernel's allocation macros, as NativeLegacyDisplay does; Apple's toolchain does not emit it for these
sources. The flag is appended after the script's own warning switches and nothing else changes; the substitution must
match exactly once.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
script = ROOT / 'scripts/build-arm-driver.py'
source = script.read_text()
marker = "command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',"
if source.count(marker) != 1:
    raise SystemExit('build-arm-driver.py changed: review the warning override')
source = source.replace(marker, marker + " '-Wno-missing-designated-field-initializers',")
sys.argv[0] = str(script)
exec(compile(source, str(script), 'exec'), {'__file__': str(script), '__name__': '__main__'})
