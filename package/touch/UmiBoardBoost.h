// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native umi always-on vdd_boost_vreg. GPIO latch evidence is not voltage evidence.
#pragma once
#include <stdint.h>
struct UmiBoardBoost {
    unsigned result=0,readResult=0,writeResult=~0U,writes=0;
    unsigned before=0x100,after=0x100,flags=0,delayUs=0;
};
template<class Read,class WriteHigh,class Delay>
static UmiBoardBoost umi_board_boost_enable(Read read,WriteHigh writeHigh,Delay delay) {
    UmiBoardBoost r;
    const uint16_t addr[]={0x0104,0x0105,0xc404,0xc405,0xc440,0xc441,
                           0xc442,0xc443,0xc444,0xc445,0xc446};
    // Qualified on this umi in attempt79. Preserve VIN, pull, drive, mux and mode.
    const uint8_t expected[]={0x51,0x20,0x10,0x11,1,0,4,0,0,1,0x80};
    uint8_t value[11]={};
    for(unsigned i=0;i<11;++i){
        uint8_t v=0;r.readResult=read(2,addr[i],&v);
        if(r.readResult){r.result=1;return r;}value[i]=v;
    }
    r.before=value[8];
    for(unsigned i=0;i<11;++i){
        if(i==8 ? (value[i]!=0 && value[i]!=0x80) : value[i]!=expected[i]){
            r.result=i<4?2:3;return r;
        }
    }
    r.flags|=1;
    if(!value[8]){
        r.writes=1;r.writeResult=writeHigh(); // Only SID2/C444=80, never disable.
        if(r.writeResult){r.result=4;return r;}
    }else {r.flags|=8;r.writeResult=0;}
    delay(4000);r.delayUs=4000;r.flags|=4;
    // No retry or rollback to low: the native board keeps this rail always on.
    for(unsigned i=0;i<11;++i){
        uint8_t v=0;r.readResult=read(2,addr[i],&v);
        if(r.readResult){r.result=5;return r;}
        if(i==8)r.after=v;
        if(v!=(i==8?0x80:expected[i])){r.result=6;return r;}
    }
    r.flags|=2;
    return r;
}
