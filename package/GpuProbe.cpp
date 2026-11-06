// SPDX-License-Identifier: BSD-2-Clause-Patent
// cas Adreno 650, stage S1: a read-only look at the GPU's clocks, power domains and SMMU before anything is brought up.
// cas-gpu=1 runs UniXNU-alioth's GpuProbe mode 1 (GpuPowerBoard.h gpb_run: the GCC GPU branches, then GPUCC only when its AHB
// clock is enabled, the GPU SMMU's global space 0 only when CX and the SMMU clock are on, RBBM_STATUS only when GX is on, so no
// block is read with its clock or rail off), plus the GPU speed-bin fuse and the two GPU temperature sensors. No register is
// written; higher modes are refused until the GMU bring-up is ported.
//   speed bin: the live FDT's gpu_speed_bin@419b in qfprom@780000, bits 7:5 (kgsl's qcom,gpu-pwrlevels-<bin>: bin 0 tops at
//              587 MHz, bins 1, 2 and 4 at 670 MHz, bin 3 at 525 MHz). The GMU's top vote must follow this bin.
//   sensors:   gpuss-0-usr is TSENS0 sensor 15, gpuss-1-usr TSENS1 sensor 8 (tsens@c223000, TM 0xc265000); same tsens-v2
//              status layout as CpuFreqProbe.cpp.
// The rows (banner rows 0, 1, 5 and 6 with cas-banner=1) are written once, 5 s after the late probe, as alioth does, so the
// display already runs if a read hangs the bus.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "GpuPowerBoard.h"
int snprintf(char *, size_t, const char *, ...);
char native_gpu_short[4][48];
volatile uint32_t native_gpu_step;
}

namespace {
constexpr uint64_t kPhys[GPB_WINDOWS] = {GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE,
                                         GPB_GMU_BASE, GPB_RSCC_BASE};
constexpr uint64_t kLen[GPB_WINDOWS] = {0x1000, 0x1000, GPB_GPUCC_SIZE, GPB_SMMU_GR0_SIZE, GPB_GPU_WINDOW, GPB_GMU_SIZE,
                                        GPB_RSCC_SIZE};
constexpr uint64_t kQfpromPage = 0x784000, kTsens0Tm = 0xc263000, kTsens1Tm = 0xc265000;
constexpr uint32_t kSpeedBinByte = 0x19b, kTsensStatus = 0xa0;
IOMemoryMap *gMaps[GPB_WINDOWS];
uintptr_t gVa[GPB_WINDOWS];
gpb_result gResult;

uintptr_t mapRange(uint64_t base, uint64_t size, IOMemoryMap **out) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    *out = map;
    return map ? static_cast<uintptr_t>(map->getVirtualAddress()) : 0;
}
int rd(void *, unsigned w, uint32_t off, uint32_t *out) {
    if (w >= GPB_WINDOWS || !gVa[w] || off >= kLen[w] || (off & 3)) return -1;
    *out = *reinterpret_cast<volatile uint32_t *>(gVa[w] + off);
    return 0;
}
int refuse(void *, unsigned, uint32_t, uint32_t) {return -1;}   // mode 1 writes nothing
void delayUs(void *, unsigned us) {IODelay(us);}

// One tsens-v2 sensor in deci-degrees, or -1000 when it does not read valid.
int32_t sensor(uint64_t tm, unsigned n) {
    IOMemoryMap *map = nullptr;
    uintptr_t va = mapRange(tm, 0x1000, &map);
    int32_t t = -1000;
    if (va) {
        uint32_t v = *reinterpret_cast<volatile uint32_t *>(va + kTsensStatus + 4 * n);
        if (v >> 21 & 1) t = (int32_t)(v << 20) >> 20;
    }
    if (map) map->release();
    return t;
}

void gpuThread(void *, wait_result_t) {
    IOSleep(5000);
    native_gpu_step = 1;
    unsigned have = 0;
    for (unsigned w = 0; w < GPB_WINDOWS; ++w) if ((gVa[w] = mapRange(kPhys[w], kLen[w], &gMaps[w]))) have |= 1u << w;
    native_gpu_step = 2;
    const gpb_io io = {nullptr, rd, refuse, delayUs};
    gpb_result &r = gResult;
    gpb_run(&io, 1, have, &r);
    native_gpu_step = 3;
    IOMemoryMap *qmap = nullptr;
    uintptr_t q = mapRange(kQfpromPage, 0x1000, &qmap);
    uint32_t fuse = q ? *reinterpret_cast<volatile uint32_t *>(q + (kSpeedBinByte & ~3u)) : 0;
    if (qmap) qmap->release();
    const unsigned bin = (fuse >> (8 * (kSpeedBinByte & 3)) >> 5) & 7;
    const int32_t t0 = sensor(kTsens0Tm, 15), t1 = sensor(kTsens1Tm, 8);
    native_gpu_step = 4;
    snprintf(native_gpu_short[0], 48, "GPU m1 %s %s %s %s io%d map%02x bin%u%s", gpb_state_text(r.gcc), gpb_state_text(r.cc),
             gpb_state_text(r.smmu), gpb_state_text(r.gpu), r.io_error, have, bin, q ? "" : "?");
    snprintf(native_gpu_short[1], 48, "GCC ahb %08x mem %08x dvm %08x", r.cfg_ahb, r.memnoc, r.dvm);
    snprintf(native_gpu_short[2], 48, "GPUCC cx %08x gx %08x gmu %08x", r.cx_gdscr, r.gx_gdscr, r.cc_gmu);
    snprintf(native_gpu_short[3], 48, "SMMU %08x smr%u rb %08x T%d/%d", r.id0, r.smr_valid, r.rbbm_status, (int)t0 / 10,
             (int)t1 / 10);
    native_gpu_step = 99;
}
}  // namespace

extern "C" void cas_gpu_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("cas-gpu", &mode, sizeof mode) || mode != 1 || native_gpu_step) return;
    native_gpu_step = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(gpuThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
