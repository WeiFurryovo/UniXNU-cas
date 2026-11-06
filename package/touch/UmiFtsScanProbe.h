// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "UmiFtsSysInfo.h"
// Read-only FTM5 observations, restricted to the firmware analyzed in
// scan-reference.json. These are asynchronous samples, not a coherent frame.
// No A4 LOAD_DATA request: it can wait on the same scan being investigated.
static constexpr uint32_t umi_fts_scan_addresses[]={
    0x20000050,0x20000060,0x20030000,0x200126ca,0x200156a8};
static constexpr unsigned umi_fts_scan_lengths[]={2,6,8,8,16};
struct UmiFtsScanProbe {
    unsigned result,validMask;
    uint8_t data[5][16];
};
template<class Read>
static UmiFtsScanProbe umi_fts_scan_probe(const UmiFtsSysInfo &info,Read read) {
    UmiFtsScanProbe out={};
    if(!info.valid || info.chip0!=0x3639 || info.fw!=0x0128 ||
       info.svn!=0x4053 || info.config!=0x004b){out.result=0x100;return out;}
    for(unsigned n=0;n<5;++n){
        uint32_t a=umi_fts_scan_addresses[n];
        const uint8_t command[]={0xfa,uint8_t(a>>24),uint8_t(a>>16),uint8_t(a>>8),uint8_t(a)};
        uint8_t bytes[16]={};
        out.result=read(command,sizeof(command),bytes,umi_fts_scan_lengths[n]);
        if(out.result)break;
        memcpy(out.data[n],bytes,umi_fts_scan_lengths[n]);out.validMask|=1U<<n;
    }
    return out;
}
