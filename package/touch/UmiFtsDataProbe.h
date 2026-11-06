// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "UmiFtsSysInfo.h"
// Same samples through the documented A6 framebuffer and firmware-qualified
// FA SRAM alias, followed by filtered/baseline samples. Rolling, not atomic.
// Every command has only an address phase, no register or NVM data writes.
static bool umi_fts_data_probe_qualified(const UmiFtsSysInfo &s) {
    return s.valid && s.chip0==0x3639 && s.fw==0x128 && s.svn==0x4053 && s.config==0x4b;
}
struct UmiFtsDataProbe {unsigned result,validMask;uint16_t addresses[3];uint8_t data[4][16];};
template<class Read> static UmiFtsDataProbe umi_fts_data_probe(const UmiFtsSysInfo &s,unsigned plane,Read read) {
    UmiFtsDataProbe out={};out.result=0x100;
    if(!umi_fts_data_probe_qualified(s) || plane>=3 || s.tx<8 || s.rx<8 || s.tx>64 || s.rx>64)return out;
    const unsigned raw[]={s.msRaw,s.ssTxRaw,s.ssRxRaw};
    const unsigned base[]={raw[plane],s.filtered[plane],s.baseline[plane]};
    const unsigned count=plane==0?unsigned(s.tx)*s.rx:(plane==1?s.tx:s.rx);
    for(unsigned i=0;i<3;++i){
        if(!base[i] || (base[i]&1) || base[i]+2*count>65536)return out;
        out.addresses[i]=uint16_t(base[i]+2*(count/2-4));
    }
    out.result=0;
    for(unsigned n=0;n<4;++n){
        unsigned a=out.addresses[n<2?0:n-1];uint8_t value[16]={};
        const uint8_t a6[]={0xa6,uint8_t(a>>8),uint8_t(a)};
        const uint8_t fa[]={0xfa,0x20,1,uint8_t(a>>8),uint8_t(a)};
        out.result=read(n==1?fa:a6,n==1?5:3,value,sizeof(value));
        if(out.result)break;
        memcpy(out.data[n],value,sizeof(value));out.validMask|=1U<<n;
    }
    return out;
}
// Full 164-byte analog-engine configuration copied by FW FUN_00019832.
// Two bounded pages; never replay this snapshot as configuration.
struct UmiFtsEnginePage {unsigned result,length;uint32_t address;uint8_t data[88];};
template<class Read> static UmiFtsEnginePage umi_fts_engine_page(const UmiFtsSysInfo &s,unsigned page,Read read) {
    UmiFtsEnginePage out={};out.result=0x100;
    if(!umi_fts_data_probe_qualified(s) || page>=2)return out;
    out.address=0x20030000+page*88;out.length=page?76:88;
    uint8_t bytes[88]={};uint32_t a=out.address;
    const uint8_t cmd[]={0xfa,uint8_t(a>>24),uint8_t(a>>16),uint8_t(a>>8),uint8_t(a)};
    out.result=read(cmd,sizeof(cmd),bytes,out.length);
    if(!out.result)memcpy(out.data,bytes,out.length);
    return out;
}
