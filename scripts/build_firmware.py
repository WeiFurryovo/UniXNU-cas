#!/usr/bin/env python3
"""Local pinned BaseTools build/genc with saved preflight and no device actions."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

from provenance import file_ref, source_state

DEVICE = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--uefi", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--phase", choices=("genc", "modules", "build"), default="build")
    args = ap.parse_args()
    uefi, out = args.uefi.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.update(SOURCE_DATE_EPOCH="1792643741", PYTHONHASHSEED="0", LC_ALL="C.UTF-8", TZ="UTC",
               WORKSPACE=str(uefi), EDK_TOOLS_PATH=str(uefi / "Mu_Basecore/BaseTools"),
               PYTHON_COMMAND=sys.executable, CONF_PATH=str(out / "Conf"))
    prefixes = ("Platforms/Xiaomi", "Common/Mu", "Common/Mu_OEM_Sample", "Mu_Basecore", "Silicon/Qualcomm", "Silicon/Silicium")
    env["PACKAGES_PATH"] = os.pathsep.join([*(str(uefi / name) for name in prefixes), str(uefi.parent / "UniXNU/package")])
    env["PATH"] = str(uefi / "Mu_Basecore/BaseTools/BinWrappers/PosixLike") + os.pathsep + env["PATH"]
    env["PYTHONPATH"] = str(uefi / "Mu_Basecore/BaseTools/Source/Python")
    commands = []
    manifest = {"phase": args.phase, "started_at": datetime.now(timezone.utc).isoformat(), "commands": commands,
                "source_date_epoch": env["SOURCE_DATE_EPOCH"], "result": "running", "hardware_proven": False}
    manifest_path = out / "run.json"

    def run(command, name, cwd=uefi):
        command = [str(s) for s in command]
        commands.append({"argv": command, "cwd": str(cwd), "log": name})
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"{name}: {' '.join(command)}", flush=True)
        with (out / name).open("x") as log:
            result = subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
        commands[-1]["returncode"] = result.returncode
        if result.returncode:
            raise RuntimeError(f"{name} failed with code {result.returncode}")

    try:
        sources = source_state(uefi, DEVICE)
        (out / "source-state.json").write_text(json.dumps(sources, indent=2) + "\n")
        manifest["source_state"] = file_ref(out / "source-state.json")
        # Unconditionally perform target checks immediately before BaseTools.
        run([sys.executable, DEVICE / "scripts/preflight.py", "--uefi", uefi, "--out", out / "preflight.json"], "preflight.log")
        run([sys.executable, DEVICE / "scripts/generate_memory_map.py", "--uefi", uefi], "memory-compare.log")
        run([sys.executable, DEVICE / "scripts/check_target.py", "--uefi", uefi], "target-constants.log")
        clang = subprocess.run(["clang", "--version"], env=env, text=True, capture_output=True, check=True).stdout
        if "version 18.1." not in clang:
            raise RuntimeError("expected LLVM 18.1.x; source the cas environment before building")
        manifest["clang"] = clang.splitlines()[0]
        base = uefi / "Mu_Basecore/BaseTools"
        if not (base / "Source/C/bin/GenFv").is_file():
            run(["make", "-C", base, "-j" + env.get("CAS_JOBS", "8")], "basetools.log")
        (out / "Conf").mkdir()
        for source, name in (("target.template", "target.txt"), ("tools_def.template", "tools_def.txt"), ("build_rule.template", "build_rule.txt")):
            shutil.copyfile(base / "Conf" / source, out / "Conf" / name)
        command = ["build", "-a", "AARCH64", "-t", "CLANGPDB", "-b", "DEBUG", "-p", "casPkg/cas.dsc", "-n", env.get("CAS_JOBS", "8"),
                   "--conf=" + str(out / "Conf"), "-D", "CAS_BUILD_DIR=" + str(out / "Build"), "-y", out / "BuildReport.txt"]
        for kind in ("PCD", "DEPEX", "FLASH", "BUILD_FLAGS", "LIBRARY", "FIXED_ADDRESS", "HASH"):
            command += ["-Y", kind]
        if args.phase != "build":
            command.append(args.phase)
        run(command, "build.log")
        if sources != source_state(uefi, DEVICE):
            raise RuntimeError("sources changed during the build; keep this attempt and rebuild")
        if args.phase == "build":
            fd = out / "Build/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd"
            if fd.stat().st_size != 0x800000:
                raise RuntimeError("cas FD must be exactly8MiB")
            manifest["fd"] = file_ref(fd)
            manifest["build_report"] = file_ref(out / "BuildReport.txt")
        manifest["result"] = "pass"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        manifest.update(result="fail", error=str(error))
    manifest["ended_at"] = datetime.now(timezone.utc).isoformat()
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({k: manifest[k] for k in ("phase", "result", "hardware_proven", "error") if k in manifest}, indent=2), flush=True)
    return 0 if manifest["result"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
