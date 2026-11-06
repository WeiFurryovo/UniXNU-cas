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
//   9  as 8, then, only with GX on (alioth's mode 9: gpuContext + cpStart, f188378 GpuProbe.cpp:707-757 and 961-1036): the GPU's
//      own SMMU context through NativeAdreno (bank 0 / ASID 2; the GPU streams 0, 1, 0x400, 0x401 of the DT's gfx3d_user iommus
//      <&kgsl_smmu 0 0x401> on SMR 0, 1, 4 and 5, their firmware FAULT S2CRs canonicalised first; then CB0 ACTLR without CPRE,
//      TLBIALL and TLBSYNC), NativeAdreno's five kernel buffers mapped write-combined, the SQE (a650_sqe.fw without its first
//      dword) copied and read back, Linux v6.6 a6xx hw_init for the A650 (alioth hwInit, :817-874: GBIF/RBBM unhalt, no TSB
//      trusted range, 64-bit ADDR_MODE, a650_hwcg clock gating, GBIF QSB, UCHE ranges, CP ROQ, UBWC, CP protect, APRIV and
//      RBBM_INT_0_MASK), CP_SQE_INSTR_BASE, ring 0 (32 KiB; NativeAdrenoRing at FFT-infra/Adreno650@70771a7 = alioth 059b678's
//      fixed ring: CP_RB_CNTL without NO_UPDATE, as Linux on a part with hardware APRIV, the RPTR read from the CP's shadow in the
//      memptrs page; CP_RB_WPTR not zeroed), CP_SQE_CNTL = 1, CP_ME_INIT (a6xx_cp_init, eight words) and its fence, then one
//      CP_MEM_WRITE of 0xa6500c0d into the scratch page that the CPU reads back.
//  10  as 9, then (alioth's leaveSecure + gpuInterrupt, :1038-1238): the zap shader through PAS 13 (DT qcom,kgsl-hyp) into the DT's
//      no-map pil_gpu_region@8681a000 (0x2000): this phone's own a650_zap.elf (it differs from alioth's), laid out by its own program
//      headers as qcom_mdt_load does (metadata = ELF header + program headers + the hash segment, in a physically contiguous
//      buffer below 2^40; cas's image: one relocatable segment at 0x5000, 0x68c bytes, so mem_setup gets 0x1000), SMC
//      init_image, mem_setup, the segment copy, auth_and_reset (IRQs masked across each call); then CP_SET_SECURE_MODE(0) through
//      the ring; then the GPU interrupt (DT kgsl_3d0_irq: SPI 300, level high) through ArmCpu's armcpu_spi_attach, and a CP_NOP
//      whose fence must raise it (the fence waits sleep on it from then on).
// Modes 11 and above (alioth's user clients and Metal) are not ported: cas_gpu_probe refuses them.
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
// on the GPU SMMU (SMR 5 and sCR0, its "PRE" row) are diagnostics of a past failure and are left out. Mode 9 writes the GPU SMMU
// (S2CR 0/1/4/5 if FAULT, context bank 0, SMR/S2CR 0/1/4/5, CB0 ACTLR/TLBIALL/TLBSYNC), the GPU register space (hw_init, CP_SQE_*,
// CP_RB_*, CP_SCRATCH), the GMU's SPTPRAC clock control and CX busy counter (hw_init), and the five kernel buffers. Mode 10 adds
// the PIL SMCs (TrustZone then owns 0x8681a000..0x8681bfff), CP packets and the GIC distributor's SPI 300 configuration.
// Not ported: alioth's UCHE row and its register dump to the log (reads only), its boot-arg knobs (pipa-gpu-hwcg, -gmem16m,
// -apriv: here hw_init always runs the table, Linux's GMEM base 0 and APRIV on; the driver's own pipa-gpu-no-update stays unset)
// and the write-combine switches that only user clients (mode 11) read.
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
// Modes 9-10 reuse the rows of stages that have already succeeded (mode 9 runs only after a complete cold start and GX on, so the
// CTX, GX2, LD and HFI rows are spent by then); row 0 keeps the summary and step, row 5 keeps the GX row (the level voted). A
// failing stage leaves its own row as the last word, and rows of a stage that never ran are not touched.
//   3 GCTX s<step> <IOReturn> k<program>/<route status> r<routes> p<phase>/<writes> a<CB0 ACTLR before>     (replaces LD)
//     success: GCTX s7 0 k0/0 r4 p<n>/<n> a<x>  (s7 = Ready, r4 = four streams verified)
//   1 CP <what> n<step> q<SQE bytes>/<readback mismatch> r<ring attach> i<CP init>/<its fence> w<marker fence> m<marker>
//     (replaces CTX) success: CP ok n7 q31628/0 r0 i0/0 w0 ma6500c0d   (what: ok, nobuf, nomap, nosqe, sqerb, noring, attach,
//     init, initf, pm4, submit, mem; n = alioth's cp step 1-7)
//   2 CPS rb<RBBM_STATUS> f<SMMU CB0 FSR> hf<CP_HW_FAULT> ci<CP_INTERRUPT_STATUS> r<CP_RB_RPTR> w<CP_RB_WPTR> i<RBBM_INT_0_STATUS>
//     (replaces GX2; written with the CP row, and again at the end of mode 10) success: rb with bit 23 clear (idle), f0 hf0 ci0,
//     r == w; i may show bit 20 (CACHE_FLUSH_TS) in mode 9, where nothing clears it
//   3 ZAP s<step> i<init_image x0>/<x1> m<mem_setup x0>/<x1> a<auth_and_reset x0>/<x1> se<secure-mode fence> r<reloc> q<resumes>
//     (mode 10, replaces GCTX) success: ZAP s7 i0/0 m0/0 a0/0 se0 r1 q0   (s: 1 start, 2 metadata, 3 init_image, 4 mem_setup,
//     5 copy, 6 auth_and_reset, 7 authenticated; negative = refused before any SMC: -20 region inside XNU's memory, -21 no
//     firmware, -1..-10 ELF/metadata/map checks; 3, 4 or 6 = that SMC failed; se: - not sent, 0 the CP left secure mode,
//     1 timeout, -1 submit failed)
//   4 IRQ <ok|bad> a<attach IOReturn> g<GIC readback> c<count before>><after> f<NOP fenced> s<status bits seen> n<status now> m<masked>
//     (mode 10, replaces HFI) success: IRQ ok a0 ga003 c<n>><more than n> f1 s100000 n0 m0  (ga003: group 1, enabled, level,
//     priority 0xa0, ga002 if the secure side owns the group bit; s bit 20 = CACHE_FLUSH_TS; ok needs attach 0, the fence and at
//     least one more interrupt during the NOP)
// Steps (row 0, native_gpu_step): 1-4 maps/run/fuse, 50 cmd-db, 70 clocks, 60 GMU state, 79 cold start entry, 80+n SMMU context
// step n, 90 cold start running, 97 cold start done / 98 failed, 100 GX vote, 101 GX on, 110+n GPU SMMU context step n, 120 CP
// buffers, 121 hw_init, 122 ring, 123 CP_ME_INIT, 124 marker, 125 marker read back, 130-134 zap (metadata, init_image, mem_setup,
// copy, auth_and_reset), 135 secure-mode exit, 140 interrupt, 141 interrupt ok, 99 finished.
// The rows are written 5 s after the late probe, as alioth does, so the display already runs if a read hangs the bus.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "GpuPowerBoard.h"
#include "A650Gmu.h"
#include "AdrenoPM4.h"
int snprintf(char *, size_t, const char *, ...);
char native_gpu_short[6][48];
volatile uint32_t native_gpu_step;
// The firmware object generated at build time from this phone's vendor partition (scripts/build-adreno650.py); never committed.
extern const uint8_t native_gpu_gmu_firmware[] __attribute__((weak_import));
extern const uint32_t native_gpu_gmu_firmware_size __attribute__((weak_import));
extern const uint8_t native_gpu_sqe_firmware[] __attribute__((weak_import));
extern const uint32_t native_gpu_sqe_firmware_size __attribute__((weak_import));
extern const uint8_t native_gpu_zap_firmware[] __attribute__((weak_import));
extern const uint32_t native_gpu_zap_firmware_size __attribute__((weak_import));
// Mode 10: the physical range XNU manages (osfmk/arm/pmap/pmap.c), the interrupt state, and ArmCpu's SPI attach (weak, as
// ButtonProbe.cpp takes it).
extern uint64_t vm_first_phys, vm_last_phys;
boolean_t ml_set_interrupts_enabled(boolean_t);
boolean_t ml_get_interrupts_enabled(void);
boolean_t ml_at_interrupt_context(void);
int get_preemption_level(void);
IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback,
                           uint64_t *router) __attribute__((weak_import));
