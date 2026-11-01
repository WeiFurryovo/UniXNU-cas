#!/usr/bin/env python3
"""Build libBootProgress.a for CONFIG_XNU_BOOT_PROGRESS=y (build-xnu-4k-baseline.sh runs this by name).

The source is the self-contained observer of UniXNU-ishtar (kits/cas/BootProgressMin.c): it implements the
thirteen entry points of zzz-boot-progress.patch with nothing outside the kernel image, draws into the frame
buffer the video console reported, and redirects the console to that panel. That makes a hang visible: the
last stage, the console lines before it and an interrupt counter stay on screen. The full sheng monitor
reads counters of drivers this board does not build.

Adapted from UniXNU-ishtar scripts/build-boot-progress-min.py: the glyph table comes from the uefi-cas
checkout beside the framework, and the undefined-symbol gate checks against this board's previous kernel.
"""
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/boot-progress'
SOURCE = ROOT / 'kits/cas/BootProgressMin.c'
RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
AR = next((c for c in ('llvm-ar', 'ar') if shutil.which(c)), 'ar')
NM = next((c for c in ('llvm-nm', 'nm') if shutil.which(c)), 'nm')
# The tree sits at <workspace>/UniXNU/out/<board>/<profile>/<board>/tree; uefi-cas is a workspace sibling.
FONT_PATH = 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib/Font.h'
candidates = [Path(os.environ['UNIXNU_UEFI_ROOT'])] if os.environ.get('UNIXNU_UEFI_ROOT') else []
candidates += [parent / 'uefi-cas' for parent in ROOT.parents]
FONT = next((c / FONT_PATH for c in candidates if (c / FONT_PATH).is_file()), None)
if FONT is None:
    raise SystemExit('no uefi-cas checkout with ' + FONT_PATH + ' (set UNIXNU_UEFI_ROOT): the panel needs its 5x12 glyphs')

OUT.mkdir(parents=True, exist_ok=True)
record = json.loads(RECORD.read_text().strip().rstrip(','))
obj = OUT / 'BootProgressMin.o'
command = []
args = record['arguments']
index = 0
while index < len(args):
    arg = args[index]
    if arg == record['file']:
        command.append(str(SOURCE))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            command += ['-o', str(obj)]
        index += 1
    elif arg == '-Xclang' and args[index + 1].startswith('-W'):
        index += 1
    elif arg == '-D' and args[index + 1].startswith('XNU_BOOT_PROGRESS='):
        index += 1
    elif not arg.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
        command.append(arg)
    index += 1
command += ['-DXNU_BOOT_PROGRESS=1', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(FONT.parent)]
subprocess.run(command, cwd=record['directory'], check=True, timeout=120)
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')

# zzz-boot-progress.patch calls sheng_boot_watchdog_policy() after the owner Data mount (kern_exec.c). The sheng
# implementation is used unchanged; it does nothing without its sheng-* boot arguments. It is BSD code, so it
# takes kern_exec.o's flags.
policy_source = ROOT / 'kits/cas/BootWatchdogPolicy.c'
policy_obj = OUT / 'BootWatchdogPolicy.o'
policy_record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json')
                           .read_text().strip().rstrip(','))
policy_command = []
args = policy_record['arguments']
index = 0
while index < len(args):
    arg = args[index]
    if arg == policy_record['file']:
        policy_command.append(str(policy_source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            policy_command += ['-o', str(policy_obj)]
        index += 1
    elif arg == '-Xclang' and args[index + 1].startswith('-W'):
        index += 1
    elif not arg.startswith('-W'):
        policy_command.append(arg)
    index += 1
policy_command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror']
subprocess.run(policy_command, cwd=policy_record['directory'], check=True, timeout=120)

# The archive is force-loaded, so everything the object imports must already be in the kernel.
KERNEL = ROOT.parent / 'kernel/kernel.macho'
undefined = sorted({line.split()[-1] for o in (obj, policy_obj)
                    for line in subprocess.run([NM, '-u', str(o)], capture_output=True, text=True,
                                               check=True).stdout.splitlines() if line.strip()})
provided = {'memset', 'memcpy', 'memmove', '___stack_chk_fail', '___stack_chk_guard', '_physmap_base'}
if KERNEL.is_file():
    provided |= {line.split()[-1] for line in subprocess.run([NM, '--defined-only', str(KERNEL)], capture_output=True,
                                                               text=True, check=False).stdout.splitlines()
                 if len(line.split()) >= 3}
unexpected = [name for name in undefined if name not in provided]
if unexpected:
    raise SystemExit('BootProgressMin.o needs symbols this build does not provide: ' + ', '.join(unexpected))

archive = OUT / 'libBootProgress.a'
archive.unlink(missing_ok=True)
subprocess.run([AR, 'rcs', str(archive), str(obj), str(policy_obj)], check=True, timeout=30)
(OUT / 'build-report.json').write_text(json.dumps({
    'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
    'font_sha256': hashlib.sha256(FONT.read_bytes()).hexdigest(),
    'watchdog_policy_sha256': hashlib.sha256(policy_source.read_bytes()).hexdigest(),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'undefined_symbols': undefined,
}, indent=2) + '\n')
print(f'boot progress: {obj.stat().st_size} B observer and the watchdog policy archived, {len(undefined)} allowed undefined symbol(s)')
