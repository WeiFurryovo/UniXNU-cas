#!/usr/bin/env python3
"""build/adreno740/libAdreno740.a for cas: the Adreno 650 GMU path that CONFIG_XNU_NATIVE_ADRENO force-loads.

After UniXNU-alioth@f188378 scripts/build-adreno650.py. build-xnu-4k-baseline.sh links CONFIG_XNU_NATIVE_ADRENO from
build/adreno740/libAdreno740.a (the A740 name is hard-coded there), so the A650 archive is produced under that name.

What it carries (cas-gpu=1..12):
  - the kits ArmSmmu4K, AdrenoPM4 (AdrenoPM4.c) and AdrenoFirmware (the HFI transport and startup), selected in board.toml so
    the tree has them at kits/<name>;
  - NativeAdreno's GMU SMMU context and memory, and for modes 9-10 its GPU SMMU context, ring 0, CP init and A6xx interrupt mask
    (NativeAdrenoGpuContext/Ring/CpInit/Irq, as UniXNU-alioth@f188378's archive has them), from FFT-infra/Adreno650 (the A650 fork
    alioth runs; 70771a7 carries alioth 059b678's ring fix: CP_RB_CNTL without NO_UPDATE, the RPTR read from the CP's shadow), pinned at DRIVER_COMMIT and
    checked out into the tree's .cache (ADRENO650_DRIVER=<checkout> overrides; its HEAD must still be DRIVER_COMMIT); for mode 11
    also NativeAdrenoSubmit/Timer/UserClient/Publish (publication, user clients and the hangcheck; the probe defines the two
    weak_import symbols they need that alioth's board provides, native_gpu_dcvs and boot_log_export_put). Adreno650
    is not in UniXNU's catalogue and selecting Adreno740 would pull Sm8550Platform, so the recipe fetches it itself;
  - for mode 12 (UniXNU-alioth@f188378 scripts/build-adreno650.py:111-167, UniXNU scripts/build-adreno740-accelerator.py) the
    staged AGXAccelerator, NativeAdrenoAccelerator.cpp at the same commit: compiled against the original IOAcceleratorES base
    (AdrenoMetal's 11-line OriginalAcceleratorABI.h, written into build/ because the tree has no drivers/AdrenoMetal), its
    initializer taken out of __mod_init_func and exported (the probe runs it after IOGPUFamily has loaded), and its three
    IOAcceleratorES imports bound by absolute address (original-bindings.S) from UniXNU scripts/data/ioaccelerator-base-abi.json,
    whose addresses are qualified here against this board's kernel and its plan (ROOT.parent/kernel, as build-cas-touch.py
    qualifies the HID ABI): the pinned 21H461 donor, the complete original IOGPUFamily instructions, the four entry bodies, the
    168-entry vtable that differs from IOService only in slots 0, 1 and 7, the unique getMetaClass that loads the metaclass,
    and both constructors calling the genuine IOService C2. GpuProbe.cpp is compiled with -DCAS_GPU_ACCELERATOR=1;
  - the board's A650 cold start (kits/cas/a650/A650Gmu.c) and probe (kits/cas/GpuProbe.cpp, cas_gpu_probe);
  - GpuFirmwareA650.c, generated here from this phone's own vendor firmware: a650_gmu.bin (mode 7), a650_sqe.fw (mode 9) and
    a650_zap.elf (mode 10; the full ELF, whose segments sit at their program-header offsets, so the split a650_zap.mdt/.b00-.b02
    are not needed: the probe takes the metadata and the segment straight from it). The firmware never enters a repository: its directory
    comes from CAS_A650_FIRMWARE or the ignored local.toml of the device repository ([cas] a650_firmware = "<dir>"), every file is
    checked against the SHA-256 recorded in proprietary-files.txt (and below), and the C file is written only into build/.
"""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/adreno740'
OUT.mkdir(parents=True, exist_ok=True)

