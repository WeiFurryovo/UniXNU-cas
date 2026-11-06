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
//   7  as 6, then (alioth's mode 7, gmuColdStart) only if the clocks came up, the GPU SMMU had no stream mapped, the command DB
//      gave its resources, the GMU sits in reset and the speed bin reads 0: the GMU's own SMMU context through NativeAdreno
//      (bank 1, ASID 1; the GMU kernel stream 5 under its DT mask 0x400 as two exact routes, 5 on SMR 2 and 0x405 on SMR 3), a
//      write-combined CPU view of its five regions, and the A650 cold start (package/a650/A650Gmu.c) with this phone's own
//      a650_gmu.bin and the HFI tables built from the command DB and the bin-0 GPU table below. GX stays off.
//   8  as 7, then, only after a complete cold start, GX power through the GMU (alioth's gxOn: A650GmuPowerGx sends the HFI
//      GX_BW_PERF_VOTE for the level `cas-gpu-perf=<1..6>`, default 1 = 305 MHz, then the OOB GPU_SET request), and with GX on
//      the GX GDSC, RBBM_STATUS and both GBIF halt registers, read only.
// Modes 9 and above (alioth's first command, zap shader, user clients, Metal) are not ported: cas_gpu_probe refuses them.
//
// GPU frequency (the hard rule: never above cas's speed bin). The GMU never picks a frequency of its own: it runs the level the
// host votes, by index into the performance table the host sent at HFI start (Linux v6.6 a6xx_hfi.c a6xx_hfi_send_perf_table
// and a6xx_hfi_set_freq; a6xx_gmu.c a6xx_gmu_pwrlevels_probe puts an "off" entry at index 0). alioth's table was the DTS
// gpu-opp-table_v2 in full (index 1..7 = 305, 400, 441.6, 490, 525, 587, 670 MHz) and its default vote index 7 (670 MHz). cas's
// table is its speed bin 0 only:
//   - the fuse: live FDT qfprom@780000 gpu_speed_bin@419b, bits 7:5 (device-8g-256/live-fdt.dts:8335); it reads 0 (gpu-002);
//   - bin 0's levels: kgsl qcom,gpu-pwrlevels-0, qcom,speed-bin = <0> (live-fdt.dts:38345-38432): 587, 525, 490, 441.6, 400 and
//     305 MHz, qcom,initial-pwrlevel = <5> (305 MHz: kgsl itself starts bin 0 at the lowest level);
//   - each level's RPMh corner: kgsl-3d0 operating-points-v2 = <0x340> (live-fdt.dts:38264) = gpu-opp-table_v2
//     (live-fdt.dts:38915-38953): opp-microvolt 0x40, 0x80, 0x90, 0xc0, 0xe0, 0x100 for 305 .. 587 MHz (670 MHz's 0x140 is
//     bins 1, 2 and 4 only; bin 3 tops at 525 MHz).
// So kCasGpuHz/kCasGpuRpmh below are the GMU's whole table: index 1 = 305 MHz (the default vote) .. 6 = 587 MHz; no 670 MHz entry
// exists, the device refuses any index above the table (A650Gmu.c perf_max), cas-gpu-perf is clamped to 1..6, and modes 7-8 run
// only when the fuse reads bin 0 (another bin's table is not known here). The GMU's DDR vote (cas-gpu-bw=1, alioth's pipa-gpu-bw)
// and alioth's top DDR vote through the apps RSC (cas-ddr-vote=1, alioth's pipa-ddr-vote, on by default there) are off unless set.
//   speed bin: the live FDT's gpu_speed_bin@419b in qfprom@780000, bits 7:5 (kgsl's qcom,gpu-pwrlevels-<bin>: bin 0 tops at
//              587 MHz, bins 1, 2 and 4 at 670 MHz, bin 3 at 525 MHz).
//   sensors:   gpuss-0-usr is TSENS0 sensor 15, gpuss-1-usr TSENS1 sensor 8 (tsens@c223000, TM 0xc265000); same tsens-v2
//              status layout as CpuFreqProbe.cpp.
// Writes: modes 1-6 go through writable(), alioth's list (CX power and clocks only). Mode 7 writes the GPU SMMU (S2CR 2 and 3 when
// they hold the firmware's FAULT word, then NativeAdreno's context bank 1 and SMR/S2CR 2-3), the GMU's five memory regions, and
// the GMU, RSCC and PDC registers of the cold start; mode 8 sends HFI messages and the OOB GPU_SET request. alioth's write tests
// on the GPU SMMU (SMR 5 and sCR0, its "PRE" row) are diagnostics of a past failure and are left out.
//
// Banner rows (cas-banner=1: banner rows 0, 1, 2, 4, 5 and 6; at most 47 characters). Modes 1-6:
//   0 GPU m<mode> <gcc> <cc> <smmu> <gpu> io<err> bin<n> cx<on> ck<on> s<step>
//   1 CC cx <gdscr before>><after> smmu <clk before>><after>
//   2 SMMU <ID0> smr<valid> gfsr <x> rb <rbbm> T<gpuss-0>/<gpuss-1> (deg C)
//   3 DB <magic> rc<8 lookups, 0 found> lv<gfx>/<mx>/<cx levels>
//   4 CLK s<step> f<failed> gmu <cx_gmu cbcr> cfg <rcg cfg> ahb <cbcr>
//   5 GMU rst <CM3_SYSRESET> fw <FW_INIT_RESULT> rscc <seq busy>/<status>
// Modes 7-8 overwrite rows as they go, so a hang leaves the last stage reached on screen (row 0's s<step> as well):
//   1 CTX pre s2 <S2CR2>/<S2CR3> smr <SMR2>/<SMR3>                         before the SMMU context, then
//     CTX s<step> <IOReturn> k<program>/<route status> r<routes> p<phase>/<writes> s2 <S2CR2>/<S2CR3 before>
//     CTX skip ck<step>/<failed> sm<smmu> smr<n> db<7 lookups> rst<SYSRESET> bin<n>    when a prerequisite is missing
//   3 LD run v<gpu[1] vote>..<gpu[6] vote> g<gmu vote>                     while the cold start runs, then
//     LD <A650GmuStatus> ph<phase> v<fw version> i<FW_INIT_RESULT> h<HFI_CTRL_STATUS> r<fw ready><hfi ready> ao<AO irq> c<CM3>
//   4 HFI <startup status> st<stage> a<acked stages> e<fw error> t<transport> n<sent> <ms>ms h<health>
//   5 AFT rst<SYSRESET> i<FW_INIT_RESULT> hfi<HFI_CTRL_STATUS> gx<SPTPRAC_PWR_CLK_STATUS> ao<AO irq>   (mode 7), then mode 8:
//     GX on p<perf> <MHz>M v<vote status> o<oob polls> <gx before>><after> gd<GX GDSCR>   or
//     GX off <A650GmuStatus> p<perf> <MHz>M v<vote status>/<fw error> o<polls> <before>><after> gd<GDSCR>
//   2 GX2 rb<RBBM_STATUS> gbif <halt>/<ack> rbbm <halt>/<ack> T<gpuss-0>/<gpuss-1>  (mode 8 with GX on)
//   4 DDR t<tcs> p<polls> <cmd status x3>                                  (only with cas-ddr-vote=1)
// Steps (row 0, native_gpu_step): 1-4 maps/run/fuse, 50 cmd-db, 70 clocks, 60 GMU state, 79 cold start entry, 80+n SMMU context
// step n, 90 cold start running, 97 cold start done / 98 failed, 100 GX vote, 101 GX on, 99 finished.
// The rows are written 5 s after the late probe, as alioth does, so the display already runs if a read hangs the bus.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "GpuPowerBoard.h"
#include "A650Gmu.h"
int snprintf(char *, size_t, const char *, ...);
char native_gpu_short[6][48];
volatile uint32_t native_gpu_step;
// The firmware object generated at build time from this phone's vendor partition (scripts/build-adreno650.py); never committed.
extern const uint8_t native_gpu_gmu_firmware[] __attribute__((weak_import));
extern const uint32_t native_gpu_gmu_firmware_size __attribute__((weak_import));
// NativeAdrenoMemory.cpp records the steps of a BO's release here; NativeAdrenoPublish.cpp (the user-client half, mode 11)
// defines them. That object is not in this archive, so these weak stand-ins do (a strong definition, once added, wins).
__attribute__((weak)) volatile uint32_t native_adreno_close_step[8];
__attribute__((weak)) void native_adreno_close_step_set(uint32_t n) {native_adreno_close_step[0] = n;}
// NativeAdrenoMemory.cpp's release path asks the kernel which process maps a page (weak_import; a static kernel link refuses an
// undefined one). This kernel has no such probe: 0 = "no process maps it", as UniXNU-alioth's BootProgressStubs.c defines it.
__attribute__((weak)) unsigned int native_pmap_user_mappings(unsigned int, void **, uint64_t *, char *) {return 0;}
}
#include "NativeAdrenoGmuContext.h"

