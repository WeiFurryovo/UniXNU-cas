// SPDX-License-Identifier: BSD-2-Clause-Patent
// cas Adreno 650 bring-up, after UniXNU-alioth's GpuProbe.cpp (f188378), whose modes this keeps. cas-gpu=<n>:
//   1  read only (GpuPowerBoard.h gpb_run: the GCC GPU branches, then GPUCC only when its AHB clock is enabled, the GPU SMMU's
//      global space 0 only when CX and the SMMU clock are on, RBBM_STATUS only when GX is on, so no block is read with its clock
//      or rail off), plus the GPU speed-bin fuse and the two GPU temperature sensors; nothing is written
//   2  as 1, plus gcc_gpu_cfg_ahb_clk's enable bit if clear (the gcc-sm8250 probe's write)
//   3  as 2, plus the CX GDSC (gdsc_enable) and the GPU SMMU's clocks (gpu_cc_ahb_clk, the HLOS SMMU vote)
//   5  as 3, plus the AOP command DB read and searched for the ARC (gfx/mx/cx/gmxc levels) and BCM (SH0, MC0, ACV, CN0) resources
//   6  as 5, with the GMU clocks brought up (GPLL0 GPU votes, gpu_cc_gmu_clk_src at 200 MHz, the five GMU branches) and the GMU
//      and RSCC state read; the GMU stays in reset
// Every write goes through writable(), alioth's list: CX power and clocks only, no GX, GMU, RSCC, GPU or SMMU register. GX and
// any GPU frequency are the GMU's (mode 7 onwards, not ported yet); its top vote must follow cas's speed bin, 0 (587 MHz).
//   speed bin: the live FDT's gpu_speed_bin@419b in qfprom@780000, bits 7:5 (kgsl's qcom,gpu-pwrlevels-<bin>: bin 0 tops at
//              587 MHz, bins 1, 2 and 4 at 670 MHz, bin 3 at 525 MHz). The GMU's top vote must follow this bin.
//   sensors:   gpuss-0-usr is TSENS0 sensor 15, gpuss-1-usr TSENS1 sensor 8 (tsens@c223000, TM 0xc265000); same tsens-v2
//              status layout as CpuFreqProbe.cpp.
// The rows (banner rows 0, 1, 2, 4, 5 and 6 with cas-banner=1) are written once, 5 s after the late probe, as alioth does, so the
// display already runs if a read hangs the bus.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "GpuPowerBoard.h"
int snprintf(char *, size_t, const char *, ...);
char native_gpu_short[6][48];
volatile uint32_t native_gpu_step;
}