// NativeAdrenoMemory.cpp records the steps of a BO's release here; NativeAdrenoPublish.cpp (the user-client half, mode 11)
// defines them. That object is not in this archive, so these weak stand-ins do (a strong definition, once added, wins).
__attribute__((weak)) volatile uint32_t native_adreno_close_step[8];
__attribute__((weak)) void native_adreno_close_step_set(uint32_t n) {native_adreno_close_step[0] = n;}
// NativeAdrenoMemory.cpp's release path asks the kernel which process maps a page (weak_import; a static kernel link refuses an
// undefined one). This kernel has no such probe: 0 = "no process maps it", as UniXNU-alioth's BootProgressStubs.c defines it.
__attribute__((weak)) unsigned int native_pmap_user_mappings(unsigned int, void **, uint64_t *, char *) {return 0;}
}
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"

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

// Mode 9 (alioth f188378 GpuProbe.cpp:653-705, 707-757, 759-874, 961-1036). The rows it takes over (see the header).
constexpr unsigned kRowCp = 1, kRowCpState = 2, kRowGpuCtx = 3, kRowZap = 3, kRowIrq = 4;
// GPU register access for NativeAdreno (dword indices) through the GPU window.
int naWrite(void *, uint32_t reg, uint32_t value) {
    if (reg >= GPB_GPU_WINDOW / 4 || !gVa[GPB_WIN_GPU]) return 1;
    *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPU] + reg * 4) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
