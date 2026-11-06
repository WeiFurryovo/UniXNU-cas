#!/usr/bin/env python3
"""Build UniXNU's Adreno Metal plugin and pack it as a GPU addon, on a macOS host (.github/workflows/gpu-plugin.yml).

  build-gpu-plugin.py UNIXNU OUT

The same steps as UniXNU scripts/build-boot-runtime.py build_plugin('ios') and its make-gpu-addon.py call (--publish
--no-daemon), without the IPSW mount that function sits behind: Mesa at its pin with the recorded patches, metal2vulkan at
its pin, scripts/build-turnip-darwin.py --target ios, scripts/build-adreno-metal-plugin.py --target ios --no-replay,
scripts/make-gpu-addon.py. OUT receives gpuaddon.bin, trust.json, the bundle's gpu-build.json and checks.json (the CDHashes
of the addon's executables, verified with codesign --verify --strict, against trust.json).

Mesa patches: turnip-frame-diagnostics.patch adds tu_frame_diagnostic.h with "--- /dev/null" but no "new file mode" line,
which git apply (and the git apply --reverse of mesa_patch_identity.py) reads as a change of a file named dev/null. The
missing line is added to such patches in UNIXNU's tree before anything reads them; the source they produce is unchanged.
A patch git apply still refuses is applied with patch -p1 (no backup files, which the patch guard would call unrecorded)."""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(sys.argv[1]).resolve()
OUT = Path(sys.argv[2]).resolve()
OUT.mkdir(parents=True, exist_ok=True)


def run(*command, **kwargs):
    print('+', ' '.join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def normalize_new_files(patch):
    text = patch.read_text()
    fixed = re.sub(r'^(diff --git a/(\S+) b/\2\n)(--- /dev/null\n)', r'\1new file mode 100644\n\3', text, flags=re.M)
    if fixed != text:
        patch.write_text(fixed)
        print(f'{patch.relative_to(ROOT)}: added the missing "new file mode" header line', flush=True)


def checkout(path, pin):
    # build-boot-runtime.py checkout(): depth 1 at the pinned commit, the mirror first and the repository after it.
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)
    run('git', 'init', '-q', path)
    for url in [pin.get('mirror'), pin['repository']]:
        if url and subprocess.run(['git', '-C', str(path), 'fetch', '--depth=1', url, pin['commit']]).returncode == 0:
            break
    else:
        raise SystemExit(f'cannot fetch {pin["commit"]} from {pin["repository"]}')
    run('git', '-C', path, 'checkout', '-q', '--detach', 'FETCH_HEAD')


pins = json.loads((ROOT / 'scripts/data/adreno-compiler-inputs.json').read_text())
for patch in pins['patches']:
    normalize_new_files(ROOT / patch)
mesa = ROOT / '.cache/mesa-work'
checkout(mesa, pins['mesa'])
for patch in pins['patches']:
    if subprocess.run(['git', '-C', str(mesa), 'apply', str(ROOT / patch)]).returncode:
        print(f'git apply refused {patch}; applying it with patch -p1', flush=True)
        run('patch', '-p1', '--forward', '--no-backup-if-mismatch', '-i', ROOT / patch, cwd=mesa)
sys.path.insert(0, str(ROOT / 'scripts'))
from mesa_patch_identity import validate_recorded_patches
validate_recorded_patches(ROOT, mesa, pins)
print('Mesa source: pinned revision plus the recorded patches', flush=True)

translator = json.loads((ROOT / 'scripts/data/gpu-metal-vulkan-inputs.json').read_text())['translator']
checkout(ROOT / '.cache/gpu-reference/metal2vulkan', translator)
venv = ROOT / '.cache/mesa-venv'
if not (venv / 'bin/python').is_file():
    run(sys.executable, '-m', 'venv', venv)
run(venv / 'bin/python', '-m', 'pip', 'install', '-q', '-r', ROOT / 'scripts/requirements-adreno-compiler.txt')
run(sys.executable, ROOT / 'scripts/build-turnip-darwin.py', '--target', 'ios')
run(sys.executable, ROOT / 'scripts/build-adreno-metal-plugin.py', '--target', 'ios', '--no-replay')

plugin = ROOT / 'build/adreno-metal-plugin'
addon = OUT / 'gpuaddon.bin'
addon.unlink(missing_ok=True)
run(sys.executable, ROOT / 'scripts/make-gpu-addon.py', '--publish', '--no-daemon', addon,
    env=dict(os.environ, ADRENO_PLUGIN_DIR=str(plugin)))
shutil.copyfile(plugin / 'trust.json', OUT / 'trust.json')
shutil.copyfile(plugin / 'NativeAdrenoMetal.bundle/gpu-build.json', OUT / 'gpu-build.json')

# The check build-boot-runtime.py makes of an addon before it trusts it: every executable signed and strictly valid.
spec = importlib.util.spec_from_file_location('build_boot_runtime', ROOT / 'scripts/build-boot-runtime.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
work = ROOT / 'build/gpu-addon-check'
shutil.rmtree(work, ignore_errors=True)
work.mkdir(parents=True)
hashes = runtime.addon_hashes(addon, work)
trust = json.loads((OUT / 'trust.json').read_text())
expected = sorted({trust['cdhash'], trust['turnip_cdhash']})
checks = dict(addon_executable_cdhashes=hashes, trust_cdhashes=expected, daemon_cdhash=trust['daemon_cdhash'],
              match=hashes == expected)
(OUT / 'checks.json').write_text(json.dumps(checks, indent=2) + '\n')
print(json.dumps(checks, indent=2))
if hashes != expected:
    raise SystemExit('The addon executables do not have the CDHashes trust.json lists')
print('PASS: GPU addon', addon, addon.stat().st_size, 'bytes')
