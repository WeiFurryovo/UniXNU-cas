#!/usr/bin/env python3
"""Bind a block driver's original IOStorageFamily imports to their absolute addresses.

UniXNU-alioth names this step but does not publish it. This version derives every value statically, the way
UniXNU-umi's audit_umi_storage.py does, from two inputs: the public Darwin 23 IOBlockStorageDevice.cpp compiled
with this tree's flags (the 190-slot dispatch shape), and this board's previous prelinked kernel, which carries
the original IOStorageFamily unchanged. No live observation is made, so the result is static only.

IOMedia::metaClass is derived as well: the IOMedia OSMetaClass constructor call in the original text names the
gMetaClass object, and exactly one pointer to it in the original __DATA_CONST is the metaClass variable.

The binding object is added to the archive the component recipe produced (it must run after that recipe).
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--component', required=True, help='Compiled object name, e.g. NativeUfs')
parser.add_argument('--archive', required=True, help='Archive name in --directory, e.g. libShengUfs.a')
parser.add_argument('--directory', required=True, type=Path)
parser.add_argument('--include-object', action='append', default=[], type=Path,
                    help='Another storage driver object whose imports the same binding object must cover. Two force-loaded '
                         'archives cannot both define the absolute names, so one carries the union for every driver')
parser.add_argument('--verify-only', action='store_true',
                    help='Check the object and record its bindings, but add nothing to the archive (another archive binds them)')
args = parser.parse_args()
directory = (ROOT / args.directory).resolve()
out = ROOT / 'build/cas-storage-abi'
out.mkdir(parents=True, exist_ok=True)
ORIGINAL_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'


def require(value, message):
    if not value:
        raise SystemExit('storage bindings: ' + message)


def symbols_of(data):
    image = np.macho(data)
    at = next(offset for command, offset, size in image['commands'] if command == 2)
    symoff, count, stroff, _ = struct.unpack_from('<4I', data, at + 8)
    rows = []
    for i in range(count):
        ix, kind, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + i * 16)
        rows.append((data[stroff + ix:data.index(b'\0', stroff + ix)].decode(), value, kind))
    return image, rows


def vtable_slots(data, image, rows, name):
    """Names bound to the slots of vtable `name` in an object file, from its relocations."""
    vtable = next(value for symbol, value, kind in rows if symbol == name and kind & 0xe == 0xe)
    relocations = {}
    for segment in image['segments']:
        for section in segment['sections']:
            if section['va'] <= vtable < section['va'] + section['size']:
                ro, rc = struct.unpack_from('<II', data, section['command_offset'] + 56)
                for i in range(rc):
                    address, word = struct.unpack_from('<II', data, ro + i * 8)
                    if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                        relocations[section['va'] + address] = rows[word & 0xffffff][0]
    slots = []
    at = vtable + 16
    while at in relocations:
        slots.append(relocations[at])
        at += 8
    return slots


# 1. The public class shape, compiled with this tree's own flags.
source = ROOT / 'refer/IOStorageFamily'
revision = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
require(revision == 'e45fbe30a7879e0b16e26dddb78538f1b00e0bff', 'unexpected public Darwin 23 IOStorageFamily source')
headers = out / 'include/IOKit/storage'
headers.mkdir(parents=True, exist_ok=True)
for path in source.glob('*.h'):
    shutil.copyfile(path, headers / path.name)
# The platform-selection header the kernel SDK lacks, as scripts/prepare-storage-headers.py staged it.
conditions = ROOT / 'build/storage-abi/include/TargetConditionals.h'
require(conditions.is_file(), 'run scripts/prepare-storage-headers.py first (no staged TargetConditionals.h)')
shutil.copyfile(conditions, out / 'include/TargetConditionals.h')
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                    .read_text().strip().rstrip(','))
reference_obj = out / 'IOBlockStorageDevice-reference.o'
command, index, arguments = [], 0, record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source / 'IOBlockStorageDevice.cpp'))
    elif value == '-o':
        command += ['-o', str(reference_obj)]
        index += 1
    elif value in ('-MF', '-MJ'):
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W') and value not in ('-MD', '-MP'):
        command.append(value)
    index += 1
command += ['-I', str(out / 'include'), '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-w']
result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
(out / 'compile.log').write_text(result.stdout + result.stderr)
require(result.returncode == 0, 'reference compile failed:\n' + (result.stdout + result.stderr)[-3000:])
reference = reference_obj.read_bytes()
ref_image, ref_rows = symbols_of(reference)
methods = vtable_slots(reference, ref_image, ref_rows, '__ZTV20IOBlockStorageDevice')
require(len(methods) == 190 and methods.count('___cxa_pure_virtual') == 14, 'public iOS class dispatch shape changed')

# 2. The original class in this board's previous prelinked kernel.
kernel_path = ROOT.parent / 'kernel/kernel.macho'
require(kernel_path.is_file(), 'no previous prelinked kernel at ' + str(kernel_path) + '; build the kernel once without this step')
kernel = kernel_path.read_bytes()
plan = json.loads(kernel_path.with_suffix('.json').read_text())
require(hashlib.sha256(kernel).hexdigest() == plan['candidate_sha256'] and plan['original_sha256'] == ORIGINAL_SHA256,
        'unexpected candidate provenance')
native = np.macho(kernel)
exports = np.exports(kernel, native)
extents = [e for e in plan['extents'] if e['module'] == 'com.apple.iokit.IOStorageFamily']
# A kernel linked with an earlier run of this step exports the absolute names it bound, all of which point
# into the original IOStorageFamily; only XNU's own exports are evidence, so those are dropped.
exports = {va: names for va, names in exports.items()
           if not any(e['va'] <= va < e['va'] + e['size'] for e in extents)}
by_name = {name: va for va, names in exports.items() for name in names}
segment = {e['segment']: e for e in extents}
text, const = segment['__TEXT_EXEC'], segment['__DATA_CONST']


def read(va, length):
    at = np.backed_offset(native['segments'], va, length, len(kernel))
    return kernel[at:at + length]


require(hashlib.sha256(read(text['va'], text['size'])).hexdigest() ==
        plan['original_executable_sha256']['com.apple.iokit.IOStorageFamily'], 'original storage executable changed')
blob = read(const['va'], const['size'])
found = []
for at in range(0, len(blob) - (len(methods) + 2) * 8 + 1, 8):
    if blob[at:at + 16] != bytes(16):
        continue
    pointers = struct.unpack_from('<' + 'Q' * len(methods), blob, at + 16)
    if all(pointers[i] == by_name[name] for i, name in enumerate(methods) if name in by_name):
        found.append((const['va'] + at, pointers))
require(len(found) == 1, 'cannot uniquely identify the original block-storage dispatch')
table, pointers = found[0]
require(sum(name in by_name for name in methods) == 176, 'unexpected original/native kernel binding count')
for i, name in enumerate(methods):
    if name not in by_name:
        require(text['va'] <= pointers[i] < text['va'] + text['size'], 'owned storage method outside original text')


def adrp_target(address, word):
    immediate = ((word >> 5 & 0x7ffff) << 2) | (word >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    return (address & ~4095) + immediate * 4096


def relative_branch(address, instruction):
    immediate = instruction & 0x3ffffff
    if immediate & 0x2000000:
        immediate -= 0x4000000
    return address + immediate * 4


def thunk_names(address):
    a, load, branch = struct.unpack('<3I', read(address, 12))
    require(a & 0x9f00001f == 0x90000010 and load & 0xffc003ff == 0xf9400210 and branch == 0xd61f0200,
            'unqualified original native import thunk')
    slot = adrp_target(address, a) + (load >> 10 & 4095) * 8
    return exports.get(struct.unpack('<Q', read(slot, 8))[0], [])


constructors = []
for address in range(text['va'], text['va'] + text['size'] - 32, 4):
    words = struct.unpack('<8I', read(address, 32))
    if (words[:2] != (0xa9bf7bfd, 0x910003fd) or words[2] & 0xfc000000 != 0x94000000 or
            words[3] & 0x9f00001f != 0x90000008 or words[4] & 0xffc003ff != 0x91000108 or
            words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0)):
        continue
    if adrp_target(address + 12, words[3]) + (words[4] >> 10 & 4095) == table + 16:
        constructors.append((address, relative_branch(address + 8, words[2])))
require(len(constructors) == 1 and '__ZN9IOServiceC2EPK11OSMetaClass' in thunk_names(constructors[0][1]),
        'original protected block constructor does not forward to IOService')
destructor = pointers[0] - 4
word = struct.unpack('<I', read(destructor, 4))[0]
require(word & 0xfc000000 == 0x14000000 and '__ZN9IOServiceD2Ev' in thunk_names(relative_branch(destructor, word)),
        'unqualified original base destructor')
require(all(read(pointers[i], 4) == struct.pack('<I', 0xd4200020) for i in (0, 1)),
        'unexpected original abstract complete-destructor behaviour')
a, add, ret = struct.unpack('<3I', read(pointers[7], 12))
require(a & 0x9f00001f == 0x90000000 and add & 0xffc003ff == 0x91000000 and ret == 0xd65f03c0,
        'unqualified original storage metaclass getter')
block_meta = adrp_target(pointers[7], a) + (add >> 10 & 4095)

# 3. IOMedia::gMetaClass from its OSMetaClass constructor call, then the one const pointer to it.
cstrings = []
for name, e in segment.items():
    if name == '__TEXT':
        data = read(e['va'], e['size'])
        cstrings += [e['va'] + m.start() + 1 for m in re.finditer(rb'\0IOMedia\0', data)]
require(len(cstrings) == 1, f'expected one "IOMedia" string, found {len(cstrings)}')
code = read(text['va'], text['size'])
words = struct.unpack('<' + 'I' * (len(code) // 4), code)


def adrp_add(i, register):
    """Value an adrp/add pair at instruction i computes into `register`, or None."""
    a, add = words[i], words[i + 1]
    if a & 0x9f00001f != 0x90000000 | register or add & 0xffc003ff != 0x91000000 | register << 5 | register:
        return None
    return adrp_target(text['va'] + i * 4, a) + (add >> 10 & 4095)


media_meta = set()
for i in range(2, len(words) - 1):
    if adrp_add(i, 1) != cstrings[0]:
        continue
    # The static initializer loads this = &gMetaClass right before the name. IOMedia::MetaClass's own
    # constructors also load the name, but take `this` from their caller and set no x0 there.
    value = adrp_add(i - 2, 0)
    if value is not None:
        media_meta.add(value)
require(len(media_meta) == 1, f'expected one IOMedia gMetaClass, found {sorted(map(hex, media_meta))}')
media_meta = media_meta.pop()
holders = [const['va'] + at for at in range(0, len(blob) - 7, 8) if struct.unpack_from('<Q', blob, at)[0] == media_meta]
require(len(holders) == 1, f'expected one IOMedia::metaClass pointer, found {len(holders)}')

possible = {name: pointers[i] for i, name in enumerate(methods) if name not in by_name}
possible.update({'__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': constructors[0][0],
                 '__ZN20IOBlockStorageDeviceD2Ev': destructor,
                 '__ZN20IOBlockStorageDevice10gMetaClassE': block_meta,
                 '__ZTV20IOBlockStorageDevice': table,
                 '__ZN7IOMedia9metaClassE': holders[0]})

# 4. The driver objects: every original storage import must be one of the derived values, and every
#    IOBlockStorageDevice subclass must dispatch through the original 190-slot shape.
def check_object(path):
    data = path.read_bytes()
    image, rows = symbols_of(data)
    imports = [name for name, value, kind in rows if kind == 1 and ('IOBlockStorageDevice' in name or 'IOMedia' in name)]
    require(all(name in possible for name in imports), f'{path.name}: unqualified storage import: ' +
            ', '.join(name for name in imports if name not in possible))
    require(not any(re.search(r'IOBlockStorageDeviceD[01]Ev$', name) for name in imports),
            f'{path.name}: driver must not invoke abstract complete/deleting destruction')
    subclasses = []
    for name, value, kind in rows:
        if not name.startswith('__ZTV') or kind & 0xe != 0xe:
            continue
        slots = vtable_slots(data, image, rows, name)
        if not any('IOBlockStorageDevice' in slot for slot in slots):
            continue
        mangled = name[len('__ZTV'):]
        require(len(slots) == 190 and '___cxa_pure_virtual' not in slots, f'{name}: wrong dispatch shape')
        for slot, (actual, expected) in enumerate(zip(slots, methods)):
            own = actual.startswith(('__ZN' + mangled, '__ZNK' + mangled))
            if expected == '___cxa_pure_virtual':
                require(own, f'{name}: transport operation {slot} not implemented')
            elif actual != expected and not own:
                raise SystemExit(f'storage bindings: {name}: dispatch mismatch at slot {slot}')
        subclasses.append(name)
    require(subclasses, 'no IOBlockStorageDevice subclass in ' + path.name)
    return data, imports, subclasses


obj_path = directory / (args.component + '.o')
obj, undefined, checked = check_object(obj_path)
for extra in args.include_object:
    _, imports, subclasses = check_object((ROOT / extra).resolve())
    undefined = list(dict.fromkeys(undefined + imports))
    checked += subclasses

bindings = {name: possible[name] for name in undefined}
assembly = directory / 'original-bindings.S'
assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n' for name, value in sorted(bindings.items())))
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror', '-Wno-incompatible-sysroot',
                '-c', assembly, '-o', directory / 'original-bindings.o'], check=True)
archive = directory / args.archive
require(archive.is_file(), 'component archive missing: ' + str(archive))
if not args.verify_only:
    subprocess.run(['xcrun', 'ar', 'rcs', archive, directory / 'original-bindings.o'], check=True)
# build-xnu-4k-baseline.sh cross-checks the RAM and Virtio transports through bindings.json beside each
# archive and the object named after the archive (libRamBlock.a -> RamBlock.o).
shutil.copyfile(obj_path, archive.with_name(archive.stem.removeprefix('lib') + '.o'))
record = dict(candidate_sha256=plan['candidate_sha256'], compiled_object_sha256=hashlib.sha256(obj).hexdigest(),
              subclasses=checked, bindings={name: hex(value) for name, value in sorted(bindings.items())},
              archived=not args.verify_only, derivation='static', live_qualified=False)
for name in ('storage-bindings.json', 'bindings.json'):
    (directory / name).write_text(json.dumps(record, indent=2) + '\n')
print(f'storage bindings: {len(bindings)} original imports bound, {len(checked)} subclasses checked (static derivation)'
      + ('; verify only, nothing archived' if args.verify_only else ''))