namespace {
constexpr uint64_t kPhys[GPB_WINDOWS] = {GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE,
                                         GPB_GMU_BASE, GPB_RSCC_BASE};
constexpr uint64_t kLen[GPB_WINDOWS] = {0x1000, 0x1000, GPB_GPUCC_SIZE, GPB_SMMU_GR0_SIZE, GPB_GPU_WINDOW, GPB_GMU_SIZE,
                                        GPB_RSCC_SIZE};
constexpr uint64_t kQfpromPage = 0x784000, kTsens0Tm = 0xc263000, kTsens1Tm = 0xc265000;
constexpr uint32_t kSpeedBinByte = 0x19b, kTsensStatus = 0xa0;

// cas speed bin 0 (see the header): index 0 is the off entry, 1..6 = 305..587 MHz, RPMh corners from gpu-opp-table_v2.
constexpr unsigned kCasGpuLevels = 7, kCasPerfMax = kCasGpuLevels - 1, kCasPerfDefault = 1, kCasBin = 0;
constexpr uint64_t kCasGpuHz[kCasGpuLevels] = {0, 305000000, 400000000, 441600000, 490000000, 525000000, 587000000};
constexpr uint32_t kCasGpuRpmh[kCasGpuLevels] = {0, 64, 128, 144, 192, 224, 256};
constexpr uint64_t kCasGpuHzLimit = 587000000;   // qcom,gpu-pwrlevels-0 qcom,gpu-pwrlevel@0
constexpr bool casTableSafe() {
    for (unsigned i = 1; i < kCasGpuLevels; ++i)
        if (kCasGpuHz[i] <= kCasGpuHz[i - 1] || kCasGpuHz[i] > kCasGpuHzLimit || kCasGpuRpmh[i] <= kCasGpuRpmh[i - 1]) return false;
    return kCasGpuHz[0] == 0 && kCasGpuRpmh[0] == 0 && kCasGpuHz[kCasPerfMax] == kCasGpuHzLimit;
}
static_assert(casTableSafe(), "the GMU's table must be cas's speed bin 0: ascending, topping at 587 MHz");
static_assert(kCasPerfDefault == 1 && kCasGpuHz[kCasPerfDefault] == 305000000, "mode 8 votes the lowest level by default");

unsigned gMode;
IOMemoryMap *gMaps[GPB_WINDOWS];
uintptr_t gVa[GPB_WINDOWS];
gpb_result gResult;
gpb_gmu_clock_on gClocks;
gpb_gmu_state gGmu;
unsigned gBin = 7;
bool gFuseRead;

uintptr_t mapRange(uint64_t base, uint64_t size, IOMemoryMap **out, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    *out = map;
    return map ? static_cast<uintptr_t>(map->getVirtualAddress()) : 0;
}
int rd(void *, unsigned w, uint32_t off, uint32_t *out) {
    if (w >= GPB_WINDOWS || !gVa[w] || off >= kLen[w] || (off & 3)) return -1;
    *out = *reinterpret_cast<volatile uint32_t *>(gVa[w] + off);
    return 0;
}
// The registers a mode may write through the gpb sequence (alioth's list); everything else, the GMU, RSCC, GPU and SMMU windows
// included, is refused here. Mode 7's GMU, RSCC, PDC and SMMU writes go through their own paths below.
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

// Row 0, rewritten at each stage so the step reached stays on screen.
void stage(uint32_t step) {
    native_gpu_step = step;
    const gpb_result &r = gResult;
    snprintf(native_gpu_short[0], 48, "GPU m%u %s %s %s %s io%d bin%u%s cx%u ck%u s%u", gMode, gpb_state_text(r.gcc),
             gpb_state_text(r.cc), gpb_state_text(r.smmu), gpb_state_text(r.gpu), r.io_error, gBin, gFuseRead ? "" : "?",
             r.powered_cx, r.powered_clocks, step);
}

// The command DB (alioth's commandDb), copied with aligned 32-bit reads since it is mapped as device memory. Row: each
// lookup's result (0 found) and the number of 16-bit levels gfx.lvl, mx.lvl and cx.lvl carry. The copy and the lookups stay
// for mode 7, which builds the HFI tables from them.
alignas(8) uint8_t gCmdDb[GPB_CMD_DB_BYTES];
gpb_cmd_db_entry gDbFound[8];
int gDbRc[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
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
    gpb_cmd_db_entry *found = gDbFound;
    int *rc = gDbRc;
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

// Mode 7 (alioth's gmuColdStart). The GMU's SMMU context: the whole adreno SMMU window (DTS kgsl-smmu@3da0000 size 0x10000),
// bank 1 / ASID 1 (Linux gives bank 0 to the GPU), the GMU kernel stream 5 and, for its mask 0x400 (DTS gmu_kernel iommus
// <&kgsl_smmu 0x5 0x400>), 0x405 on the next SMR. The PDC (DTS gmu reg kgsl_gmu_pdc_cfg 0xb290000 size 0x10000).
constexpr uint64_t kSmmuWindow = 0x10000, kPdcPhys = 0x0b290000, kPdcBytes = 0x10000;
constexpr unsigned kGmuBank = 1, kGmuAsid = 1, kGmuSlots[2] = {2, 3}, kGmuSids[2] = {5, 0x405};
// Every mapping below lives for the rest of the boot: the GMU runs out of the memory behind them.
IOMemoryMap *gSmmuMap, *gPdcMap, *gPayloadMaps[5];
A650GmuDevice gDevice;
A650GmuConfig gConfig;
AdrenoHfiStartupConfig gStartup;
A650GmuStartupResult gCold;
bool gColdOk;
extern "C" {
NativeAdrenoGmuContextReport native_gpu_gmu_context;
A650GmuGxResult native_gpu_gx;
}
void contextProgress(uint32_t step) {stage(80 + step);}
volatile uint8_t *gWin[A650GmuWindows];
int gmuRead(void *, unsigned w, size_t at, uint32_t *out) {
    if (w >= A650GmuWindows || !gWin[w] || (at & 3) || at + 4 > gDevice.io.bytes[w]) return 1;
    *out = *reinterpret_cast<volatile uint32_t *>(gWin[w] + at);
    return 0;
}
int gmuWrite(void *, unsigned w, size_t at, uint32_t value) {
    if (w >= A650GmuWindows || !gWin[w] || (at & 3) || at + 4 > gDevice.io.bytes[w]) return 1;
    *reinterpret_cast<volatile uint32_t *>(gWin[w] + at) = value;
    return 0;
}
int gmuBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int gmuNow(void *, uint64_t *us) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    *us = ns / 1000;
    return 0;
}
int gmuWait(void *, unsigned us) {IODelay(us); return 0;}

void gmuColdStart() {
    const gpb_result &r = gResult;
    const gpb_gmu_clock_on &o = gClocks;
    char *ctx = native_gpu_short[1], *load = native_gpu_short[3];
    stage(79);
    if (!&native_gpu_gmu_firmware || !&native_gpu_gmu_firmware_size || !native_gpu_gmu_firmware_size) {
        snprintf(ctx, 48, "CTX no firmware linked");
        return;
    }
    // The bin-0 table is the only one known here: any other bin (or an unread fuse) stops before the GMU is given a table.
    if (o.step != 5 || o.failed || r.smmu != GPB_DONE || r.smr_valid || gDbRc[0] || gDbRc[1] || gDbRc[2] || gDbRc[4] || gDbRc[5] ||
        gDbRc[6] || gDbRc[7] || gGmu.sysreset != 1 || !gFuseRead || gBin != kCasBin) {
        snprintf(ctx, 48, "CTX skip ck%u/%d sm%u smr%u db%d%d%d%d%d%d%d rst%x bin%u%s", o.step, o.failed, unsigned(r.smmu),
                 r.smr_valid, gDbRc[0] != 0, gDbRc[1] != 0, gDbRc[2] != 0, gDbRc[4] != 0, gDbRc[5] != 0, gDbRc[6] != 0,
                 gDbRc[7] != 0, gGmu.sysreset, gBin, gFuseRead ? "" : "?");
        return;
    }
    const uintptr_t smmu = mapRange(GPB_SMMU_BASE, kSmmuWindow, &gSmmuMap);
    if (!smmu) {snprintf(ctx, 48, "CTX nomap"); return;}
    // What SMR/S2CR hold for the GMU's two slots before NativeAdreno programs them, and alioth's S2CR fix: the kit calls a
    // stream slot free only when its S2CR reads exactly 0x00020000, and this SMMU's firmware leaves 0x000203ff in the unused
    // ones (type FAULT with CBNDX 0xff still in the low byte). Writing the canonical FAULT word makes them recognisable; with
    // type FAULT the hardware ignores the rest of the word, so an unmatched stream is treated exactly as before. Only slots
    // already in FAULT are touched (zero would be type TRANSLATE, not free).
    volatile uint32_t *s = reinterpret_cast<volatile uint32_t *>(smmu);
    uint32_t preS2cr[2], preSmr[2];
    for (unsigned i = 0; i < 2; ++i) {
        preSmr[i] = s[0x800 / 4 + kGmuSlots[i]];
        preS2cr[i] = s[0xc00 / 4 + kGmuSlots[i]];
    }
    snprintf(ctx, 48, "CTX pre s2 %x/%x smr %x/%x", preS2cr[0], preS2cr[1], preSmr[0], preSmr[1]);
    for (unsigned i = 0; i < 2; ++i) {
        volatile uint32_t *s2cr = &s[0xc00 / 4 + kGmuSlots[i]];
        if ((*s2cr & (3u << 16)) == (2u << 16)) *s2cr = 0x00020000u;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    NativeAdrenoGmuContextReport &g = native_gpu_gmu_context;
    const IOReturn status = NativeAdrenoGmuContextBringupRoutes(reinterpret_cast<volatile uint8_t *>(smmu), kSmmuWindow, kGmuBank,
        kGmuAsid, kGmuSlots, kGmuSids, 2, contextProgress, &g);
    // program_writes is the kit's own count of streams it finished writing (alioth: phase 5 with writes 1 means the second
    // stream failed).
    snprintf(ctx, 48, "CTX s%u %x k%u/%u r%u p%u/%u s2 %x/%x", g.step, unsigned(status), g.program_status, g.route_status,
             g.routes_verified, g.program_phase, g.program_writes, preS2cr[0], preS2cr[1]);
    if (status) return;
    // The five regions (icache, dummy, debug, log, hfi): the GMU maps them Normal non-cacheable, the CPU writes them combined.
    NativeAdrenoGmuPayload payloads[5];
    const IOReturn got = NativeAdrenoGmuContextPayloads(payloads);
    if (got) {snprintf(load, 48, "LD payloads %x", unsigned(got)); return;}
    A650GmuMemory views[5];
    for (unsigned i = 0; i < 5; ++i) {
        const uintptr_t va = mapRange(payloads[i].physical, payloads[i].bytes, &gPayloadMaps[i], kIOMapWriteCombineCache);
        if (!va) {snprintf(load, 48, "LD nomap payload %u", i); return;}
        views[i] = {reinterpret_cast<volatile uint32_t *>(va), size_t(payloads[i].bytes), payloads[i].iova};
    }
    const uintptr_t pdc = mapRange(kPdcPhys, kPdcBytes, &gPdcMap);
    if (!pdc || !gVa[GPB_WIN_GMU] || !gVa[GPB_WIN_RSCC]) {snprintf(load, 48, "LD nomap window"); return;}
    gWin[A650GmuCore] = reinterpret_cast<volatile uint8_t *>(gVa[GPB_WIN_GMU]);
    gWin[A650GmuRscc] = reinterpret_cast<volatile uint8_t *>(gVa[GPB_WIN_RSCC]);
    gWin[A650GmuPdc] = reinterpret_cast<volatile uint8_t *>(pdc);
    gDevice = {};
    gDevice.io = {nullptr, {A650_GMU_CORE_BYTES, A650_GMU_RSCC_BYTES, A650_GMU_PDC_BYTES}, gmuRead, gmuWrite, gmuBarrier};
    gDevice.services = {nullptr, gmuNow, gmuWait};
    gConfig = {A650_GMU_CHIP_ID, native_gpu_gmu_firmware, native_gpu_gmu_firmware_size, views[0], views[1], views[3], views[4], 100};
    // The GMU's DDR vote for the GPU (A650Gmu.c A650GmuDdrVote): off unless cas-gpu-bw=1, as alioth's pipa-gpu-bw.
    unsigned ddrVote = 0;
    PE_parse_boot_argn("cas-gpu-bw", &ddrVote, sizeof ddrVote);
    A650GmuDdrVote = ddrVote ? 1 : 0;
    const AdrenoHfiStartupStatus built = A650GmuBuildStartup(kCasGpuHz, kCasGpuRpmh, kCasGpuLevels, gDbFound[0].aux,
        gDbFound[0].aux_bytes, gDbFound[1].aux, gDbFound[1].aux_bytes, gDbFound[2].aux, gDbFound[2].aux_bytes, gDbFound[5].address,
        gDbFound[4].address, gDbFound[6].address, gDbFound[7].address, &gStartup);
    if (built) {snprintf(load, 48, "LD vote table %u", unsigned(built)); return;}
    if (gStartup.gpu_count != kCasGpuLevels || gStartup.gpu[kCasPerfMax].hz != kCasGpuHzLimit) {
        snprintf(load, 48, "LD table refused: %u levels", gStartup.gpu_count);
        return;
    }
    snprintf(load, 48, "LD run v%x..%x g%x", gStartup.gpu[1].vote, gStartup.gpu[kCasPerfMax].vote, gStartup.gmu[1].vote);
    stage(90);
    const A650GmuStartupLimits limits = {1000, 100, 50000, 2000000};
    const A650GmuStatus cold = A650GmuColdStart(&gDevice, &gConfig, &gStartup, &limits, &gCold);
    const A650GmuResult &l = gCold.load;
    snprintf(load, 48, "LD %u ph%u v%x i%x h%x r%u%u ao%x c%x", unsigned(cold), l.phase, l.firmware_version,
             l.firmware_init_result, l.hfi_status, l.firmware_ready_observed, l.hfi_ready_observed, l.ao_fault_status,
             l.cm3_fault_status);
    const AdrenoHfiStartupResult &p = gCold.protocol;
    snprintf(native_gpu_short[4], 48, "HFI %u st%u a%x e%x t%u n%u %llums h%u", unsigned(gCold.protocol_status), unsigned(p.stage),
             p.acknowledged_stages, p.firmware_error, unsigned(p.transport_status), p.published_commands,
             static_cast<unsigned long long>(p.elapsed_us / 1000), unsigned(gCold.health_status));
    uint32_t after[5] = {};
    gmuRead(nullptr, A650GmuCore, 0x5000 * 4, &after[0]);   // CM3_SYSRESET
    gmuRead(nullptr, A650GmuCore, 0x501c * 4, &after[1]);   // CM3_FW_INIT_RESULT
    gmuRead(nullptr, A650GmuCore, 0x5180 * 4, &after[2]);   // HFI_CTRL_STATUS
    gmuRead(nullptr, A650GmuCore, 0x50d0 * 4, &after[3]);   // SPTPRAC_PWR_CLK_STATUS (GX)
    gmuRead(nullptr, A650GmuCore, 0x9305 * 4, &after[4]);   // AO_HOST_INTERRUPT_STATUS
    snprintf(native_gpu_short[5], 48, "AFT rst%x i%x hfi%x gx%x ao%x", after[0], after[1], after[2], after[3], after[4]);
    gColdOk = cold == A650GmuSuccess && gDevice.perf_max == kCasPerfMax;
    stage(gColdOk ? 97 : 98);
}

// Mode 8 (alioth's gxOn). GPU register reads through the GPU window (a6xx.xml dword offsets).
constexpr uint32_t kRbbmGbifHalt = 0x0016, kRbbmGbifHaltAck = 0x0017, kRbbmStatus = 0x0210, kGbifHalt = 0x3c45,
                   kGbifHaltAck = 0x3c46;
uint32_t gpuRead(uint32_t dword) {
    uint32_t v = 0xdeadbeef;
    rd(nullptr, GPB_WIN_GPU, dword * 4, &v);
    return v;
}
// alioth's rpmhVoteDdr: MC0 and SH0 at their top level and ACV's bit 3 through active TCS 0/1 of the apps RSC's DRV 2 (downstream
// DTS apps_rsc drv-2 at 0x18220000, qcom,tcs-offset 0xd00), as Linux's rpmh-rsc sends an active-only request. On alioth it is on by
// default (pipa-ddr-vote); here it runs only with cas-ddr-vote=1: it is a speed-up, not part of GX power, and holds the DDR at
// its top level. Row 4.
void rpmhVoteDdr() {
    unsigned vote = 0;
    PE_parse_boot_argn("cas-ddr-vote", &vote, sizeof vote);
    if (!vote) return;
    char *row = native_gpu_short[4];
    const uint32_t mc0 = gDbFound[5].address, sh0 = gDbFound[4].address, acv = gDbFound[6].address;
    if (!mc0 || !sh0 || !acv) {snprintf(row, 48, "DDR no BCM addresses %x %x %x", mc0, sh0, acv); return;}
    IOMemoryMap *map = nullptr;
    volatile uint32_t *drv = reinterpret_cast<volatile uint32_t *>(mapRange(0x18220000, 0x10000, &map));
    if (!drv) {snprintf(row, 48, "DDR no mapping"); return;}
    auto r = [&](uint32_t off) {return drv[off / 4];};
    auto w = [&](uint32_t off, uint32_t v) {drv[off / 4] = v; __asm__ volatile("dsb sy" ::: "memory");};
    const uint32_t config = r(0x0C);                                   // DRV_PRNT_CHLD_CONFIG
    const uint32_t tcsCount = (config >> 12) & 0x3f, perTcs = (config >> 27) & 0x1f;   // drv-id 2: bits 17:12; NCPT bits 31:27
    constexpr uint32_t kTcsBase = 0xd00, kTcsStride = 672, kCmdStride = 20;
    auto tcs = [&](unsigned id, uint32_t reg) {return kTcsBase + kTcsStride * id + reg;};
    unsigned id = 0;
    for (; id < 2; ++id) if (r(tcs(id, 0x18))) break;                 // RSC_DRV_STATUS nonzero: idle
    if (id == 2 || tcsCount < 2 || perTcs < 3) {
        snprintf(row, 48, "DDR refused %x %x %x", config, r(tcs(0, 0x18)), r(tcs(1, 0x18)));
        map->release();
        return;
    }
    const uint32_t top = (1u << 30) | (1u << 29) | (0x3fffu << 14) | 0x3fffu, acvOn = (1u << 30) | (1u << 29) | 0x8u;
    const uint32_t addrs[3] = {mc0, sh0, acv}, datas[3] = {top, top, acvOn};
    w(tcs(id, 0x1C), 0);                                                // RSC_DRV_CMD_ENABLE
    w(kTcsBase + 0x00, r(kTcsBase + 0x00) | (1u << id));                 // RSC_DRV_IRQ_ENABLE: the completion latches in IRQ_STATUS
    for (unsigned c = 0; c < 3; ++c) {
        w(tcs(id, 0x30) + kCmdStride * c, 8u | (1u << 16) | (1u << 8));  // MSGID: length 8, write, response required
        w(tcs(id, 0x34) + kCmdStride * c, addrs[c]);
        w(tcs(id, 0x38) + kCmdStride * c, datas[c]);
    }
    w(tcs(id, 0x1C), 7);
    uint32_t control = r(tcs(id, 0x14));                                 // RSC_DRV_CONTROL: AMC trigger off, AMC off, AMC on, trigger
    control &= ~(1u << 24); w(tcs(id, 0x14), control);
    control &= ~(1u << 16); w(tcs(id, 0x14), control);
    w(tcs(id, 0x14), 1u << 16);
    w(tcs(id, 0x14), (1u << 16) | (1u << 24));
    unsigned polls = 0;
    for (; polls < 2000 && !(r(kTcsBase + 0x04) & (1u << id)); ++polls) IODelay(5);   // RSC_DRV_IRQ_STATUS
    uint32_t status[3];
    for (unsigned c = 0; c < 3; ++c) status[c] = r(tcs(id, 0x3C) + kCmdStride * c);
    w(kTcsBase + 0x08, 1u << id);                                        // RSC_DRV_IRQ_CLEAR
    w(tcs(id, 0x1C), 0);
    w(kTcsBase + 0x00, r(kTcsBase + 0x00) & ~(1u << id));
    snprintf(row, 48, "DDR t%u p%u %05x %05x %05x", id, polls, status[0], status[1], status[2]);
    map->release();
}

void gxOn() {
    // The vote: cas-gpu-perf=<n>, default 1 (305 MHz), never above the bin-0 table's top (6 = 587 MHz) nor the device's perf_max.
    unsigned perf = kCasPerfDefault, asked = 0;
    if (PE_parse_boot_argn("cas-gpu-perf", &asked, sizeof asked)) perf = asked < 1 ? 1 : asked > kCasPerfMax ? kCasPerfMax : asked;
    if (perf > gDevice.perf_max) perf = gDevice.perf_max;
    const unsigned mhz = unsigned(kCasGpuHz[perf] / 1000000);
    stage(100);
    A650GmuGxResult &g = native_gpu_gx;
    const A650GmuStatus status = perf >= 1 && kCasGpuHz[perf] <= kCasGpuHzLimit ? A650GmuPowerGx(&gDevice, perf, &g) : A650GmuArgument;
    uint32_t gdscr = 0;
    rd(nullptr, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &gdscr);
    char *row = native_gpu_short[5];
    if (!g.gx_on) {
        snprintf(row, 48, "GX off %u p%u %uM v%u/%x o%u %x>%x gd%x", unsigned(status), perf, mhz, g.vote_status, g.vote_error,
                 g.oob_polls, g.gx_before, g.gx_after, gdscr);
    } else {
        stage(101);
        snprintf(row, 48, "GX on p%u %uM v%u o%u %x>%x gd%x", perf, mhz, g.vote_status, g.oob_polls, g.gx_before, g.gx_after, gdscr);
        const int32_t t0 = sensor(kTsens0Tm, 15), t1 = sensor(kTsens1Tm, 8);
        snprintf(native_gpu_short[2], 48, "GX2 rb%x gbif %x/%x rbbm %x/%x T%d/%d", gpuRead(kRbbmStatus), gpuRead(kGbifHalt),
                 gpuRead(kGbifHaltAck), gpuRead(kRbbmGbifHalt), gpuRead(kRbbmGbifHaltAck), (int)t0 / 10, (int)t1 / 10);
    }
    rpmhVoteDdr();
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
    gFuseRead = q != 0;
    gBin = (fuse >> (8 * (kSpeedBinByte & 3)) >> 5) & 7;
    const int32_t t0 = sensor(kTsens0Tm, 15), t1 = sensor(kTsens1Tm, 8);
    stage(4);
    snprintf(native_gpu_short[1], 48, "CC cx %08x>%08x smmu %08x>%08x", r.cx_before, r.cx_gdscr, r.cc_smmu_before, r.cc_smmu);
    snprintf(native_gpu_short[2], 48, "SMMU %08x smr%u gfsr %x rb %x T%d/%d", r.id0, r.smr_valid, r.gfsr, r.rbbm_status,
             (int)t0 / 10, (int)t1 / 10);
    if (gMode >= 5) {stage(50); commandDb(native_gpu_short[3]);}
    uint32_t cx_gmu = r.cc_gmu, ahb = r.cc_ahb;
    if (gMode >= 6) {
        stage(70);
        gpb_gmu_clock_on &o = gClocks;
        gpb_gmu_clocks_on(&io, &r, &o);
        snprintf(native_gpu_short[4], 48, "CLK s%u f%d gmu %08x cfg %08x ahb %08x", o.step, o.failed, o.cx_gmu, o.gmu_cfg, o.ahb);
        cx_gmu = o.cx_gmu; ahb = o.ahb;
        if (!gpb_gmu_regs_readable(&r, cx_gmu, ahb)) snprintf(native_gpu_short[5], 48, "GMU unreadable");
        else {
            stage(60);
            gpb_read_gmu_state(&io, &r, &gGmu);
            snprintf(native_gpu_short[5], 48, "GMU rst %08x fw %08x rscc %x/%x", gGmu.sysreset, gGmu.fw_init, gGmu.rscc_busy,
                     gGmu.rscc_status);
        }
    }
    if (gMode >= 7) gmuColdStart();
    if (gMode >= 8 && gColdOk) gxOn();
    stage(99);
}
}  // namespace

extern "C" void cas_gpu_probe(void) {
    unsigned mode = 0;
    // Modes 9 and above (alioth's GPU command path) are not ported; mode 4 does not exist.
    if (!PE_parse_boot_argn("cas-gpu", &mode, sizeof mode) || !mode || mode == 4 || mode > 8 || native_gpu_step) return;
    gMode = mode;
    native_gpu_step = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(gpuThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
