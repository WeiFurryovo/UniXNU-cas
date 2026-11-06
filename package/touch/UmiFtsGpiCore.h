// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8250 GPI wire layout checked against pinned msm_gpi.h/msm_gpi_mmio.h.
#pragma once
// clk-branch.c skips HALT polling for a branch with its explicit HWCG bit on.
// Only WRAP_1_S_AHB declares HWCG (CBCR bit1) in gcc-kona.c.
static inline bool umiGpiBranchReady(uint32_t value,bool hasHwcg) {
    return (hasHwcg && (value&2)) || !(value&0x80000000U) ||
           (value&0xf0000000U)==0x20000000U;
}
// clk-rcg2.c calc_rate: HID 0 bypasses the divider; HID1 gives 2/(1+1).
// Both yield the 19.2 MHz XO parent; reject other parents and fractional modes.
static inline bool umiGpiXoUndivided(uint32_t cfg) {
    return !(cfg&0x3700U) && (cfg&31)<=1;
}
struct UmiGpiTre { uint32_t d[4]; };
static inline UmiGpiTre umiGpiConfig(unsigned level) {
    // Original 400 kHz timing: 19.2 MHz / 2 / 24; TX+RX byte packing.
    return {{0x0318070a,0,(level<<16)|2,0x00220001}};
}
static inline UmiGpiTre umiGpiGo(bool read,bool stretch,unsigned bytes) {
    return {{(stretch?0x04000000U:0U)|0x4900U|(read?2U:1U),0,
             read?bytes:0U,read?0x00200900U:0x00200001U}};
}
static inline UmiGpiTre umiGpiBuffer(uint64_t address,unsigned bytes) {
    return {{uint32_t(address),uint32_t(address>>32),bytes,0x00100200}};
}
static inline bool umiGpiRingIndex(uint64_t pointer,uint64_t base,unsigned count,unsigned &index) {
    if(pointer<base || pointer-base>=uint64_t(count)*16 || ((pointer-base)&15))return false;
    index=unsigned((pointer-base)/16);return true;
}
// Full TRE address and length prevent a stale/wrapped completion accepting data.
static inline unsigned umiGpiCompletion(const UmiGpiTre &ev,unsigned ch,uint64_t expected,
                                       unsigned bytes,bool readGo) {
    unsigned type=(ev.d[3]>>16)&255,channel=ev.d[3]>>24,code=ev.d[2]>>24;
    if(type!=0x22 || channel!=ch || (ev.d[3]&0x340) ||
       (uint64_t(ev.d[1])<<32|ev.d[0])!=expected)return 1;
    if(code!=(readGo?4U:2U) || (!readGo && (ev.d[2]&0xffffff)!=bytes))return 2;
    return 0;
}
// Separate idle context: never modify the live UFS bank. Stream 0x56 is GPI1,
// 0x43 is QUP1; caller must quiesce all HLOS GPI1 channels before takeover.
struct UmiTouchSmmuPlan {
    unsigned error=0,bank=~0U,pages=0,count=0,banks=0,slot[2]={~0U,~0U};
    uint32_t id[3]={},oldSctlr=0;
    uint64_t usedMask[2]={},enabledMask[2]={},nonzeroMask[2]={};
    uint32_t oldSmr[2]={},oldS2cr[2]={};
};
static constexpr uint16_t umiTouchStreams[2]={0x56,0x43};
template<class Read> static UmiTouchSmmuPlan umiTouchSmmuPlan(uint64_t dma,unsigned bytes,Read rd) {
    UmiTouchSmmuPlan p;uint32_t id0=rd(0x20),id1=rd(0x24),id2=rd(0x28);
    p.count=id0&255;p.pages=1U<<(((id1>>28)&7)+1);unsigned banks=id1&255;
    p.id[0]=id0;p.id[1]=id1;p.id[2]=id2;p.banks=banks;
    if(!(id0&(1U<<30)) || !(id0&(1U<<27)) || !(id2&(1U<<12)) ||
       !p.count || p.count>128 || (id1>>31) || p.pages>128 || banks<3 || banks>128 ||
       (p.pages+banks)*4096>0xd0000){p.error=1;return p;}
    const unsigned bits[]={32,36,40,42,44,48};
    for(unsigned shift=0;shift<=8;shift+=4){unsigned code=(id2>>shift)&15;
        if(code>=6 || !bytes || dma>=(1ULL<<bits[code]) || bytes>(1ULL<<bits[code])-dma){p.error=2;return p;}}
    // Last bank belongs to UFS. A currently unreferenced, disabled bank is required.
    bool used[128]={};unsigned free[128],nf=0;
    for(unsigned i=0;i<p.count;++i){uint32_t smr=rd(0x800+4*i),s2cr=rd(0xc00+4*i);
        if(!(smr&0x80000000U)){free[nf++]=i;continue;}
        if(((s2cr>>16)&3)==0 && (s2cr&255)<128)used[s2cr&255]=true;
        unsigned mask=(smr>>16)&0x7fff;
        for(unsigned s=0;s<2;++s)if(((smr^umiTouchStreams[s])&0x7fff&~mask)==0){
            if(mask || p.slot[s]!=~0U){p.error=3;return p;}p.slot[s]=i;}
    }
    // SCTLR.M alone enables translation. Firmware leaves disabled banks at
    // e.g. 0xe0; requiring the whole register to be zero rejects those banks.
    // NativeUfs and ArmSmmu4K also normalize an owned, M=0 bank before use.
    for(unsigned b=0;b<banks;++b){uint32_t ctl=rd((p.pages+b)*4096);uint64_t bit=1ULL<<(b%64);
        if(used[b])p.usedMask[b/64]|=bit;
        if(ctl&1)p.enabledMask[b/64]|=bit;
        if(ctl)p.nonzeroMask[b/64]|=bit;
        if(b>0 && b<banks-1 && !used[b] && !(ctl&1)){p.bank=b;p.oldSctlr=ctl;}}
    if(p.bank==~0U){p.error=4;return p;}
    for(unsigned s=0;s<2;++s){if(p.slot[s]==~0U){if(!nf){p.error=5;return p;}p.slot[s]=free[--nf];}
        p.oldSmr[s]=rd(0x800+4*p.slot[s]);p.oldS2cr[s]=rd(0xc00+4*p.slot[s]);}
    return p;
}
template<class Read,class Write> static unsigned umiTouchSmmuApply(const UmiTouchSmmuPlan &p,Read rd,Write wr) {
    if(p.error)return p.error;
    // Validate the entire plan again before the first mutation.
    if(p.bank==0 || p.bank>=p.banks-1)return 6;
    unsigned cb=(p.pages+p.bank)*4096;
    if((p.oldSctlr&1) || rd(cb)!=p.oldSctlr)return 6;
    for(unsigned i=0;i<p.count;++i)if((rd(0x800+4*i)&0x80000000U) &&
        ((rd(0xc00+4*i)>>16)&3)==0 && (rd(0xc00+4*i)&255)==p.bank)return 6;
    for(unsigned s=0;s<2;++s)if(rd(0x800+4*p.slot[s])!=p.oldSmr[s] || rd(0xc00+4*p.slot[s])!=p.oldS2cr[s])return 6;
    // Only this unreferenced disabled bank is cleared. Live banks (including
    // UFS at banks-1) are neither disabled nor reprogrammed.
    wr(cb,0);if(rd(cb)!=0)return 7;
    wr(0x1800+4*p.bank,1);wr(0x1000+4*p.bank,0x10000);
    if(rd(cb)!=0 || rd(0x1800+4*p.bank)!=1 || rd(0x1000+4*p.bank)!=0x10000)return 7;
    for(unsigned s=0;s<2;++s){unsigned slot=p.slot[s];
        wr(0x800+4*slot,0);if(rd(0x800+4*slot)&0x80000000U)return 8;
        wr(0xc00+4*slot,p.bank);if(rd(0xc00+4*slot)!=p.bank)return 9;
        wr(0x800+4*slot,0x80000000U|umiTouchStreams[s]);
        if(rd(0x800+4*slot)!=(0x80000000U|umiTouchStreams[s]))return 10;
    }return 0;
}