int naRead(void *, uint32_t reg, uint32_t *value) {
    if (reg >= GPB_GPU_WINDOW / 4 || !gVa[GPB_WIN_GPU]) return 1;
    *value = *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPU] + reg * 4);
    return 0;
}
int naBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
// The kernel buffers are Normal non-cacheable for the GPU and write-combined for the CPU: ordering only, no cache maintenance.
int naPublish(void *, const volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int naAcquire(void *, volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
uint64_t naNow(void *) {uint64_t us = 0; gmuNow(nullptr, &us); return us;}
// The fence waits' pause: with the GPU interrupt attached (mode 10) a waiter that may block sleeps until the next GPU interrupt or
// 100 us, whichever is first; before that, or where it may not block, it spins 20 us.
volatile bool gIrqAttached;
volatile uint32_t gIrqCount, gIrqSeen, gIrqLast, gIrqRepeat, gIrqMasked;
int naIdle(void *) {
    if (gIrqAttached && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && get_preemption_level() == 0) {
        assert_wait_timeout(event_t(&gIrqCount), THREAD_UNINT, 100, NSEC_PER_USEC);
        thread_block(THREAD_CONTINUE_NULL);
    } else {
        IODelay(20);
    }
    return 0;
}
// No AHB fence status: the A650's GMU runs without IFPC here, so the GPU domain stays up and a WPTR write is not dropped.
NativeAdrenoRingIo gRio{nullptr, naWrite, naRead, naBarrier, nullptr, nullptr};
NativeAdrenoRingCache gCache{nullptr, naPublish, naAcquire};
NativeAdrenoRingClock gClock{nullptr, naNow, naIdle};
void gw(uint32_t dword, uint32_t value) {naWrite(nullptr, dword, value);}
void gw64(uint32_t dword, uint64_t value) {gw(dword, uint32_t(value)); gw(dword + 1, uint32_t(value >> 32));}
uint32_t smmuRead(uint32_t at) {
    if (!gSmmuMap || at >= kSmmuWindow) return 0xdeadbeef;
    return *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + at);
}
void smmuWrite(uint32_t at, uint32_t v) {
    if (!gSmmuMap || at >= kSmmuWindow) return;
    *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + at) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
// IDR1: PAGESIZE (bit 31) and NUMPAGENDXB (bits 30:28): the context banks start after 2^(NUMPAGENDXB + 1) pages. 0 if that lies
// outside the mapped window (alioth reads it unchecked; here no bank register is touched then).
uint32_t smmuBank0() {
    const uint32_t id1 = smmuRead(0x24);
    const uint32_t bank0 = ((id1 >> 31) ? 0x10000u : 0x1000u) << (((id1 >> 28) & 7) + 1);
    return bank0 + 0x1000 <= kSmmuWindow ? bank0 : 0;
}

extern "C" {
NativeAdrenoGmuContextReport native_gpu_context;
uint32_t native_gpu_cp[24];
}
constexpr unsigned kGpuBank = 0, kGpuAsid = 2, kGpuSlots[4] = {0, 1, 4, 5}, kGpuSids[4] = {0, 1, 0x400, 0x401};
NativeAdrenoGpuBuffer gNb[5];
IOMemoryMap *gNbMaps[5];
volatile uint8_t *gNbCpu[5];
NativeAdrenoRing *gRing;
NativeAdrenoCpInit *gCpInit;
void naProgress(uint32_t step) {stage(110 + step);}

// The GPU's context: bank 0 (arm-smmu-qcom gives the GPU bank 0), the four streams of iommus <0 0x401> (stream 0 under the mask
// 0x401) on the SMR slots the GMU does not use, NativeAdreno's aperture of 4 KiB pages and its five kernel buffers. The table
// walks are non-coherent (cleaned tables), which works whether or not the walker snoops.
bool gpuContext() {
    char *row = native_gpu_short[kRowGpuCtx];
    stage(110);
    // The same S2CR normalisation the GMU's slots got: only slots in FAULT are rewritten to the canonical FAULT word.
    volatile uint32_t *s = reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress());
    for (unsigned i = 0; i < 4; ++i) {
        volatile uint32_t *s2cr = &s[0xc00 / 4 + kGpuSlots[i]];
        if ((*s2cr & (3u << 16)) == (2u << 16)) *s2cr = 0x00020000u;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    NativeAdrenoGmuContextReport &g = native_gpu_context;
    const IOReturn ctx = NativeAdrenoGpuContextBringupRoutes(reinterpret_cast<volatile uint8_t *>(gSmmuMap->getVirtualAddress()),
        kSmmuWindow, kGpuBank, kGpuAsid, kGpuSlots, kGpuSids, 4, naProgress, &g);
    // CB0 ACTLR without CPRE (arm_mmu500_reset, MMU-500 errata 841119/826419), then TLBIALL, TLBSYNC and TLBSTATUS of the bank.
    uint32_t actlr = 0;
    const uint32_t bank0 = smmuBank0();
    if (!ctx && bank0) {
        actlr = smmuRead(bank0 + 0x4);
        smmuWrite(bank0 + 0x4, actlr & ~2u);
        smmuWrite(bank0 + 0x618, 0);
        smmuWrite(bank0 + 0x7f0, 0);
        for (unsigned i = 0; i < 1000 && (smmuRead(bank0 + 0x7f4) & 1); ++i) IODelay(1);
    }
    snprintf(row, 48, "GCTX s%u %x k%u/%u r%u p%u/%u a%x%s", g.step, unsigned(ctx), g.program_status, g.route_status,
             g.routes_verified, g.program_phase, g.program_writes, actlr, bank0 ? "" : " nobank");
    return ctx == kIOReturnSuccess && bank0;
}

// CP register protection of the A650: Linux v6.6 a650_protect, encoded as A6XX_PROTECT_RDONLY (start | (count - 1) << 18) and
// A6XX_PROTECT_NORDWR (that | 1 << 31); the last entry goes into CP_PROTECT(47) with an unbounded range.
constexpr uint32_t rdonly(uint32_t reg, uint32_t len) {return ((len & 0x3FFF) << 18) | (reg & 0x3FFFF);}
constexpr uint32_t nordwr(uint32_t reg, uint32_t len) {return (1u << 31) | rdonly(reg, len);}
constexpr uint32_t kProtect[] = {
    rdonly(0x00000, 0x04ff), rdonly(0x00501, 0x0005), rdonly(0x0050b, 0x02f4), nordwr(0x0050e, 0x0000), nordwr(0x00510, 0x0000),
    nordwr(0x00534, 0x0000), nordwr(0x00800, 0x0082), nordwr(0x008a0, 0x0008), nordwr(0x008ab, 0x0024), rdonly(0x008de, 0x00ae),
    nordwr(0x00900, 0x004d), nordwr(0x0098d, 0x0272), nordwr(0x00e00, 0x0001), nordwr(0x00e03, 0x000c), nordwr(0x03c00, 0x00c3),
    rdonly(0x03cc4, 0x1fff), nordwr(0x08630, 0x01cf), nordwr(0x08e00, 0x0000), nordwr(0x08e08, 0x0000), nordwr(0x08e50, 0x001f),
    nordwr(0x08e80, 0x027f), nordwr(0x09624, 0x01db), nordwr(0x09e60, 0x0011), nordwr(0x09e78, 0x0187), nordwr(0x0a630, 0x01cf),
    nordwr(0x0ae02, 0x0000), nordwr(0x0ae50, 0x032f), nordwr(0x0b604, 0x0000), nordwr(0x0b608, 0x0007), nordwr(0x0be02, 0x0001),
    nordwr(0x0be20, 0x17df), nordwr(0x0f000, 0x0bff), rdonly(0x0fc00, 0x1fff), nordwr(0x18400, 0x1fff), nordwr(0x1a800, 0x1fff),
    nordwr(0x1f400, 0x0443), rdonly(0x1f844, 0x007b), nordwr(0x1f887, 0x001b), nordwr(0x1f8c0, 0x0000),
};
constexpr unsigned kProtectCount = sizeof kProtect / sizeof kProtect[0];
static_assert(kProtectCount == 39, "a650_protect has 39 entries");

// Linux v6.6 a650_hwcg (a6xx_gpu.c), as register offsets of a6xx.xml.
constexpr uint32_t kHwcg[][2] = {
    {0x00b0, 0x02222222}, {0x00b4, 0x02222220}, {0x00b8, 0x00000080}, {0x00bc, 0x0000f3cf},   // SP0 CNTL, CNTL2, DELAY, HYST
    {0x00c0, 0x02222222}, {0x00c4, 0x22222222}, {0x00c8, 0x22222222}, {0x00cc, 0x00022222},   // TP0 CNTL..CNTL4
    {0x00d0, 0x11111111}, {0x00d4, 0x11111111}, {0x00d8, 0x11111111}, {0x00dc, 0x00011111},   // TP0 DELAY..DELAY4
    {0x00e0, 0x77777777}, {0x00e4, 0x77777777}, {0x00e8, 0x77777777}, {0x00ec, 0x00077777},   // TP0 HYST..HYST4
    {0x00f0, 0x22222222}, {0x00f4, 0x01002222}, {0x00f8, 0x00002220}, {0x0100, 0x00040f00},   // RB0 CNTL, CNTL2, CCU0, RB_CCU0 HYST
    {0x0104, 0x25222022}, {0x0105, 0x00005555}, {0x0106, 0x00000011}, {0x0107, 0x00445044},   // RAC CNTL, CNTL2, DELAY, HYST
    {0x0108, 0x04222222}, {0x0111, 0x00002222}, {0x0114, 0x00222222}, {0x0117, 0x00000002},   // TSE_RAS_RBBM, MODE VFD, GPC, DELAY HLSQ_2
    {0x011b, 0x00002222}, {0x0109, 0x00004000}, {0x0112, 0x00002222}, {0x0115, 0x00000200},   // MODE HLSQ, DELAY TSE_RAS_RBBM, VFD, GPC
    {0x011c, 0x00000000}, {0x010a, 0x00000000}, {0x0113, 0x00000000}, {0x0116, 0x04104004},   // DELAY HLSQ, HYST TSE_RAS_RBBM, VFD, GPC
    {0x011d, 0x00000000}, {0x0120, 0x00000222}, {0x0121, 0x00000111}, {0x0122, 0x00000777},   // HYST HLSQ, TEX_FCHE CNTL, DELAY, HYST
    {0x010b, 0x22222222}, {0x0110, 0x00000004}, {0x010f, 0x00000002}, {0x0533, 0x00000182},   // UCHE CNTL, HYST, DELAY; ISDB_CNT
    {0x0044, 0x00000000}, {0x0042, 0x00000000}, {0x0118, 0x00000222}, {0x0119, 0x00000111},   // RAC_THRESHOLD, SP_HYST; GMU_GX CNTL, DELAY
    {0x011a, 0x00000555},                                                                     // GMU_GX HYST
};
static_assert(sizeof kHwcg / sizeof kHwcg[0] == 49, "a650_hwcg has 49 entries");

// a6xx_set_hwcg of Linux v6.6: with the SP clock off (GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL bit 0), the table, then the SP clock on
// and RBBM_CLOCK_CNTL. alioth: without it no shader wrote anything.
void setHwcg() {
    uint32_t *n = native_gpu_cp;
    n[9] = gpuRead(0x00ae);                                             // RBBM_CLOCK_CNTL as found
    gmuRead(nullptr, A650GmuCore, 0x80 * 4, &n[10]);                     // GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL as found
    gmuWrite(nullptr, A650GmuCore, 0x80 * 4, n[10] & ~1u);
    for (const auto &r : kHwcg) gw(r[0], r[1]);
    uint32_t sptp = 0;
    gmuRead(nullptr, A650GmuCore, 0x80 * 4, &sptp);
    gmuWrite(nullptr, A650GmuCore, 0x80 * 4, sptp | 1u);
    gw(0x00ae, 0x8aa8aa82);                                             // RBBM_CLOCK_CNTL
    gmuRead(nullptr, A650GmuCore, 0x80 * 4, &n[11]);
    n[12] = gpuRead(0x00ae);
}

// hw_init of Linux v6.6 a6xx_gpu.c for the A650 (a650 family, with a GMU); register offsets from a6xx.xml / a6xx_gmu.xml.
void hwInit() {
    gw(0x3C45, 0); (void)gpuRead(0x3C45);                               // GBIF_HALT
    gw(0x0016, 0); (void)gpuRead(0x0016);                               // RBBM_GBIF_HALT
    gw(0xF803, 0);                                                      // RBBM_SECVID_TSB_CNTL
    gw64(0xF800, 0); gw(0xF802, 0);                                     // TSB_TRUSTED_BASE, _SIZE: no trusted range
    static const uint32_t addrMode[] = {0x842, 0xC01, 0x8601, 0x8E05, 0x9E01, 0xBE05, 0xA601, 0x9601, 0xE00, 0xAE01, 0xB601, 0xF810};
    for (uint32_t reg : addrMode) gw(reg, 1);                           // *_ADDR_MODE_CNTL: 64-bit
    setHwcg();
    for (uint32_t reg = 0x3C03; reg <= 0x3C06; ++reg) gw(reg, 0x00071620);   // GBIF_QSB_SIDE0..3 (a650 family)
    gw(0x0011, 0x3);                                                    // RBBM_GBIF_CLIENT_QOS_CNTL
    gw(0x050B, 0xffffffff);                                             // RBBM_PERFCTR_GPU_BUSY_MASKED
    gw64(0xE05, 0x0001ffffffffffc0ull);                                 // UCHE_WRITE_RANGE_MAX
    gw64(0xE09, 0x0001fffffffff000ull);                                 // UCHE_TRAP_BASE
    gw64(0xE07, 0x0001fffffffff000ull);                                 // UCHE_WRITE_THRU_BASE
    // UCHE_GMEM_RANGE_MIN/MAX are left alone, as Linux does on the a650 family (GMEM base 0).
    gw(0xE18, 0x804); gw(0xE17, 0x4);                                   // UCHE_FILTER_CNTL, UCHE_CACHE_WAYS
    gw(0x8C2, 0x02000140); gw(0x8C1, 0x8040362c);                       // CP_ROQ_THRESHOLDS_2/_1 (a640/a650 family)
    gw(0x8C3, 128);                                                     // CP_MEM_POOL_SIZE
    gw(0x9E00, 0x00300200);                                             // PC_DBG_ECO_CNTL (A650)
    gw(0x98D, 1);                                                       // CP_AHB_CNTL: error response
    gw(0x500, 1);                                                       // RBBM_PERFCTR_CNTL
    gw(0x8D0, 0);                                                       // CP_PERFCTR_CP_SEL(0): PERF_CP_ALWAYS_COUNT
    // a6xx_set_ubwc_config for the A650: hbb_lo 3 (highest bank bit 16: cas's DT qcom,highest-bank-bit = <0x10> too), amsbc 1,
    // rgb565 predicator 1, uavflagprd_inv 2, min_acc_len 0, ubwc_mode 0.
    gw(0x8E08, 0x816); gw(0xB604, 0x6); gw(0xAE02, 0x26);               // RB, TPL1, SP _NC_MODE_CNTL
    gw(0xE01, 3u << 21);                                                // UCHE_MODE_CNTL
    gw(0x001F, (1u << 30) | 0x1fffff);                                  // RBBM_INTERFACE_HANG_INT_CNTL
    gw(0xE19, 1);                                                       // UCHE_CLIENT_PF
    gw(0xB608, 0); gw(0xB609, 0x3fe05ff4); gw(0xB60A, 0x3fa0ebee);       // TPL1_BICUBIC_WEIGHTS_TABLE_0..4 (a650 family)
    gw(0xB60B, 0x3f5193ed); gw(0xB60C, 0x3f0243f0);
    // The CX GMU counter 0 counts busy ticks; the power counter on (GMU registers).
    gmuWrite(nullptr, A650GmuCore, 0x930e * 4, 0xff000000);              // GPU_GMU_AO_GPU_CX_BUSY_MASK
    uint32_t sel = 0;
    gmuRead(nullptr, A650GmuCore, 0x5041 * 4, &sel);
    gmuWrite(nullptr, A650GmuCore, 0x5041 * 4, (sel & ~0xffu) | (1u << 5));   // GMU_CX_GMU_POWER_COUNTER_SELECT_0
    gmuWrite(nullptr, A650GmuCore, 0x5040 * 4, 1);                       // GMU_CX_GMU_POWER_COUNTER_ENABLE
    // a6xx_set_cp_protect: enable, fault on violation, last span unbounded.
    gw(0x84F, (1u << 0) | (1u << 1) | (1u << 3));                       // CP_PROTECT_CNTL
    for (unsigned i = 0; i + 1 < kProtectCount; ++i) gw(0x850 + i, kProtect[i]);
    gw(0x850 + 47, kProtect[kProtectCount - 1]);                        // CP_PROTECT(47)
    // CP_APRIV_CNTL as Linux on the A650 (hw_apriv): the CP's own instruction, ring and rptr accesses as privileged. alioth: without
    // it every shader read ended in UCHE_OOB_ACCESS; and with APRIV the ring runs without NO_UPDATE (NativeAdrenoRing::program).
    gw(0x844, (1u << 6) | (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1));
    gw(0x038, NativeAdrenoIrq::maskA6xx());                             // RBBM_INT_0_MASK: A6XX_INT_MASK
}

// The CP rows: kRowCp (what, alioth's step and results) and kRowCpState (RBBM_STATUS, CP_RB_RPTR, CP_RB_WPTR, CP_HW_FAULT,
// CP_INTERRUPT_STATUS, RBBM_INT_0_STATUS, SMMU CB0 FSR).
void cpStateRow() {
    const uint32_t bank0 = smmuBank0();
    snprintf(native_gpu_short[kRowCpState], 48, "CPS rb%x f%x hf%x ci%x r%x w%x i%x", gpuRead(0x210),
             bank0 ? smmuRead(bank0 + 0x58) : 0xdeadbeef, gpuRead(0x821), gpuRead(0x823), gpuRead(0x806), gpuRead(0x807),
             gpuRead(0x201));
}
void cpRow(const char *what) {
    const uint32_t *n = native_gpu_cp;
    snprintf(native_gpu_short[kRowCp], 48, "CP %s n%u q%u/%u r%x i%x/%x w%x m%x", what, n[0], n[1], n[2], n[3], n[4], n[5], n[7],
             n[8]);
    cpStateRow();
}
// SQE, hw_init, ring 0, CP_ME_INIT, then a CP_MEM_WRITE into the scratch page that the CPU reads back.
bool cpStart() {
    uint32_t *n = native_gpu_cp;
    stage(120);
    n[0] = 1;
    if (NativeAdrenoGpuContextBuffers(gNb)) {cpRow("nobuf"); return false;}
    for (unsigned i = 0; i < 5; ++i) {
        const uintptr_t va = mapRange(gNb[i].physical, gNb[i].bytes, &gNbMaps[i], kIOMapWriteCombineCache);
        if (!va) {cpRow("nomap"); return false;}
        gNbCpu[i] = reinterpret_cast<volatile uint8_t *>(va);
    }
    // The SQE without its first dword (adreno_fw_create_bo), read back.
    n[0] = 2;
    if (!&native_gpu_sqe_firmware || !&native_gpu_sqe_firmware_size || native_gpu_sqe_firmware_size < 8 ||
        native_gpu_sqe_firmware_size - 4 > gNb[0].bytes) {cpRow("nosqe"); return false;}
    const uint32_t sqeBytes = native_gpu_sqe_firmware_size - 4;
    n[1] = sqeBytes;
    for (uint32_t i = 0; i < sqeBytes; ++i) gNbCpu[0][i] = native_gpu_sqe_firmware[4 + i];
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint32_t i = 0; i < sqeBytes && !n[2]; ++i) if (gNbCpu[0][i] != native_gpu_sqe_firmware[4 + i]) n[2] = i + 1;
    if (n[2]) {cpRow("sqerb"); return false;}
    n[0] = 3; stage(121);
    hwInit();
    gw64(0x830, gNb[0].iova);                                           // CP_SQE_INSTR_BASE, before CP_SQE_CNTL
    // Ring 0: 32 KiB at its IOVA, the memptrs page for the RPTR shadow and the fence; A6xx forms (CACHE_FLUSH_TS fences).
    n[0] = 4; stage(122);
    gRing = new NativeAdrenoRing;
    gCpInit = new NativeAdrenoCpInit;
    if (!gRing || !gCpInit || gRing->setGeneration(6)) {cpRow("noring"); return false;}
    n[3] = uint32_t(gRing->attach(&gRio, &gCache, const_cast<uint8_t *>(gNbCpu[1]), gNb[1].bytes, gNb[1].iova,
                                  const_cast<uint8_t *>(gNbCpu[2]), gNb[2].bytes, gNb[2].iova));
    if (n[3]) {cpRow("attach"); return false;}
    gw(0x808, 1);                                                       // CP_SQE_CNTL: start the microcode
    // a6xx_cp_init: CP_ME_INIT with its eight words.
    n[0] = 5; stage(123);
    NativeAdrenoCpInitConfig config{gNb[0].iova, 0, 1, 6};
    uint32_t fence = 0;
    n[4] = uint32_t(gCpInit->run(&gRio, gRing, &config, &fence));
    if (n[4]) {cpRow("init"); return false;}
    n[5] = uint32_t(gRing->wait(fence, &gClock, 200000, &n[6]));
    if (n[5]) {cpRow("initf"); return false;}
    // The first command through NativeAdreno's mapping: CP_MEM_WRITE (address lo/hi, data) into the scratch page.
    n[0] = 6; stage(124);
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gNbCpu[4]);
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t words[4] = {};
    if (AdrenoPM4Type7(0x3d, 3, &words[0]) != AdrenoPM4Success) {cpRow("pm4"); return false;}
    words[1] = uint32_t(gNb[4].iova); words[2] = uint32_t(gNb[4].iova >> 32); words[3] = 0xA6500C0D;
    if (gRing->submit(words, 4, &fence)) {cpRow("submit"); return false;}
    uint32_t seen = 0;
    n[7] = uint32_t(gRing->wait(fence, &gClock, 200000, &seen));
    n[8] = scratch[0];
    if (n[7] || n[8] != 0xA6500C0D) {cpRow("mem"); return false;}
    n[0] = 7; stage(125);
    cpRow("ok");
    return true;
}

