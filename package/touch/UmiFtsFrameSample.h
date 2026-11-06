// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "UmiFtsSysInfo.h"
// Eight signed nodes near the centre of each source-defined raw plane.
// This helper alone neither freezes sensing nor makes the planes atomic.
// The caller records whether it used a live or source-defined frozen read.
struct UmiFtsFrameSample {
    uint16_t addresses[3];uint8_t data[3][16];
    unsigned validMask,index,result;bool active;
    bool begin(const UmiFtsSysInfo &s) {
        memset(this,0,sizeof(*this));result=3;
        if(!s.valid || s.tx<8 || s.rx<8 || s.tx>64 || s.rx>64)return false;
        const unsigned count[]={unsigned(s.tx)*s.rx,s.tx,s.rx};
        const unsigned base[]={s.msRaw,s.ssTxRaw,s.ssRxRaw};
        for(unsigned i=0;i<3;++i){
            if(!base[i] || (base[i]&1) || base[i]+count[i]*2>65536)return false;
            addresses[i]=(uint16_t)(base[i]+(count[i]/2-4)*2);
        }
        result=0;active=true;return true;
    }
    template<class Read> bool step(Read read) {
        if(!active)return true;
        uint8_t command[]={0xa6,(uint8_t)(addresses[index]>>8),(uint8_t)addresses[index]},bytes[16]={};
        result=read(command,sizeof(command),bytes,sizeof(bytes));
        if(result){active=false;return true;}
        memcpy(data[index],bytes,sizeof(bytes));validMask|=1U<<index;++index;
        active=index<3;return !active;
    }
};
