#!/usr/bin/env python3
"""Build the public gpu019 plugin on macOS; never read firmware or device inputs.

  build-gpu-plugin.py UNIXNU OUT [--inputs scripts/ci/patches/inputs.json]
  build-gpu-plugin.py --self-test

inputs.json schema 1 contains framework_commit and exactly two patches. Each patch
records repository, base_commit, patch (a basename), sha256 and files, mapping each
changed path to before_sha256 (null only for a new file) and after_sha256. Only
ordinary allowlisted files are accepted. No fuzz, reverse, deletion or renames.
Mesa's existing recorded patch compatibility workaround is deliberately separate
from these exact patches. The original translator patch guard is retained.

provenance.json, not the legacy gpu-build.json, binds the LLVM IR, AIR, GOP headers,
compiled sources, toolchain, tests and signed deliverables. Pixel SKIP is not PASS.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import zlib

FRAMEWORK = 'e2fdae4312d0acc3d8a52bc457101e7918095e6a'
PINS = {
    'drivers/AdrenoMetal': '6416a76e0a287d85afbcddda21192388460e155c',
    'drivers/GopFramebuffer': 'e47c1b8de169cd2bd0b885c7d85e433010e3cdf2',
    'drivers/Adreno740': '6fa7bae2a5b3b523e790eb3e0348af4d98969e4d',
    'kits/AdrenoPM4': '282606b528b2595bc4274ea7490b62b323f35a5f',
}
PATCH_NAMES = {'drivers/AdrenoMetal': 'adreno-metal-b3a8.patch',
               'drivers/GopFramebuffer': 'gop-wire-b3a8.patch'}
ALLOWED = {
    'drivers/AdrenoMetal': {
        'GpuScanoutClient.h', 'GpuScanoutPresenter.h', 'GpuScanoutLayout.h',
        'NativeBridgeMetalFrontend.mm', 'shaders/scanout_convert.ll',
        'bridge/scanout_layout_host.cpp', 'bridge/scanout_client_host.mm',
        'bridge/NativeMetalObjects.rs', 'bridge/NativeMetalObjects.h',
        'bridge/check_scanout_b3a8.py', 'bridge/scanout_host.mm', 'bridge/present_host.mm',
        'README.md',
    },
    'drivers/GopFramebuffer': {'NativeGOPWire.h'},
}
REQUIRED = {'drivers/AdrenoMetal': {'GpuScanoutClient.h', 'GpuScanoutPresenter.h',
                                  'shaders/scanout_convert.ll', 'bridge/check_scanout_b3a8.py',
                                  'bridge/scanout_host.mm', 'bridge/present_host.mm'},
            'drivers/GopFramebuffer': {'NativeGOPWire.h'}}
SHA256 = re.compile(r'[0-9a-f]{64}\Z')
MACHO = {b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xfe\xed\xfa\xcf', b'\xbe\xba\xfe\xca'}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(*command, **kwargs):
    print('+', ' '.join(str(part) for part in command), flush=True)
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def output(*command, **kwargs):
    return run(*command, capture_output=True, text=True, **kwargs).stdout.strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def relative_path(value):
    require(isinstance(value, str) and value and '\\' not in value, 'invalid relative path')
    path = PurePosixPath(value)
    require(not path.is_absolute() and path.as_posix() == value and
            all(p not in ('.', '..', '.git') for p in path.parts) and
            re.fullmatch(r'[A-Za-z0-9_./+-]+', value), f'unsafe path: {value!r}')
    return path


def safe_path(root, name, *, absent=False):
    parts = relative_path(name).parts
    path = root
    require(root.is_dir() and not root.is_symlink(), f'unsafe root: {root}')
    for part in parts:
        path = path / part
        require(not path.is_symlink(), f'symlink refused: {path}')
    if not absent or path.exists():
        require(path.is_file() and stat.S_ISREG(path.stat().st_mode), f'not a regular file: {path}')
    require(path.resolve().is_relative_to(root.resolve()), f'path escapes root: {name}')
    return path


def strict_json(path):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, f'duplicate JSON key: {key}')
            result[key] = value
        return result
    return json.loads(path.read_text(), object_pairs_hook=pairs)


def load_inputs(path):
    data = strict_json(path)
    require(set(data) == {'schema_version', 'framework_commit', 'patches'}, 'invalid inputs keys')
    require(type(data['schema_version']) is int and data['schema_version'] == 1, 'unsupported inputs schema')
    require(data['framework_commit'] == FRAMEWORK, 'framework pin mismatch')
    require(isinstance(data['patches'], list) and len(data['patches']) == 2, 'exactly two patches required')
    seen = set()
    for entry in data['patches']:
        require(set(entry) == {'repository', 'base_commit', 'patch', 'sha256', 'files'}, 'invalid patch keys')
        repo = entry['repository']
        require(repo in PATCH_NAMES and repo not in seen, 'unexpected or duplicate patch repository')
        seen.add(repo)
        require(entry['base_commit'] == PINS[repo] and entry['patch'] == PATCH_NAMES[repo], 'patch pin/name mismatch')
        require(isinstance(entry['sha256'], str) and SHA256.fullmatch(entry['sha256']), 'invalid patch hash')
        files = entry['files']
        require(isinstance(files, dict) and REQUIRED[repo] <= set(files) <= ALLOWED[repo], 'unexpected/missing patched paths')
        for name, hashes in files.items():
            relative_path(name)
            require(set(hashes) == {'before_sha256', 'after_sha256'}, 'invalid file hash keys')
            for key, value in hashes.items():
                require((key == 'before_sha256' and value is None) or
                        (isinstance(value, str) and SHA256.fullmatch(value)), f'invalid hash: {name}')
    return data


def check_patch_text(text, files):
    headers = re.findall(r'^diff --git a/(\S+) b/(\S+)$', text, re.M)
    require(headers and len(headers) == len(files) and
            all(a == b for a, b in headers) and {a for a, _ in headers} == set(files),
            'patch paths differ from manifest (quoted paths/renames refused)')
    require(not re.search(r'^(?:GIT binary patch|Binary files |deleted file mode |old mode |new mode |rename |copy |similarity index )', text, re.M),
            'binary, deleted, renamed or mode-changing patch refused')
    require(all(mode == '100644' for mode in re.findall(r'^new file mode (\d+)$', text, re.M)), 'non-regular new file refused')
    for line in text.splitlines():
        if line.startswith(('--- ', '+++ ')):
            name = line[4:]
            require(name == '/dev/null' or (name[:2] in ('a/', 'b/') and name[2:] in files), 'unexpected patch file header')
            require(not (line.startswith('+++ ') and name == '/dev/null'), 'deletion refused')


def apply_patch(root, inputs_dir, entry):
    repo = root / entry['repository']
    require(repo.is_dir() and not repo.is_symlink(), 'invalid repository directory')
    require(output('git', '-C', repo, 'rev-parse', 'HEAD') == entry['base_commit'], 'base revision differs')
    require(not output('git', '-C', repo, 'status', '--porcelain', '--untracked-files=all'), 'patch repository is not clean')
    patch = safe_path(inputs_dir, entry['patch'])
    require(digest(patch) == entry['sha256'], 'patch SHA256 differs')
    check_patch_text(patch.read_text(), entry['files'])
    for name, hashes in entry['files'].items():
        path = safe_path(repo, name, absent=hashes['before_sha256'] is None)
        if hashes['before_sha256'] is None:
            require(not path.exists(), f'new path already exists: {name}')
        else:
            require(digest(path) == hashes['before_sha256'], f'base file hash differs: {name}')
    # --index makes the clean checked-out base and index agree; no patch(1) fallback here.
    run('git', '-C', repo, 'apply', '--check', '--index', '--whitespace=error-all', patch)
    run('git', '-C', repo, 'apply', '--index', '--whitespace=error-all', patch)
    changed = output('git', '-C', repo, 'diff', '--cached', '--name-only', '--no-renames').splitlines()
    require(set(changed) == set(entry['files']) and len(changed) == len(entry['files']), 'actual patch paths differ')
    modes = output('git', '-C', repo, 'ls-files', '--stage', '--', *changed).splitlines()
    require(all(line.startswith(('100644 ', '100755 ')) for line in modes), 'patched index contains a special file')
    for name, hashes in entry['files'].items():
        require(digest(safe_path(repo, name)) == hashes['after_sha256'], f'applied hash differs: {name}')
    return {'repository': entry['repository'], 'base_commit': entry['base_commit'],
            'patch': entry['patch'], 'sha256': entry['sha256'], 'files': entry['files'],
            'result_tree': output('git', '-C', repo, 'write-tree'), 'status': 'PASS'}


def normalize_new_files(root, patch):
    # Preserve the existing Mesa compatibility fix; never normalize gpu019 patches.
    text = patch.read_text()
    fixed = re.sub(r'^(diff --git a/(\S+) b/\2\n)(--- /dev/null\n)', r'\1new file mode 100644\n\3', text, flags=re.M)
    if fixed != text:
        patch.write_text(fixed)
        print(f'{patch.relative_to(root)}: added missing new file mode header', flush=True)


def checkout(path, pin):
    require(not path.exists() and not path.is_symlink(), f'refuse to replace existing download: {path}')
    path.parent.mkdir(parents=True, exist_ok=True)
    # A fresh empty download directory, separate from our scripts and execution cwd.
    with tempfile.TemporaryDirectory(prefix='source-download-', dir=path.parent) as temporary:
        source = Path(temporary) / 'source'
        run('git', 'init', '-q', source)
        for url in [pin.get('mirror'), pin['repository']]:
            if url and subprocess.run(['git', '-C', str(source), 'fetch', '--depth=1', url, pin['commit']]).returncode == 0:
                break
        else:
            raise RuntimeError(f'cannot fetch pinned dependency: {pin["commit"]}')
        run('git', '-C', source, 'checkout', '-q', '--detach', 'FETCH_HEAD')
        require(output('git', '-C', source, 'rev-parse', 'HEAD') == pin['commit'], 'download pin mismatch')
        source.rename(path)


def llvm_tool(name):
    result = next((p for p in [f'/opt/homebrew/opt/llvm/bin/{name}', shutil.which(name)] if p and Path(p).is_file()), None)
    require(result, f'{name} required')
    return result


def test_command(report, out, name, command, *, env=None, timeout=600, marker=None):
    log = out / 'logs' / (name + '.log')
    start = time.monotonic()
    try:
        result = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        partial = error.stdout or ''
        log.write_text(partial.decode(errors='replace') if isinstance(partial, bytes) else partial)
        report['tests'][name] = {'status': 'FAIL', 'reason': 'timeout', 'timeout_seconds': timeout,
                                 'log_sha256': digest(log)}
        raise
    log.write_text(result.stdout)
    passed = result.returncode == 0 and (marker is None or marker in result.stdout)
    report['tests'][name] = {'status': 'PASS' if passed else 'FAIL', 'exit_code': result.returncode,
                             'seconds': round(time.monotonic() - start, 2), 'log_sha256': digest(log)}
    print(result.stdout[-6000:], end='', flush=True)
    require(passed, f'{name} failed; see {log.name}')
    return result.stdout


# Device::compile_compute_pipeline performs AIR -> SPIR-V with no Vulkan loader.
# The API and public synthetic profile are the ones used by MetalVulkanProbe.rs.
TRANSLATE_RS = r'''use metal2vulkan::device::{Device, DeviceDescription, DevicePolicy, SamplingPolicy};
use metal2vulkan::library::Library;
use metal2vulkan::pipeline::{ComputePipelineDescriptor, FunctionDescriptor, StageSource};
use metal2vulkan::Budget;
use std::{env, fs};
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = env::args().collect();
    let description = DeviceDescription::from_khronos_json(&fs::read_to_string(&args[1])?, None)?;
    let mut policy = DevicePolicy::default().exact_answers();
    policy.sampling = SamplingPolicy::Exact;
    let device = Device::new(description, policy)?;
    let bytes = fs::read(&args[2])?;
    let library = Library::from_air(&bytes)?;
    for name in ["scanout_convert", "scanout_opaque", "scanout_b3a8"] {
        let function = library.function(name)?;
        let descriptor = ComputePipelineDescriptor::new(StageSource::Function(FunctionDescriptor::new(function)));
        let pipeline = device.compile_compute_pipeline(&descriptor, &Budget::standard())?;
        let words = &*pipeline.stage.spirv;
        assert!(!words.is_empty() && words[0] == 0x07230203);
        let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_le_bytes()).collect();
        fs::write(format!("{}/{name}.spv", args[3]), bytes)?;
        println!("PASS: translated {name} (no GPU execution)");
    }
    Ok(())
}
'''

# Probe only hardware/loader availability. A later bridge or pixel failure is a FAIL,
# never reclassified as an unavailable runner after a successful capability probe.
CAPABILITY_MM = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <cstdio>
#include <vulkan/vulkan.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        if (!MTLCreateSystemDefaultDevice()) {puts("SKIP: no Metal device on runner"); return 77;}
        void *loader = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!loader) {puts("SKIP: Vulkan loader unavailable"); return 77;}
        auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
        if (!get) {puts("SKIP: vkGetInstanceProcAddr unavailable"); return 77;}
        auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!create) return 1;
        VkApplicationInfo app = {}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_2;
        const char *extensions[] = {"VK_KHR_portability_enumeration"};
        VkInstanceCreateInfo info = {}; info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app; info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = extensions;
        info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        VkInstance instance = VK_NULL_HANDLE;
        VkResult result = create(&info, nullptr, &instance);
        if (result != VK_SUCCESS) {printf("SKIP: host Vulkan 1.2 instance unavailable (%d)\n", result); return 77;}
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
        auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
        if (!enumerate || !destroy) return 1;
        uint32_t count = 0; result = enumerate(instance, &count, nullptr); destroy(instance, nullptr);
        if (result != VK_SUCCESS || !count) {printf("SKIP: no usable host Vulkan physical device (%d)\n", result); return 77;}
        puts("PASS: host Metal device and Vulkan physical device available"); return 0;
    }
}
'''


def target_environment(sdk, deployment, base=None):
    env = dict(os.environ if base is None else base)
    for key in ('SDKROOT', 'IPHONEOS_DEPLOYMENT_TARGET', 'MACOSX_DEPLOYMENT_TARGET'):
        env.pop(key, None)
    env['SDKROOT'] = str(sdk)
    env[deployment[0]] = deployment[1]
    return env


def host_checks(root, out, report, host_env):
    # Every host subprocess receives its own macOS SDK; never export it globally.
    def host_test(report, out, name, command, **kwargs):
        kwargs.setdefault('env', host_env)
        return test_command(report, out, name, command, **kwargs)

    host_test(report, out, 'host-bridge-build',
              [sys.executable, root / 'scripts/build-adreno-metal-bridge.py', '--target', 'host'], timeout=2100)
    build = root / 'build/adreno-metal-bridge'
    air = build / 'scanout_convert.air'
    host_triple = output(llvm_tool('clang'), '-dumpmachine', env=host_env)
    host_test(report, out, 'air-verifier',
                 [llvm_tool('opt'), '-mtriple=' + host_triple, '-passes=verify', '-disable-output', air])
    work = root / 'build/gpu019-ci-tests'
    work.mkdir()
    translate = work / 'translate.rs'
    translate.write_text(TRANSLATE_RS)
    manifest = work / 'Cargo.toml'
    bridge_manifest = (build / 'Cargo.toml').read_text()
    require(bridge_manifest.count('[lib]') == 1, 'unexpected bridge manifest')
    # Keep the dependency features/profile but do not rebuild the bridge lib outside
    # its own script: its include_bytes! inputs require the script's NMB_* environment.
    binary_section = '[[bin]]\nname = "gpu019-translate"\npath = ' + json.dumps(str(translate)) + '\n'
    manifest.write_text(re.sub(r'\[lib\]\n.*?(?=\[dependencies\])',
                               lambda _: binary_section, bridge_manifest, count=1, flags=re.S))
    shutil.copyfile(build / 'Cargo.lock', work / 'Cargo.lock')
    # Reuse the bridge's exact translator, features, profile and Cargo target directory.
    host_test(report, out, 'air-translator-build',
                 ['cargo', '+1.98.0', 'build', '--release', '--locked', '--manifest-path', manifest,
                  '--target', 'aarch64-apple-darwin', '--target-dir', root / 'build/m2v-target', '--bin', 'gpu019-translate'],
                 timeout=1800)
    profile = root / '.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-validation/profiles/vk12-bare.json'
    text = host_test(report, out, 'air-translation',
                        [root / 'build/m2v-target/aarch64-apple-darwin/release/gpu019-translate', profile, air, work],
                        marker='PASS: translated scanout_b3a8')
    require(all(f'PASS: translated {name}' in text for name in ('scanout_convert', 'scanout_opaque', 'scanout_b3a8')),
            'not all scanout entry points translated')
    for name in ('scanout_convert', 'scanout_opaque', 'scanout_b3a8'):
        host_test(report, out, name + '-spirv-validation', ['spirv-val', '--target-env', 'vulkan1.2', work / (name + '.spv')])
    report['translation'] = {'gpu_execution': False, 'air_sha256': digest(air), 'profile_sha256': digest(profile),
                             'source_sha256': digest(translate),
                             'spirv_sha256': {p.name: digest(p) for p in sorted(work.glob('*.spv'))}}
    kit = root / 'drivers/AdrenoMetal'
    flags = ['-std=c++17', '-fobjc-arc', '-O1', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-I', kit]
    frameworks = ['-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface', '-framework', 'CoreFoundation']
    for name in ('scanout_host', 'present_host'):
        host_test(report, out, name + '-compile',
                     ['xcrun', '--sdk', 'macosx', 'clang++', *flags, kit / 'NativeBridgeMetalFrontend.mm',
                      kit / 'bridge' / (name + '.mm'), build / 'host/libadreno_metal_bridge.a', *frameworks, '-o', work / name])
    probe = work / 'capability.mm'
    probe.write_text(CAPABILITY_MM)
    host_test(report, out, 'host-capability-compile',
                 ['xcrun', '--sdk', 'macosx', 'clang++', *flags, '-I', root / '.cache/mesa-work/include',
                  probe, '-framework', 'Foundation', '-framework', 'Metal', '-o', work / 'capability'])
    loader = Path('/opt/homebrew/lib/libMoltenVK.dylib')
    capability = subprocess.run([str(work / 'capability'), str(loader)], capture_output=True, text=True, timeout=60, env=host_env)
    (out / 'logs/host-capability.log').write_text(capability.stdout + capability.stderr)
    require(capability.returncode in (0, 77), 'host capability probe failed unexpectedly')
    report['tests']['host-capability'] = {'status': 'PASS' if capability.returncode == 0 else 'SKIP',
                                          'detail': capability.stdout.strip(), 'exit_code': capability.returncode}
    print(capability.stdout, end='', flush=True)
    names = ('scanout_host-cold', 'scanout_host-warm', 'present_host-cold', 'present_host-warm', 'present_host-system')
    if capability.returncode == 77:
        for name in names:
            report['tests'][name] = {'status': 'SKIP', 'reason': capability.stdout.strip(), 'gpu_execution': False}
        return
    env = dict(host_env)
    env.pop('NMB_SYSTEM_DEVICE', None)
    for name in ('scanout_host', 'present_host'):
        cache = work / (name + '-cache')
        cache.mkdir()
        for state in ('cold', 'warm'):
            text = host_test(report, out, name + '-' + state, [work / name, loader, cache], env=env, marker='PASS:')
            if name == 'scanout_host':
                require('b3a8' in text.lower(), 'scanout test did not report b3a8 coverage')
            report['tests'][name + '-' + state]['gpu_execution'] = True
    host_test(report, out, 'present_host-system', [work / 'present_host', loader, work / 'system-cache'],
                 env=dict(env, NMB_SYSTEM_DEVICE='1'), marker='PASS:')
    report['tests']['present_host-system']['gpu_execution'] = True


def source_hashes(root):
    files = {}
    for directory in PINS:
        base = root / directory
        for name in output('git', '-C', base, 'ls-files', '-z').split('\0'):
            if name:
                files[directory + '/' + name] = digest(safe_path(base, name))
    for name in ('build-adreno-metal-plugin.py', 'build-adreno-metal-bridge.py', 'build-turnip-darwin.py',
                 'make-gpu-addon.py', 'build-boot-runtime.py', 'mesa_patch_identity.py',
                 'data/adreno-compiler-inputs.json', 'data/gpu-metal-vulkan-inputs.json',
                 'requirements-adreno-compiler.txt'):
        files['scripts/' + name] = digest(safe_path(root / 'scripts', name))
    return dict(sorted(files.items()))


def verify_addon(addon, bundle):
    expected = {'addons/NativeAdrenoMetal.publish': b'1\n'}
    for path in sorted(bundle.rglob('*')):
        require(not path.is_symlink(), 'bundle symlink refused')
        if path.is_file():
            name = path.relative_to(bundle).as_posix()
            require(name in {'NativeAdrenoMetal', 'libvulkan_freedreno.dylib', 'Info.plist',
                             'gpu-build.json', '_CodeSignature/CodeResources'}, 'unexpected bundle file: ' + name)
            expected['addons/NativeAdrenoMetal.bundle/' + name] = path.read_bytes()
    data = addon.read_bytes()
    require(len(data) >= 24 and data[:8] == b'NGPUADD2', 'invalid addon header')
    count, reserved, length = struct.unpack_from('<IIQ', data, 8)
    require(reserved == 0 and length == len(data) and count == len(expected), 'invalid addon header/count')
    at, entries, executables = 24, {}, []
    for _ in range(count):
        require(at + 24 <= len(data), 'short addon record')
        length, mode, size, packed = struct.unpack_from('<IIQQ', data, at)
        at += 24
        require(1 < length <= 160 and at + length <= len(data) and data[at + length - 1] == 0, 'invalid addon path')
        name = data[at:at + length - 1].decode('utf-8')
        relative_path(name)
        require(name in expected and name not in entries, 'unexpected/duplicate addon entry')
        at += (length + 7) & ~7
        require(at + packed <= len(data), 'short compressed addon entry')
        unpacker = zlib.decompressobj()
        body = unpacker.decompress(data[at:at + packed], len(expected[name]) + 1)
        require(unpacker.eof and not unpacker.unused_data and not unpacker.unconsumed_tail, 'invalid compressed addon entry')
        at += (packed + 7) & ~7
        require(len(body) == size and body == expected[name], 'addon differs from signed bundle')
        executable = body[:4] in MACHO
        require(mode == (0o755 if executable else 0o644), 'unexpected addon permissions')
        if executable:
            executables.append(name)
        entries[name] = hashlib.sha256(body).hexdigest()
    require(at == len(data) and set(entries) == set(expected), 'addon entry mismatch')
    require(set(executables) == {'addons/NativeAdrenoMetal.bundle/NativeAdrenoMetal',
                                 'addons/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib'} and len(executables) == 2,
            'addon must contain exactly plugin and Turnip executables')
    return entries


def signature(path):
    run('codesign', '--verify', '--strict', path)
    detail = run('codesign', '--display', '--verbose=4', path, capture_output=True, text=True).stderr
    match = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)
    require(match and re.search(r'^Page size=4096$', detail, re.M), 'invalid signature identity/page size')
    return match[1]


def build(root, out, inputs_path, report):
    require(sys.platform == 'darwin', 'full build requires real macOS/Xcode; Linux only supports --self-test')
    require(output('git', '-C', root, 'rev-parse', 'HEAD') == FRAMEWORK, 'framework HEAD differs from fixed pin')
    require(not output('git', '-C', root, 'status', '--porcelain', '--untracked-files=all', '--ignore-submodules=all'),
            'framework must be a clean checkout')
    for name, pin in PINS.items():
        base = root / name
        require(not base.is_symlink() and output('git', '-C', base, 'rev-parse', 'HEAD') == pin, f'submodule pin differs: {name}')
        require(not output('git', '-C', base, 'status', '--porcelain', '--untracked-files=all'), f'dirty submodule: {name}')
    inputs = load_inputs(inputs_path)
    report.update(framework_commit=FRAMEWORK, base_commits=PINS, inputs_sha256=digest(inputs_path),
                  wrapper_sha256=digest(Path(__file__)), target='arm64-apple-ios17.0', daemon_included=False)
    report['patches'] = [apply_patch(root, inputs_path.parent, entry) for entry in inputs['patches']]
    for name, command in {
        'xcode': ['xcodebuild', '-version'], 'ios_sdk': ['xcrun', '--sdk', 'iphoneos', '--show-sdk-version'],
        'ios_sdk_build': ['xcrun', '--sdk', 'iphoneos', '--show-sdk-build-version'],
        'macos_sdk': ['xcrun', '--sdk', 'macosx', '--show-sdk-version'],
        'clang': ['xcrun', '--sdk', 'iphoneos', 'clang', '--version'],
        'llvm_as': [llvm_tool('llvm-as'), '--version'], 'llvm_opt': [llvm_tool('opt'), '--version'],
        'rustc': ['rustc', '+1.98.0', '--version', '--verbose'], 'cargo': ['cargo', '+1.98.0', '--version'],
        'spirv_val': ['spirv-val', '--version'], 'python': [sys.executable, '--version'],
    }.items():
        report['toolchain'][name] = output(*command)
    sdk = Path(output('xcrun', '--sdk', 'iphoneos', '--show-sdk-path'))
    require(sdk.is_dir() and (sdk / 'SDKSettings.json').is_file(), 'real iphoneos SDK required')
    report['toolchain']['ios_sdk_settings_sha256'] = digest(sdk / 'SDKSettings.json')
    host_sdk = Path(output('xcrun', '--sdk', 'macosx', '--show-sdk-path'))
    require(host_sdk.is_dir() and (host_sdk / 'SDKSettings.json').is_file(), 'real macOS SDK required')
    host_env = target_environment(host_sdk, ('MACOSX_DEPLOYMENT_TARGET', '14.0'))
    ios_env = target_environment(sdk, ('IPHONEOS_DEPLOYMENT_TARGET', '17.0'))
    report['toolchain']['macos_sdk_settings_sha256'] = digest(host_sdk / 'SDKSettings.json')
    report['toolchain']['host_deployment_target'] = '14.0'
    test_command(report, out, 'b3a8-ir',
                 [sys.executable, '-I', root / 'drivers/AdrenoMetal/bridge/check_scanout_b3a8.py',
                  '--clang', llvm_tool('clang'), '--llvm-as', llvm_tool('llvm-as'), '--opt', llvm_tool('opt')],
                 env=host_env, marker='PASS: complete AIR verifies; actual kernel passes O0, O2 and ASan/UBSan host runs')
    layout = root / 'build/gpu019-layout-test'
    layout.parent.mkdir(exist_ok=True)
    test_command(report, out, 'scanout-layout-compile',
                 [llvm_tool('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                  root / 'drivers/AdrenoMetal/bridge/scanout_layout_host.cpp', '-o', layout], env=host_env)
    test_command(report, out, 'scanout-layout', [layout], env=host_env, marker='PASS')
    client = root / 'build/gpu019-client-test'
    test_command(report, out, 'scanout-client-compile',
                 ['xcrun', '--sdk', 'macosx', 'clang++', '-std=c++17', '-fobjc-arc',
                  '-framework', 'Foundation', '-framework', 'Metal',
                  root / 'drivers/AdrenoMetal/bridge/scanout_client_host.mm', '-o', client], env=host_env)
    test_command(report, out, 'scanout-client', [client], env=host_env, marker='PASS')

    # The existing iOS17 unused-function warning workaround, recorded by the source manifest.
    plugin_script = root / 'scripts/build-adreno-metal-plugin.py'
    text = plugin_script.read_text()
    require(text.count("'-Wextra', '-Werror',") == 1, 'plugin compiler flags not found')
    plugin_script.write_text(text.replace("'-Wextra', '-Werror',", "'-Wextra', '-Werror', '-Wno-error=unused-function',"))
    pins = strict_json(root / 'scripts/data/adreno-compiler-inputs.json')
    for patch in pins['patches']:
        normalize_new_files(root, root / patch)
    mesa = root / '.cache/mesa-work'
    checkout(mesa, pins['mesa'])
    for patch in pins['patches']:
        if subprocess.run(['git', '-C', str(mesa), 'apply', str(root / patch)]).returncode:
            print(f'git apply refused recorded Mesa patch {patch}; using its existing patch -p1 fallback', flush=True)
            run('patch', '-p1', '--forward', '--no-backup-if-mismatch', '-i', root / patch, cwd=mesa)
    spec = importlib.util.spec_from_file_location('mesa_patch_identity', root / 'scripts/mesa_patch_identity.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.validate_recorded_patches(root, mesa, pins)
    report['mesa'] = {'commit': pins['mesa']['commit'], 'patches': {p: digest(root / p) for p in pins['patches']}, 'status': 'PASS'}
    translator = strict_json(root / 'scripts/data/gpu-metal-vulkan-inputs.json')['translator']
    checkout(root / '.cache/gpu-reference/metal2vulkan', translator)
    report['translator'] = {'commit': translator['commit']}
    host_checks(root, out, report, host_env)
    report['source_sha256'] = source_hashes(root)
    report['source_manifest_sha256'] = hashlib.sha256(json.dumps(report['source_sha256'], sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    report['translator']['patches'] = {p.relative_to(root).as_posix(): digest(p)
                                        for p in sorted((root / 'drivers/AdrenoMetal/bridge/patches').glob('*.patch'))}
    venv = root / '.cache/mesa-venv'
    run(sys.executable, '-m', 'venv', venv)
    run(venv / 'bin/python', '-m', 'pip', 'install', '-q', '-r', root / 'scripts/requirements-adreno-compiler.txt')
    test_command(report, out, 'turnip-ios-build', [sys.executable, root / 'scripts/build-turnip-darwin.py', '--target', 'ios'], env=ios_env, timeout=3600)
    test_command(report, out, 'plugin-ios-build',
                 [sys.executable, root / 'scripts/build-adreno-metal-plugin.py', '--target', 'ios', '--no-replay'], env=ios_env, timeout=2400)
    require(report['source_sha256'] == source_hashes(root), 'sources changed during compilation')
    plugin = root / 'build/adreno-metal-plugin'
    bundle = plugin / 'NativeAdrenoMetal.bundle'
    require(plistlib.loads((bundle / 'Info.plist').read_bytes())['MinimumOSVersion'] == '17.0', 'wrong deployment target')
    for name in ('NativeAdrenoMetal', 'libvulkan_freedreno.dylib'):
        header = (bundle / name).read_bytes()[:12]
        require(struct.unpack('<III', header) == (0xfeedfacf, 0x100000c, 0), 'non-arm64 Mach-O refused')
    plugin_cdhash = signature(bundle)
    turnip_cdhash = signature(bundle / 'libvulkan_freedreno.dylib')
    trust = strict_json(plugin / 'trust.json')
    require(trust['cdhash'] == plugin_cdhash and trust['turnip_cdhash'] == turnip_cdhash and plugin_cdhash != turnip_cdhash,
            'trust CDHashes do not identify plugin and Turnip')
    # Preserve the upstream schema for trust-append-module.py consumers. Its daemon
    # field is informational: exactly two hashes below are eligible for deployment.
    shutil.copyfile(plugin / 'trust.json', out / 'trust.json')
    addon = out / 'gpuaddon.bin'
    run(sys.executable, root / 'scripts/make-gpu-addon.py', '--publish', '--no-daemon', addon,
        env=dict(os.environ, ADRENO_PLUGIN_DIR=str(plugin)))
    entries = verify_addon(addon, bundle)
    checks = {'status': 'PASS', 'addon_entries_sha256': entries, 'addon_executable_cdhashes': sorted([plugin_cdhash, turnip_cdhash]),
              'trust_cdhashes': sorted([trust['cdhash'], trust['turnip_cdhash']]), 'daemon_included': False, 'match': True}
    write_json(out / 'checks.json', checks)
    shutil.copyfile(bundle / 'gpu-build.json', out / 'gpu-build.json')
    # Only package a fully checked bundle. Failed builds upload diagnostics, never this file.
    run('tar', '-C', plugin, '-czf', out / 'NativeAdrenoMetal.bundle.tar.gz', 'NativeAdrenoMetal.bundle')
    air = root / 'build/adreno-metal-bridge/scanout_convert.air'
    require(digest(air) == report['translation']['air_sha256'], 'iOS embedded AIR differs from translated/tested host AIR')
    report['compiled'] = {
        'scanout_ir_sha256': digest(root / 'drivers/AdrenoMetal/shaders/scanout_convert.ll'),
        'scanout_air_sha256': digest(air),
        'host_bridge_sha256': digest(root / 'build/adreno-metal-bridge/host/libadreno_metal_bridge.a'),
        'ios_bridge_sha256': digest(root / 'build/adreno-metal-bridge/ios/libadreno_metal_bridge.a'),
        'plugin_sha256': digest(bundle / 'NativeAdrenoMetal'),
        'turnip_sha256': digest(bundle / 'libvulkan_freedreno.dylib'),
        'cargo_lock_sha256': digest(root / 'build/adreno-metal-bridge/Cargo.lock'),
    }
    report['artifacts_sha256'] = {name: digest(out / name) for name in
                                ('gpuaddon.bin', 'trust.json', 'checks.json', 'gpu-build.json', 'NativeAdrenoMetal.bundle.tar.gz')}
    report['signature_checks'] = checks
    report['tests']['ios17-signatures-addon'] = {'status': 'PASS', 'executable_count': 2, 'daemon_included': False}
    print('PASS: GPU addon and provenance; host pixel results are recorded separately', flush=True)


def self_test():
    import unittest

    class Checks(unittest.TestCase):
        def test_paths(self):
            for name in ('../escape', '/absolute', 'a/../b', '.git/config', 'a//b', 'a/./b', 'a\\b', 'a b', ''):
                with self.subTest(name=name), self.assertRaises(RuntimeError):
                    relative_path(name)
            self.assertEqual(str(relative_path('bridge/check_scanout_b3a8.py')), 'bridge/check_scanout_b3a8.py')

        def test_symlinks(self):
            with tempfile.TemporaryDirectory() as td:
                root = Path(td)
                (root / 'file').write_text('safe')
                (root / 'link').symlink_to(root / 'file')
                (root / 'dir').symlink_to(root, target_is_directory=True)
                for name in ('link', 'dir/file'):
                    with self.assertRaises(RuntimeError):
                        safe_path(root, name)

        def test_duplicate_json(self):
            with tempfile.TemporaryDirectory() as td:
                path = Path(td) / 'input.json'
                path.write_text('{"schema_version":1,"schema_version":1}')
                with self.assertRaises(RuntimeError):
                    strict_json(path)

        def test_exact_patch(self):
            with tempfile.TemporaryDirectory() as td:
                root = Path(td)
                repo = root / 'repo'
                run('git', 'init', '-q', repo)
                (repo / 'source.h').write_text('before\n')
                run('git', '-C', repo, 'add', 'source.h')
                run('git', '-C', repo, '-c', 'user.name=CI test', '-c', 'user.email=ci@example.invalid', 'commit', '-qm', 'base')
                base = output('git', '-C', repo, 'rev-parse', 'HEAD')
                before = digest(repo / 'source.h')
                (repo / 'source.h').write_text('after\n')
                after = digest(repo / 'source.h')
                patch = root / 'test.patch'
                patch.write_text(output('git', '-C', repo, 'diff') + '\n')
                run('git', '-C', repo, 'restore', 'source.h')
                entry = {'repository': 'repo', 'base_commit': base, 'patch': patch.name, 'sha256': digest(patch),
                         'files': {'source.h': {'before_sha256': before, 'after_sha256': after}}}
                wrong = dict(entry, sha256='0' * 64)
                with self.assertRaises(RuntimeError):
                    apply_patch(root, root, wrong)
                self.assertEqual(apply_patch(root, root, entry)['status'], 'PASS')
                with self.assertRaises(RuntimeError):
                    apply_patch(root, root, entry)
                for text in (patch.read_text().replace('b/source.h', 'b/other.h'),
                             patch.read_text() + 'new file mode 120000\n',
                             patch.read_text() + 'GIT binary patch\n'):
                    with self.assertRaises(RuntimeError):
                        check_patch_text(text, entry['files'])

        def test_addon(self):
            with tempfile.TemporaryDirectory() as td:
                root = Path(td)
                bundle = root / 'bundle'
                bundle.mkdir()
                for name in ('NativeAdrenoMetal', 'libvulkan_freedreno.dylib'):
                    (bundle / name).write_bytes(b'\xcf\xfa\xed\xfe' + name.encode())
                entries = [('addons/NativeAdrenoMetal.bundle/' + p.name, 0o755, p.read_bytes()) for p in sorted(bundle.iterdir())]
                entries.append(('addons/NativeAdrenoMetal.publish', 0o644, b'1\n'))
                def pack(items):
                    body = b''
                    for name, mode, data in items:
                        path = name.encode() + b'\0'
                        compressed = zlib.compress(data)
                        body += struct.pack('<IIQQ', len(path), mode, len(data), len(compressed))
                        body += path + bytes(-len(path) % 8) + compressed + bytes(-len(compressed) % 8)
                    return b'NGPUADD2' + struct.pack('<IIQ', len(items), 0, 24 + len(body)) + body
                addon = root / 'addon.bin'
                addon.write_bytes(pack(entries))
                self.assertEqual(len(verify_addon(addon, bundle)), 3)
                for bad in (entries + [('bin/adrenoprobed', 0o755, b'\xcf\xfa\xed\xfe')],
                            [entries[0], entries[0], entries[2]],
                            [(entries[0][0], 0o644, entries[0][2]), *entries[1:]]):
                    addon.write_bytes(pack(bad))
                    with self.assertRaises(RuntimeError):
                        verify_addon(addon, bundle)

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Checks))
    require(result.wasSuccessful(), 'CI wrapper self-tests failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('unixnu', type=Path, nargs='?')
    parser.add_argument('out', type=Path, nargs='?')
    parser.add_argument('--inputs', type=Path, default=Path(__file__).parent / 'patches/inputs.json')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    require(args.unixnu is not None and args.out is not None, 'UNIXNU and OUT are required')
    root, out = args.unixnu.resolve(), args.out.resolve()
    require(not args.unixnu.is_symlink() and not args.out.is_symlink(), 'symlink root/output refused')
    require(not out.exists() or not any(out.iterdir()), 'OUT must be new or empty')
    out.mkdir(parents=True, exist_ok=True)
    (out / 'logs').mkdir()
    inputs_path = safe_path(args.inputs.absolute().parent, args.inputs.name)
    report = {'schema_version': 1, 'status': 'RUNNING', 'tests': {}, 'toolchain': {},
              'hardware_acceptance': 'NOT_TESTED', 'legacy_gpu_build_is_sufficient_evidence': False}
    try:
        build(root, out, inputs_path, report)
        report['status'] = 'PASS'
    except BaseException as error:
        report['status'] = 'FAIL'
        report['error'] = str(error)
        raise
    finally:
        write_json(out / 'provenance.json', report)
        write_json(out / 'tests.json', report['tests'])


if __name__ == '__main__':
    main()