DRIVER_GIT = 'https://github.com/FFT-infra/Adreno650'
DRIVER_COMMIT = '70771a71795d7e1a78afdb82badd7943adb0a3a0'
# a650_* of the cas vendor partition (vendor/firmware), as listed in proprietary-files.txt.
FIRMWARE = {
    'gmu': ('a650_gmu.bin', 'df79fa8482fcc7587bb239b9c9e2b24bce4729893b990fe3f82b30da8175b01c'),
    'sqe': ('a650_sqe.fw', 'b4c27420fd6a5c6b511b254cc0fad8cdd40eea38ceb3418f895a1084f4460487'),
    'zap': ('a650_zap.elf', 'a3b05d17dcb0b4a7c9320df5a087a1f54717dc1774eba2ea9cd766464e71167e'),
}
DRIVER_OBJECTS = ['NativeAdrenoGmuContext', 'NativeAdrenoGmuMemory', 'NativeAdrenoSmmuContext', 'NativeAdrenoSmmuMemory',
                  'NativeAdrenoMemory', 'NativeAdrenoVm', 'NativeAdrenoCpu',
                  # Modes 9-10: the GPU's SMMU context and kernel buffers, ring 0, CP_ME_INIT and the A6xx RBBM interrupt mask.
                  'NativeAdrenoGpuContext', 'NativeAdrenoRing', 'NativeAdrenoCpInit', 'NativeAdrenoIrq',
                  # Mode 11: the submission object, the always-on timer, the user client and the publication (with its
                  # hangcheck), as UniXNU-alioth@f188378's archive has them. NativeAdrenoAccelerator (mode 12) is staged below.
                  'NativeAdrenoSubmit', 'NativeAdrenoTimer', 'NativeAdrenoUserClient', 'NativeAdrenoPublish']
# Mode 12: the original IOAcceleratorES base this board binds to (UniXNU scripts/data/ioaccelerator-base-abi.json, sha256 below),
# the donor it was audited on, and AdrenoMetal@6416a76's OriginalAcceleratorABI.h (MIT, NativeMetal contributors), reproduced
# byte for byte and checked by its SHA-256.
ABI_RECORD_SHA = '7d898b215006553b4de3820b0a29b4a330ddb5ec44fee77a3695bd6ea682a7be'
DONOR_SHA = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
BASE_HEADER = '''// SPDX-License-Identifier: MIT
#pragma once
// Exact supplied iOS17 base, independently audited against its complete
// original vtable and protected constructor/destructor bodies. No extra fields
// or operational overrides beyond IOService. This declares a real ancestry;
// no original class, method, vtable or metaclass is replaced.
#include <IOKit/IOService.h>
class IOAcceleratorES : public IOService {
 OSDeclareAbstractStructors(IOAcceleratorES)
};
static_assert(sizeof(IOService)==136 && sizeof(IOAcceleratorES)==136,"Qualified original accelerator base extent");
'''
BASE_HEADER_SHA = '211e922f9b760ddec2e3453db6584bac57597b3d39e4bf0d013c1b2debd53365'


def fail(why):
    sys.exit('build-adreno650: ' + why)


def run(cmd, **kw):
    return subprocess.run([str(x) for x in cmd], check=True, capture_output=True, text=True, **kw).stdout


# 1. The firmware, from outside every repository.
def firmware_directory():
    value = os.environ.get('CAS_A650_FIRMWARE')
    device = os.environ.get('UNIXNU_DEVICE')
    if not value:
        candidates = [Path(device)] if device else []
        candidates.append((ROOT / 'kits/cas').resolve().parent)
        for base in candidates:
            local = base / 'local.toml'
            if local.is_file():
                value = tomllib.loads(local.read_text()).get('cas', {}).get('a650_firmware')
                if value:
                    break
    if not value:
        fail('no A650 firmware: set CAS_A650_FIRMWARE or [cas] a650_firmware in the device repository\'s local.toml')
    path = Path(os.path.expandvars(value)).expanduser()
    if not path.is_dir():
        fail(f'{path} is not a directory')
    return path


source_dir = firmware_directory()
generated = OUT / 'GpuFirmwareA650.c'
lines = ['#include <stdint.h>', '',
         '// Generated by scripts/build-adreno650.py from the cas vendor partition\'s A650 firmware. Not a source file: it lives',
         '// only in the build tree and must never be committed. The symbols are what package/GpuProbe.cpp weak-imports.']