// Mode 10. SIP SMC as qcom_scm-smc.c: x0 function, x1 arginfo, x2.. arguments; INTERRUPTED (1) resumes with x0 = 1 and the
// returned x6, V2_EBUSY (-12) retries after 30 ms up to 20 times. This core's IRQs stay masked across the call and its resumes
// (alioth/tb128fu: an unmasked init_image was interrupted 10000 times in a row).
struct ScmRes {int64_t a0; uint64_t a1;};
uint32_t gScmResumes;
ScmRes scmCall(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2) {
    const boolean_t was = ml_set_interrupts_enabled(FALSE);
    ScmRes out{-1000, 0};
    uint64_t first = fn, a6 = 0;
    gScmResumes = 0;
    for (unsigned resume = 0, busy = 0; resume < 10000; gScmResumes = resume) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = arginfo, x2 asm("x2") = p0, x3 asm("x3") = p1;
        register uint64_t x4 asm("x4") = p2, x5 asm("x5") = 0, x6 asm("x6") = a6, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {first = 1; a6 = x6; ++resume; continue;}
        if (int64_t(x0) == -12 && busy++ < 20) {first = fn; a6 = 0; IODelay(30000); continue;}
        out = ScmRes{int64_t(x0), x1};
        break;
    }
    ml_set_interrupts_enabled(was);
    return out;
}
// ARM_SMCCC_CALL_VAL(STD, SMC_64, OWNER_SIP, svc << 8 | cmd): PIL is service 2 (qcom_scm.h).
constexpr uint64_t kScmPil(uint32_t cmd) {return 0x42000000u | (0x02u << 8) | cmd;}
constexpr uint32_t kGpuPasId = 13;                                  // cas DT qcom,kgsl-hyp qcom,pas-id = <0x0d>
constexpr uint64_t kZapPhys = 0x8681a000, kZapBytes = 0x2000;       // cas DT pil_gpu_region@8681a000, no-map (dts/cas.dts region10)
uint32_t le32(const uint8_t *p) {return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;}
uint16_t le16(const uint8_t *p) {return uint16_t(p[0] | p[1] << 8);}
void cleanToPoc(const void *p, size_t len) {
    uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const size_t line = 4u << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1); a < reinterpret_cast<uintptr_t>(p) + len; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t msSince(uint64_t start) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    return uint32_t(ns / 1000000);
}
// [0] step, [1]/[2] init_image x0/x1, [3]/[4] mem_setup, [5]/[6] auth_and_reset, [7] resumes, [8..10] ms of the three calls,
// [11] program headers, [12] relocatable, [13] span, [14] the secure-mode fence (kZapSecureUnsent until sent), [15] metadata page.
constexpr int32_t kZapSecureUnsent = -100;
extern "C" {
int32_t native_gpu_zap[16];
}
IOBufferMemoryDescriptor *gZapMeta;
void zapRow() {
    const int32_t *z = native_gpu_zap;
    char se[12];
    if (z[14] == kZapSecureUnsent) snprintf(se, sizeof se, "-");
    else snprintf(se, sizeof se, "%d", z[14]);
    snprintf(native_gpu_short[kRowZap], 48, "ZAP s%d i%d/%x m%d/%x a%d/%x se%s r%d q%d", z[0], z[1], z[2], z[3], z[4], z[5], z[6],
             se, z[12], z[7]);
}
// qcom_mdt_load for the zap shader: the ELF header and program headers plus the hash segment as metadata for init_image,
// mem_setup for a relocatable image, the loadable segments copied into the reserved region, then auth_and_reset. z[0] is the
// step reached (negative: why it stopped before any SMC). Every layout fact comes from cas's own a650_zap.elf program headers.
bool zapLoad() {
    int32_t *z = native_gpu_zap;
    auto stop = [&](int why) {z[0] = why; zapRow(); return false;};
    stage(130);
    z[0] = 1;
    zapRow();
    // The region must not be memory XNU hands out: TrustZone locks it after authentication.
    if (kZapPhys + kZapBytes > vm_first_phys && kZapPhys < vm_last_phys) return stop(-20);
    if (!&native_gpu_zap_firmware || !&native_gpu_zap_firmware_size) return stop(-21);
    const uint8_t *fw = native_gpu_zap_firmware;
    const uint64_t size = native_gpu_zap_firmware_size;
    if (size < 52 || fw[0] != 0x7f || fw[1] != 'E' || fw[2] != 'L' || fw[3] != 'F' || fw[4] != 1) return stop(-1);
    const uint32_t phoff = le32(fw + 28);
    const unsigned phent = le16(fw + 42), phnum = le16(fw + 44);
    z[11] = int32_t(phnum);
    if (phent != 32 || phnum < 2 || phnum > 16 || phoff + uint64_t(phnum) * 32 > size) return stop(-2);
    auto ph = [&](unsigned i, unsigned field) {return le32(fw + phoff + i * 32 + field * 4);};   // type off va pa filesz memsz flags
    if (ph(0, 0) == 1) return stop(-3);
    unsigned hash = 0;
    for (unsigned i = 1; i < phnum && !hash; ++i) if (((ph(i, 6) >> 24) & 7) == 2) hash = i;
    if (!hash) return stop(-4);
    uint64_t lo = ~0ull, hi = 0;
    bool relocate = false;
    for (unsigned i = 0; i < phnum; ++i) {
        if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;   // mdt_phdr_loadable
        if (ph(i, 6) & (1u << 27)) relocate = true;
        lo = lo < ph(i, 3) ? lo : ph(i, 3);
        const uint64_t end = (uint64_t(ph(i, 3)) + ph(i, 5) + 0xfff) & ~0xfffull;
        hi = hi > end ? hi : end;
    }
    z[12] = relocate; z[13] = int32_t(hi - lo);
    if (lo >= hi || hi - lo > kZapBytes) return stop(-5);
    const uint64_t ehdr = ph(0, 4), hashBytes = ph(hash, 4), hashOff = ph(hash, 1);
    if (ehdr > size || hashOff + hashBytes > size || ehdr + hashBytes > 0x4000) return stop(-6);
    // Metadata: physically contiguous, written back to the point of coherency (Linux: dma_alloc_coherent), any address below
    // 2^40: the SMC64 call carries it in full, and TrustZone answers init_image with an error if it refuses it.
    z[0] = 2;
    const size_t metaBytes = size_t((ehdr + hashBytes + 0xfff) & ~0xfffull);
    gZapMeta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        metaBytes, 0xFFFFFFF000ULL);
    if (!gZapMeta || gZapMeta->prepare() != kIOReturnSuccess) return stop(-7);
    uint8_t *meta = static_cast<uint8_t *>(gZapMeta->getBytesNoCopy());
    const uint64_t metaPhys = gZapMeta->getPhysicalSegment(0, nullptr);
    z[15] = int32_t(metaPhys >> 12);
    if (!meta || !metaPhys) return stop(-8);
    for (uint64_t i = 0; i < ehdr; ++i) meta[i] = fw[i];
    for (uint64_t i = 0; i < hashBytes; ++i) meta[ehdr + i] = fw[hashOff + i];
    cleanToPoc(meta, metaBytes);
    // qcom_scm_pas_init_image: ARGS(2, VAL, RW).
    z[0] = 3; stage(131);
    uint64_t start = mach_absolute_time();
    ScmRes r = scmCall(kScmPil(0x01), 2 | (2u << 6), kGpuPasId, metaPhys, 0);
    z[1] = int32_t(r.a0); z[2] = int32_t(r.a1); z[8] = int32_t(msSince(start)); z[7] = int32_t(gScmResumes);
    if (r.a0 || r.a1) return stop(3);
    if (relocate) {
        // qcom_scm_pas_mem_setup: ARGS(3).
        z[0] = 4; stage(132);
        start = mach_absolute_time();
        r = scmCall(kScmPil(0x02), 3, kGpuPasId, kZapPhys, hi - lo);
        z[3] = int32_t(r.a0); z[4] = int32_t(r.a1); z[9] = int32_t(msSince(start));
        if (r.a0 || r.a1) return stop(4);
    }
    // qcom_mdt_load_no_init into the reserved region (relocatable: offsets from the lowest segment address).
    z[0] = 5; stage(133);
    {
        IOMemoryMap *map = nullptr;
        volatile uint8_t *region = reinterpret_cast<volatile uint8_t *>(mapRange(kZapPhys, kZapBytes, &map, kIOMapWriteCombineCache));
        if (!region) return stop(-9);
        const uint64_t base = relocate ? lo : kZapPhys;
        for (unsigned i = 0; i < phnum; ++i) {
            if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;
            const uint64_t at = uint64_t(ph(i, 3)) - base, file = ph(i, 4), mem = ph(i, 5), off = ph(i, 1);
            if (ph(i, 3) < base || at + mem > kZapBytes || file > mem || off + file > size) {map->release(); return stop(-10);}
            for (uint64_t b = 0; b < file; ++b) region[at + b] = fw[off + b];
            for (uint64_t b = file; b < mem; ++b) region[at + b] = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        map->release();   // the CPU view is gone before TrustZone takes the region
    }
    // qcom_scm_pas_auth_and_reset: ARGS(1).
    z[0] = 6; stage(134);
    start = mach_absolute_time();
    r = scmCall(kScmPil(0x05), 1, kGpuPasId, 0, 0);
    z[5] = int32_t(r.a0); z[6] = int32_t(r.a1); z[10] = int32_t(msSince(start));
    if (r.a0 || r.a1) return stop(6);
    z[0] = 7;   // TrustZone authenticated the zap shader
    zapRow();
    return true;
}
// The zap shader, then CP_SET_SECURE_MODE(0) through the ring (hw_init after a6xx_zap_shader_init).
bool leaveSecure() {
    native_gpu_zap[14] = kZapSecureUnsent;
    if (!zapLoad()) return false;
    stage(135);
    uint32_t words[2] = {}, fence = 0, seen = 0;
    if (AdrenoPM4Type7(0x66, 1, &words[0]) != AdrenoPM4Success || gRing->submit(words, 2, &fence)) {
        native_gpu_zap[14] = -1;
        zapRow();
        return false;
    }
    native_gpu_zap[14] = int32_t(gRing->wait(fence, &gClock, 200000, &seen));
    zapRow();
    return native_gpu_zap[14] == 0;
}