namespace {
constexpr uint64_t kPhys[GPB_WINDOWS] = {GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE,
                                         GPB_GMU_BASE, GPB_RSCC_BASE};
constexpr uint64_t kLen[GPB_WINDOWS] = {0x1000, 0x1000, GPB_GPUCC_SIZE, GPB_SMMU_GR0_SIZE, GPB_GPU_WINDOW, GPB_GMU_SIZE,
                                        GPB_RSCC_SIZE};
constexpr uint64_t kQfpromPage = 0x784000, kTsens0Tm = 0xc263000, kTsens1Tm = 0xc265000;
constexpr uint32_t kSpeedBinByte = 0x19b, kTsensStatus = 0xa0;
unsigned gMode;
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
// The registers a mode may write (alioth's list); everything else, the GMU, RSCC, GPU and SMMU windows included, is refused.
bool writable(unsigned w, uint32_t off) {
    if (w == GPB_WIN_GCC_GPU && off == GPB_GCC_GPU_CFG_AHB) return gMode >= 2;
    if (w == GPB_WIN_GPUCC && (off == GPB_CC_CX_GDSCR || off == GPB_CC_AHB_CLK || off == GPB_CC_SMMU_VOTE_CLK)) return gMode >= 3;
    if (gMode < 6) return false;
    if (w == GPB_WIN_GCC_VOTE) return off == GPB_GCC_PLL_VOTE;
    if (w == GPB_WIN_GCC_GPU) return off == GPB_GCC_DDRSS_GPU_AXI || off == GPB_GCC_GPU_MEMNOC_GFX;
    if (w == GPB_WIN_GPUCC) return off == GPB_CC_GMU_RCGR || off == GPB_CC_GMU_RCGR + 4 || off == GPB_CC_CX_GMU_CLK || off == GPB_CC_CXO_CLK;
    return false;
}
int wr(void *, unsigned w, uint32_t off, uint32_t value) {
    if (w >= GPB_WINDOWS || !gVa[w] || off >= kLen[w] || (off & 3) || !writable(w, off)) return -1;
    *reinterpret_cast<volatile uint32_t *>(gVa[w] + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

// The command DB (alioth's commandDb), copied with aligned 32-bit reads since it is mapped as device memory. Row: each
// lookup's result (0 found) and the number of 16-bit levels gfx.lvl, mx.lvl and cx.lvl carry.
alignas(8) uint8_t gCmdDb[GPB_CMD_DB_BYTES];
void commandDb(char *row) {
    IODeviceMemory *memory = IODeviceMemory::withRange(GPB_CMD_DB_BASE, GPB_CMD_DB_BYTES);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
    if (memory) memory->release();
    if (!map) {snprintf(row, 48, "DB nomap"); return;}
    const volatile uint32_t *src = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    for (uint32_t i = 0; i < GPB_CMD_DB_BYTES / 4; ++i) {
        uint32_t v = src[i];
        memcpy(gCmdDb + 4 * i, &v, 4);
    }
    map->release();
    static const char *const names[8] = {"gfx.lvl", "mx.lvl", "cx.lvl", "gmxc.lvl", "SH0", "MC0", "ACV", "CN0"};
    gpb_cmd_db_entry found[8] = {};
    int rc[8];
    for (unsigned i = 0; i < 8; ++i) rc[i] = gpb_cmd_db_find(gCmdDb, sizeof gCmdDb, names[i], &found[i]);
    snprintf(row, 48, "DB %08x rc%d%d%d%d%d%d%d%d lv%u/%u/%u", gpb_le32(gCmdDb + 4), rc[0] != 0, rc[1] != 0, rc[2] != 0,
             rc[3] != 0, rc[4] != 0, rc[5] != 0, rc[6] != 0, rc[7] != 0, found[0].aux_bytes / 2, found[1].aux_bytes / 2,
             found[2].aux_bytes / 2);
}
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
    const gpb_io io = {nullptr, rd, wr, delayUs};
    gpb_result &r = gResult;
    gpb_run(&io, gMode, have, &r);
    native_gpu_step = 3;
    IOMemoryMap *qmap = nullptr;
    uintptr_t q = mapRange(kQfpromPage, 0x1000, &qmap);
    uint32_t fuse = q ? *reinterpret_cast<volatile uint32_t *>(q + (kSpeedBinByte & ~3u)) : 0;
    if (qmap) qmap->release();
    const unsigned bin = (fuse >> (8 * (kSpeedBinByte & 3)) >> 5) & 7;
    const int32_t t0 = sensor(kTsens0Tm, 15), t1 = sensor(kTsens1Tm, 8);
    native_gpu_step = 4;
    snprintf(native_gpu_short[0], 48, "GPU m%u %s %s %s %s io%d bin%u%s cx%u ck%u", gMode, gpb_state_text(r.gcc),
             gpb_state_text(r.cc), gpb_state_text(r.smmu), gpb_state_text(r.gpu), r.io_error, bin, q ? "" : "?", r.powered_cx,
             r.powered_clocks);
    snprintf(native_gpu_short[1], 48, "CC cx %08x>%08x smmu %08x>%08x", r.cx_before, r.cx_gdscr, r.cc_smmu_before, r.cc_smmu);
    snprintf(native_gpu_short[2], 48, "SMMU %08x smr%u gfsr %x rb %x T%d/%d", r.id0, r.smr_valid, r.gfsr, r.rbbm_status,
             (int)t0 / 10, (int)t1 / 10);
    if (gMode >= 5) {native_gpu_step = 50; commandDb(native_gpu_short[3]);}
    uint32_t cx_gmu = r.cc_gmu, ahb = r.cc_ahb;
    if (gMode >= 6) {
        native_gpu_step = 70;
        gpb_gmu_clock_on o;
        gpb_gmu_clocks_on(&io, &r, &o);
        snprintf(native_gpu_short[4], 48, "CLK s%u f%d gmu %08x cfg %08x ahb %08x", o.step, o.failed, o.cx_gmu, o.gmu_cfg, o.ahb);
        cx_gmu = o.cx_gmu; ahb = o.ahb;
    }
    if (gMode >= 6) {
        if (!gpb_gmu_regs_readable(&r, cx_gmu, ahb)) snprintf(native_gpu_short[5], 48, "GMU unreadable");
        else {
            native_gpu_step = 60;
            gpb_gmu_state g;
            gpb_read_gmu_state(&io, &r, &g);
            snprintf(native_gpu_short[5], 48, "GMU rst %08x fw %08x rscc %x/%x", g.sysreset, g.fw_init, g.rscc_busy,
                     g.rscc_status);
        }
    }
    native_gpu_step = 99;
}
}  // namespace

extern "C" void cas_gpu_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("cas-gpu", &mode, sizeof mode) || !mode || mode == 4 || mode > 6 || native_gpu_step) return;
    gMode = mode;
    native_gpu_step = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(gpuThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