for name, (filename, digest) in FIRMWARE.items():
    data = (source_dir / filename).read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        fail(f'{filename}: SHA-256 differs from the recorded {digest}')
    lines.append(f'// {filename} sha256 {digest}')
    lines.append(f'const uint8_t native_gpu_{name}_firmware[] = {{')
    for at in range(0, len(data), 24):
        lines.append(','.join(str(b) for b in data[at:at + 24]) + ',')
    lines.append('};')
    lines.append(f'const uint32_t native_gpu_{name}_firmware_size = {len(data)};')
text = '\n'.join(lines) + '\n'
if not generated.is_file() or generated.read_text() != text:
    with generated.open('w') as stream:
        stream.write(text)
        stream.flush()
        os.fsync(stream.fileno())

# 2. The driver source at its pinned commit.
driver = os.environ.get('ADRENO650_DRIVER')
if driver:
    driver = Path(driver)
else:
    driver = ROOT / '.cache/sources/Adreno650'
    head = None
    if (driver / '.git').is_dir():
        head = subprocess.run(['git', '-C', str(driver), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    if head != DRIVER_COMMIT:
        driver.mkdir(parents=True, exist_ok=True)
        if not (driver / '.git').is_dir():
            run(['git', '-C', driver, 'init', '-q'])
            run(['git', '-C', driver, 'remote', 'add', 'origin', DRIVER_GIT])
        for attempt in range(3):
            try:
                run(['git', '-C', driver, 'fetch', '-q', '--depth', '1', 'origin', DRIVER_COMMIT])
                break
            except subprocess.CalledProcessError as error:
                if attempt == 2:
                    fail('fetching ' + DRIVER_GIT + ': ' + error.stderr)
        run(['git', '-C', driver, 'checkout', '-q', '--detach', 'FETCH_HEAD'])
if (driver / '.git').exists():
    head = run(['git', '-C', driver, 'rev-parse', 'HEAD']).strip()
    if head != DRIVER_COMMIT:
        fail(f'{driver} is at {head}, not the pinned {DRIVER_COMMIT}')

# 3. Sources.
KITS = ROOT / 'kits'
PACKAGE = ROOT / 'kits/cas'
for kit in ('ArmSmmu4K', 'AdrenoPM4', 'AdrenoFirmware'):
    if not (KITS / kit).is_dir():
        fail(f'kits/{kit} missing: select it in board.toml [components] include')
sources = sorted((KITS / 'ArmSmmu4K').glob('ArmSmmu4K*.c')) + [KITS / 'AdrenoPM4/AdrenoPM4.c']
sources += [KITS / 'AdrenoFirmware' / (name + '.c') for name in ('AdrenoFirmware', 'AdrenoHfi', 'AdrenoHfiStartup', 'AdrenoHfiVotes')]
sources += [driver / (name + '.cpp') for name in DRIVER_OBJECTS]
sources += [PACKAGE / 'a650/A650Gmu.c', PACKAGE / 'GpuProbe.cpp', generated]

recipes = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
c_recipe = json.loads((recipes / 'bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
cxx_recipe = json.loads((recipes / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
includes = []
for directory in (KITS / 'AdrenoPM4', KITS / 'ArmSmmu4K', KITS / 'AdrenoFirmware', PACKAGE / 'a650', PACKAGE, driver,
                  ROOT / '.cache/xnu-work/osfmk'):
    includes += ['-I', str(directory)]
objects = []
for source in sources:
    recipe = c_recipe if source.suffix == '.c' else cxx_recipe
    obj = OUT / (source.stem + ('.kit.o' if source.suffix == '.c' else '.o'))
    command, i, arguments = [], 0, recipe['arguments']
    while i < len(arguments):
        value = arguments[i]
        if value == recipe['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ'):
            i += 1
        elif value == '-Xclang' and arguments[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP') and not value.startswith('-DXNU_BOOT_PROGRESS='):
            command.append(value)
        i += 1
    if source.name == 'GpuProbe.cpp':
        command.append('-DCAS_GPU_ACCELERATOR=1')
    command += includes + ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                           '-Wno-incompatible-sysroot', '-Wno-c99-designator', '-Wno-missing-field-initializers',
                           '-Wno-missing-designated-field-initializers', '-Wno-reorder-init-list',
                           '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier']
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    if result.returncode:
        fail(str(source) + ':\n' + result.stderr[-4000:])
    objects.append(obj)

AR = shutil.which('llvm-ar') or 'ar'
NM = shutil.which('llvm-nm') or 'nm'
archive = OUT / 'libAdreno740.a'
archive.unlink(missing_ok=True)
run([AR, 'rcs', archive, *objects])

# 3b. Mode 12's staged accelerator. First the binding addresses, against this board's kernel.
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset, exports  # noqa: E402

record_path = ROOT / 'scripts/data/ioaccelerator-base-abi.json'
if hashlib.sha256(record_path.read_bytes()).hexdigest() != ABI_RECORD_SHA:
    fail(f'{record_path} is not the audited IOAcceleratorES record')
abi = json.loads(record_path.read_text())
if abi['operational_overrides'] or abi['instance_bytes'] != 136 or abi['virtual_entries'] != 168 or abi['metadata_overrides'] != [0, 1, 7]:
    fail('the audited IOAcceleratorES is not the 136-byte, 168-entry base without operational overrides')
reference = ROOT.parent / 'kernel/kernel.macho'
if not reference.is_file():
    fail(f'no previous kernel at {reference} to qualify the IOAcceleratorES addresses against')
kb = reference.read_bytes()
plan = json.loads(reference.with_suffix('.json').read_text())
km = macho(kb)
if hashlib.sha256(kb).hexdigest() != plan['candidate_sha256'] or plan['original_sha256'] != DONOR_SHA:
    fail('the reference kernel does not match its plan, or its donor is not the pinned 21H461 kernelcache')


def kread(address, size):
    at = backed_offset(km['segments'], address, size, len(kb))
    return kb[at:at + size]


def adrp(pc, word):
    imm = ((word >> 5) & 0x7ffff) * 4 + ((word >> 29) & 3)
    if imm & (1 << 20):
        imm -= 1 << 21
    return (pc & ~4095) + (imm << 12)


def branch(pc):
    word = struct.unpack('<I', kread(pc, 4))[0]
    if word & 0x7c000000 != 0x14000000:
        fail(f'no direct branch at {pc:#x}')
    imm = word & 0x3ffffff
    return pc + 4 * (imm - (1 << 26) if imm & (1 << 25) else imm)


def imported(stub):
    a, b, c = struct.unpack('<3I', kread(stub, 12))
    if a & 0x9f00001f != 0x90000010 or b & 0xffc003ff != 0xf9400210 or c != 0xd61f0200:
        fail(f'no import stub at {stub:#x}')
    return struct.unpack('<Q', kread(adrp(stub, a) + ((b >> 10) & 4095) * 8, 8))[0]


knames = {n: a for a, group in exports(kb, km).items() for n in group}
extents = {e['segment']: e for e in plan['extents'] if e['module'] == 'com.apple.iokit.IOGPUFamily'}
text = extents['__TEXT_EXEC']
if (plan['original_executable_sha256']['com.apple.iokit.IOGPUFamily'] != abi['original_iogpu_text_sha256'] or
        hashlib.sha256(kread(text['va'], text['size'])).hexdigest() != abi['original_iogpu_text_sha256']):
    fail('the original IOGPUFamily instructions in this kernel are not the audited ones')
for address, digest in abi['original_entry_sha256'].items():
    if hashlib.sha256(kread(int(address, 16), 32)).hexdigest() != digest:
        fail(f'original entry {address} differs from the audit')
vptr, metaclass = int(abi['vptr'], 16), int(abi['metaclass'], 16)
table = struct.unpack('<168Q', kread(vptr, 168 * 8))
base = struct.unpack('<168Q', kread(knames['__ZTV9IOService'] + 16, 168 * 8))
getter = int(abi['get_meta_class'], 16)
if ([i for i in range(168) if table[i] != base[i]] != [0, 1, 7] or kread(vptr + 168 * 8, 24) != bytes(24) or
        table[0] != int(abi['nondeleting_destructor'], 16) or table[7] != getter):
    fail('the original IOAcceleratorES vtable is not the audited one')
a, b, c = struct.unpack('<3I', kread(getter, 12))
if (a & 0x9f00001f != 0x90000000 or b & 0xffc003ff != 0x91000000 or c != 0xd65f03c0 or
        adrp(getter, a) + ((b >> 10) & 4095) != metaclass):
    fail('getMetaClass does not return the audited metaclass')
data = extents['__DATA']
if not data['va'] <= metaclass < data['va'] + data['size']:
    fail('the metaclass is outside IOGPUFamily __DATA')
for constructor in abi['protected_compatible_constructor_aliases']:
    start = int(constructor, 16)
    w = struct.unpack('<8I', kread(start, 32))
    if (w[0] != 0xa9bf7bfd or w[1] != 0x910003fd or w[6] != 0xa8c17bfd or w[7] != 0xd65f03c0 or
            imported(branch(start + 8)) != knames['__ZN9IOServiceC2EPK11OSMetaClass']):
        fail(f'constructor {constructor} does not only call IOService C2')
if imported(branch(table[0])) != knames['__ZN9IOServiceD2Ev']:
    fail('the destructor does not chain to IOService D2')
bindings = {'__ZN15IOAcceleratorESC2EPK11OSMetaClass': int(abi['protected_compatible_constructor_aliases'][0], 16),
            '__ZN15IOAcceleratorESD2Ev': int(abi['nondeleting_destructor'], 16),
            '__ZN15IOAcceleratorES10gMetaClassE': metaclass}

# Then the class itself, compiled with the kernel's C++ recipe against the original base header.
header_root = OUT / 'accelerator'
(header_root / 'include').mkdir(parents=True, exist_ok=True)
(header_root / 'AdrenoMetal').mkdir(parents=True, exist_ok=True)
base_header = header_root / 'AdrenoMetal/OriginalAcceleratorABI.h'
if hashlib.sha256(BASE_HEADER.encode()).hexdigest() != BASE_HEADER_SHA:
    fail('OriginalAcceleratorABI.h differs from AdrenoMetal@6416a76')
if not base_header.is_file() or base_header.read_text() != BASE_HEADER:
    with base_header.open('w') as stream:
        stream.write(BASE_HEADER)
        stream.flush()
        os.fsync(stream.fileno())
accel_source = driver / 'NativeAdrenoAccelerator.cpp'
accel_obj = OUT / 'NativeAdrenoAccelerator.o'
command, i, arguments = [], 0, cxx_recipe['arguments']
while i < len(arguments):
    value = arguments[i]
    if value == cxx_recipe['file']:
        command.append(str(accel_source))
    elif value == '-o':
        command += ['-o', str(accel_obj)]
        i += 1
    elif value in ('-MF', '-MJ'):
        i += 1
    elif value == '-Xclang' and arguments[i + 1].startswith('-W'):
        i += 1
    elif not value.startswith('-W') and value not in ('-MD', '-MP') and not value.startswith('-DXNU_BOOT_PROGRESS='):
        command.append(value)
    i += 1
# "../AdrenoMetal/OriginalAcceleratorABI.h" resolves through build/adreno740/accelerator/include/.. (the driver checkout has no sibling).
command += ['-I', str(header_root / 'include'), '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-overloaded-virtual', '-Wno-missing-designated-field-initializers',
            '-Wno-incompatible-sysroot']
result = subprocess.run(command, cwd=cxx_recipe['directory'], capture_output=True, text=True)
if result.returncode:
    fail(str(accel_source) + ':\n' + result.stderr[-4000:])
undefined_base = {line.split()[0] for line in run([NM, '-gP', accel_obj]).splitlines()
                  if len(line.split()) > 1 and line.split()[1] in 'Uw' and 'IOAcceleratorES' in line.split()[0]}
if undefined_base - set(bindings) or not undefined_base:
    fail('unexpected original-base imports: ' + ', '.join(sorted(undefined_base - set(bindings))))
OBJCOPY = shutil.which('llvm-objcopy')
if not OBJCOPY:
    fail('llvm-objcopy not found (the accelerator needs it)')
late = OUT / 'NativeAdrenoAccelerator.late.o'
run([OBJCOPY, '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp',
     accel_obj, late])


def text_of(path):
    raw = path.read_bytes()
    section = next(s for g in macho(raw)['segments'] for s in g['sections'] if s['name'] == '__text')
    return raw[section['fileoff']:section['fileoff'] + section['size']]


if text_of(accel_obj) != text_of(late):
    fail('late staging changed the accelerator\'s code')
assembly = OUT / 'original-bindings.S'
assembly.write_text(''.join(f'.globl {n}\n.set {n}, {a:#x}\n' for n, a in sorted(bindings.items()) if n in undefined_base))
bind_obj = OUT / 'original-bindings.o'
run([cxx_recipe['arguments'][0], '-c', '--target=arm64-apple-ios17.0', '-Wno-incompatible-sysroot', '-o', bind_obj, assembly])
run([AR, 'rcs', archive, late, bind_obj])

# 4. Every NativeAdreno or kit symbol the objects use must be defined by them; the rest is the kernel's.
nm = run([NM, '-gP', archive]).splitlines()
defined = {f.split()[0] for f in nm if len(f.split()) > 1 and f.split()[1] in 'TSDBCRV'}
undefined = {f.split()[0] for f in nm if len(f.split()) > 1 and f.split()[1] in 'Uw'}
# The GPU addon's counters (native_gpu_addon_*) are the kernel's own (patches/xnu/zzy-native-gpu-addon-mount.patch, kern_exec.c).
missing = sorted(s for s in undefined - defined
                 if re.search(r'NativeAdreno|native_adreno|ArmSmmu4K|AdrenoPM4|AdrenoHfi|AdrenoFirmware|A650Gmu|native_gpu_', s)
                 and not s.startswith('_native_gpu_addon_'))
if missing:
    fail('symbols used but not built here: ' + ', '.join(missing))
required = ('_cas_gpu_probe', '_native_gpu_short', '_NativeAdrenoGmuContextBringupRoutes', '_NativeAdrenoGmuContextPayloads',
            '_A650GmuColdStart', '_A650GmuBuildStartup', '_A650GmuPowerGx', '_native_gpu_gmu_firmware', '_native_gpu_gmu_firmware_size',
            '_NativeAdrenoGpuContextBringupRoutes', '_NativeAdrenoGpuContextBuffers', '_native_gpu_sqe_firmware',
            '_native_gpu_sqe_firmware_size', '_native_gpu_zap_firmware', '_native_gpu_zap_firmware_size',
            '_NativeAdrenoGpuContextVm', '_native_adreno_publish', '_native_adreno_set_restart', '_native_adreno_close_step_set',
            '_native_gpu_dcvs', '_boot_log_export_put', '_native_adreno_accelerator_allocate',
            '__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp')
absent = [s for s in required if s not in defined]
if absent:
    fail('required definitions absent: ' + ', '.join(absent))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


(OUT / 'build-report.json').write_text(json.dumps(dict(
    driver=DRIVER_GIT, driver_commit=DRIVER_COMMIT,
    firmware={filename: digest for filename, digest in FIRMWARE.values()},
    sources={str(s.relative_to(ROOT)) if s.is_relative_to(ROOT) else str(s): sha(s) for s in sources if s != generated},
    accelerator=dict(source_sha256=sha(accel_source), base_header_sha256=BASE_HEADER_SHA, abi_record_sha256=ABI_RECORD_SHA,
                     reference_kernel_sha256=plan['candidate_sha256'], bindings={n: hex(a) for n, a in bindings.items()}),
    archive_sha256=sha(archive)), indent=2) + '\n')
print(f'PASS: {len(objects)} objects archived in {archive.relative_to(ROOT)} (driver {DRIVER_COMMIT[:7]}, firmware checked); '
      'staged accelerator bound to ' + ', '.join(f'{a:#x}' for _, a in sorted(bindings.items())))