// The GPU interrupt (cas DT kgsl_3d0_irq: interrupts = <0 0x12c 4>, SPI 300, level high = INTID 332). As a6xx_irq: read
// RBBM_INT_0_STATUS and clear it through RBBM_INT_CLEAR_CMD before anything else, so the level line drops before the EOI; the same
// status 100000 times in a row masks the GPU interrupts. Each interrupt wakes the fence waiters (naIdle).
void gpuIrq(void *) {
    const uint32_t status = gpuRead(0x201);
    if (status) gw(0x37, status);
    gIrqRepeat = (status == gIrqLast && !(status & (1u << 20))) ? gIrqRepeat + 1 : 0;
    gIrqLast = status;
    gIrqSeen = gIrqSeen | status;
    gIrqCount = gIrqCount + 1;
    if (gIrqRepeat > 100000) {gw(0x38, 0); gIrqMasked = 1;}
    thread_wakeup(event_t(&gIrqCount));
}
// The secure-mode fence left CACHE_FLUSH_TS set, so a working line fires as soon as it is enabled; a CP_NOP fence must fire it
// again (here: the count must grow across the NOP, which alioth only printed).
bool gpuInterrupt() {
    stage(140);
    uint32_t readback = 0;
    uint64_t router = ~0ull;
    const IOReturn attached = &armcpu_spi_attach ? armcpu_spi_attach(32 + 300, false, gpuIrq, nullptr, &readback, &router)
                                                 : kIOReturnUnsupported;
    gIrqAttached = attached == kIOReturnSuccess;
    IOSleep(20);
    const uint32_t first = gIrqCount;
    uint32_t nop = 0, fence = 0, seen = 0;
    const bool fenced = AdrenoPM4Type7(0x10, 0, &nop) == AdrenoPM4Success && !gRing->submit(&nop, 1, &fence) &&
                        !gRing->wait(fence, &gClock, 200000, &seen);
    IOSleep(20);
    const uint32_t after = gIrqCount;
    const bool ok = gIrqAttached && fenced && after > first;
    snprintf(native_gpu_short[kRowIrq], 48, "IRQ %s a%x g%x c%u>%u f%u s%x n%x m%u", ok ? "ok" : "bad", unsigned(attached), readback,
             first, after, fenced, gIrqSeen, gpuRead(0x201), gIrqMasked);
    cpStateRow();
    if (ok) stage(141);
    return ok;
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
    // alioth's gpuThread: mode 9 only with GX on, mode 10 only after the marker came back.
    const bool cp = gMode >= 9 && gColdOk && native_gpu_gx.gx_on && gpuContext() && cpStart();
    if (gMode >= 10 && cp && leaveSecure()) gpuInterrupt();
    stage(99);
}
}  // namespace

extern "C" void cas_gpu_probe(void) {
    unsigned mode = 0;
    // Modes 11 and above (alioth's user clients and Metal) are not ported; mode 4 does not exist.
    if (!PE_parse_boot_argn("cas-gpu", &mode, sizeof mode) || !mode || mode == 4 || mode > 10 || native_gpu_step) return;
    gMode = mode;
    native_gpu_step = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(gpuThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
