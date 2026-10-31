#!/usr/bin/env python3
"""Sign and fully verify the OS and SystemOS windows of the built userdata image, on any host.

The framework's build-owner-os-policy.py does this for the OS only and reads the window through a macOS
hdiutil attachment. Here both windows are read straight from userdata-root.img, which holds them byte for byte
(scripts/unixnu/userdata.py), so the same signed manifests come out on Linux:

  build/verified-root-os/       manifest.bin, trusted-key.bin, original-samples.bin, verification.json
  build/verified-root-systemos/ manifest.bin, trusted-key.bin, verification.json

Both are signed with build/verified-root-host/keys/owner.pem, and every block is checked again through the
VerifiedRoot C verifier (build/verified-root-host/verified-root.dylib, any shared object format) before the
report claims it. build-verified-root-probe.py --component os --include-systemos consumes the two directories.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import uuid

ROOT = Path(__file__).resolve().parents[1]
WINDOWS = {
    # name: (volume UUID, image sha256, block count), the values build-verified-root-probe.py requires.
    'OS': ('04769D26-2CA0-4825-B443-E2E5F88300F2',
           'e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2', 1483785),
    'SystemOS': ('8846FE78-2910-4297-AF97-D59BB6F8FC16',
                 'b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f', 806400),
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--userdata', type=Path, required=True, help='Directory with userdata-root.img and userdata-root.json')
args = parser.parse_args()
report = json.loads((args.userdata / 'userdata-root.json').read_text())
image_path = args.userdata / 'userdata-root.img'
windows = {w['name']: w for w in report['windows']}
keys = ROOT / 'build/verified-root-host/keys'
spec = importlib.util.spec_from_file_location('verified_manifest', ROOT / 'kits/VerifiedRoot/tools/manifest.py')
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class Window:
    """A read-only stream over one window of the image, ending exactly at the window's end."""

    def __init__(self, stream, offset, size):
        self.stream, self.end = stream, offset + size
        stream.seek(offset)

    def read(self, count):
        return self.stream.read(max(0, min(count, self.end - self.stream.tell())))


class View(ctypes.Structure):
    _fields_ = [('hashes', ctypes.c_void_p), ('image_bytes', ctypes.c_uint64), ('block_count', ctypes.c_uint64),
                ('generation', ctypes.c_uint64), ('volume_uuid', ctypes.c_uint8 * 16), ('image_sha256', ctypes.c_uint8 * 32)]


native = ctypes.CDLL(str(ROOT / 'build/verified-root-host/verified-root.dylib'))
native.vr_host_open.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(View)]
native.vr_host_block.argtypes = [ctypes.POINTER(View), ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]

for name, (volume, expected_sha256, expected_blocks) in WINDOWS.items():
    window = windows[name]
    if window['sha256'] != expected_sha256:
        raise SystemExit(f'{name}: the userdata window hashes to {window["sha256"]}, not the original {expected_sha256}')
    out = ROOT / ('build/verified-root-os' if name == 'OS' else 'build/verified-root-systemos')
    with image_path.open('rb', buffering=1024 * 1024) as stream:
        record = manifest.make(Window(stream, window['offset'], window['size']), window['size'],
                               keys / 'owner.pem', keys / 'owner-public.pem', uuid.UUID(volume), 1, out)
    encoded = ctypes.create_string_buffer((out / 'manifest.bin').read_bytes())
    anchor = ctypes.create_string_buffer((out / 'trusted-key.bin').read_bytes())
    view = View()
    if native.vr_host_open(encoded, len(encoded) - 1, anchor, 1, ctypes.byref(view)) or bytes(view.volume_uuid) != uuid.UUID(volume).bytes:
        raise SystemExit(f'{name}: C signature/volume authentication failed')
    if view.block_count != expected_blocks:
        raise SystemExit(f'{name}: {view.block_count} blocks, expected {expected_blocks}')
    complete = hashlib.sha256()
    indices = [0, 1, view.block_count // 2, view.block_count - 1]
    samples, lengths = [], []
    with image_path.open('rb', buffering=1024 * 1024) as stream:
        reader = Window(stream, window['offset'], window['size'])
        for index in range(view.block_count):
            block = reader.read(4096)
            if native.vr_host_block(ctypes.byref(view), index, block, len(block)) != 0:
                raise SystemExit(f'{name}: C block verification failed at {index}')
            complete.update(block)
            if index in indices:
                samples.append(block.ljust(4096, b'\0'))
                lengths.append(len(block))
        if reader.read(1):
            raise SystemExit(f'{name}: window exceeded its authenticated bounds')
    if complete.digest() != bytes(view.image_sha256) or complete.hexdigest() != expected_sha256:
        raise SystemExit(f'{name}: complete digest differs')
    result = dict(image_bytes=view.image_bytes, blocks_verified=view.block_count, image_sha256=complete.hexdigest(),
                  volume_uuid=volume, generation=view.generation, source_format='userdata window',
                  source_offset=window['offset'], trusted_key_sha256=record['trusted_key_sha256'],
                  real_c_signature_and_block_verification=True, original_image_modified=False, host_only=True,
                  boot_root_authenticated=False, hardware_tested=False)
    if name == 'OS':
        (out / 'original-samples.bin').write_bytes(b''.join(samples))
        result.update(actual_sample_indices=indices, actual_sample_lengths=lengths)
    (out / 'verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS: {name} signature and all {view.block_count} blocks verified by the C verifier')
