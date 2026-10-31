#!/usr/bin/env python3
"""Run build-verified-root-probe.py with every include directory of its compile record present.

The probe replays bsd_init.o's compile command and adds -Werror. That command names
build/xnu/ios-obj/EXPORT_HDRS/libsa, which XNU's build only creates when libsa exports a header; none does
in this configuration, and clang 24 turns -Wmissing-include-dirs into an error under -Werror. An empty
directory finds the same headers as a missing one, so it is created.

The record also carries -Weverything, which in clang 24 includes -Wunsafe-buffer-usage and -Wshadow-header:
style diagnostics Apple's toolchain does not emit for these sources. The probe runs with those two turned off
after its -Werror; nothing else in the script changes, and the substitution must match exactly once.
"""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json')
                    .read_text().strip().rstrip(','))
arguments = record['arguments']
for index, value in enumerate(arguments):
    path = arguments[index + 1] if value == '-I' else value[2:] if value.startswith('-I') else None
    if path:
        directory = Path(record['directory'], path)
        if not directory.exists():
            directory.mkdir(parents=True)
            print(f'created empty include directory {directory}')
probe = ROOT / 'scripts/build-verified-root-probe.py'
source = probe.read_text()
marker = "command += ['-I' + str(ROOT / 'kits/VerifiedRoot/include'), '-Werror']"
if source.count(marker) != 1:
    raise SystemExit('build-verified-root-probe.py changed: review the warning overrides')
source = source.replace(marker, marker[:-1] + ", '-Wno-unsafe-buffer-usage', '-Wno-shadow-header']")
sys.argv[0] = str(probe)
exec(compile(source, str(probe), 'exec'), {'__file__': str(probe), '__name__': '__main__'})
