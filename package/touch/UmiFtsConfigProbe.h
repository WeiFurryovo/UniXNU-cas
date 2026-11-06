// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "UmiFtsSysInfo.h"
// Stock readConfig: A8 + 16-bit offset, no dummy byte on I2C.
// Qualified range 0..0xcff was captured twice in working Recovery.
// Fingerprints locate differences; matching CRCs are not a bytewise proof.
struct UmiFtsConfigProbe {
    static constexpr unsigned chunk=64,count=52,bytes=chunk*count;
    uint32_t crc[count];unsigned completed,result;bool active;
    bool begin(const UmiFtsSysInfo &s) {
        memset(this,0,sizeof(*this));result=0x100;
        if(!s.valid || s.chip0!=0x3639 || s.fw!=0x128 || s.svn!=0x4053 || s.config!=0x4b)return false;
        result=0;active=true;return true;
    }
    static uint32_t checksum(const uint8_t *p,unsigned n) {
        uint32_t c=~0U;
        for(unsigned i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320U & (0U-(c&1)));}
        return ~c;
    }
    template<class Read> bool step(Read read) {
        if(!active)return true;
        unsigned offset=completed*chunk;
        const uint8_t command[]={0xa8,uint8_t(offset>>8),uint8_t(offset)};
        uint8_t b[chunk]={};result=read(command,sizeof(command),b,sizeof(b));
        if(result){active=false;return true;}
        crc[completed++]=checksum(b,sizeof(b));active=completed<count;
        return !active;
    }
};
