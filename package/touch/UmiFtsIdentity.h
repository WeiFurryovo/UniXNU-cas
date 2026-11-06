// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
// Native fts_get_lockdown_info: read factory record0x70, never program NVM.
// The fixed8-byte identity selects umi's st_fts_j2_2.ftb profile. The caller
// has already qualified chip/FW/AFE and performs GPIO resets2/3 afterwards.
struct UmiFtsIdentity {
    unsigned result,flags,readResult,writeResult,polls;
    uint8_t before[8],after[8],identity[8],event[8];
};
template<class Transfer,class Sleep,class Now>
static UmiFtsIdentity umi_fts_read_identity(Transfer transfer,Sleep sleep,Now now) {
    UmiFtsIdentity r={};r.readResult=r.writeResult=0x100;
    const uint8_t address[]={0xa6,0,0},request[]={0xa4,0x06,0x70},fifo=0x86;
    uint8_t header[28]={};
    r.readResult=transfer(address,sizeof(address),header,4);
    if(r.readResult){r.result=1;return r;}
    for(unsigned n=0;n<4;++n)r.before[n]=header[n];r.flags|=1;
    if(header[0]!=0xa5){r.result=2;return r;}
    unsigned before=unsigned(header[2])|(unsigned(header[3])<<8);
    r.writeResult=transfer(request,sizeof(request),nullptr,0);
    if(r.writeResult){r.result=3;return r;}r.flags|=2;
    sleep(10);uint64_t started=now();
    for(unsigned n=0;n<50 && now()-started<100000000ULL;++n){
        uint8_t event[8]={};++r.polls;
        r.readResult=transfer(&fifo,1,event,8);
        if(r.readResult){r.result=4;return r;}
        for(unsigned j=0;j<8;++j)r.event[j]=event[j];
        if(event[0]==0xf3 || event[0]==3){r.result=5;return r;}
        if(event[0]==0x43 && event[1]==1 && event[2]==0xa4 && event[3]==6 && event[4]==0x70){r.flags|=4;break;}
        sleep(2);
    }
    if(!(r.flags&4)){r.result=6;return r;}
    r.readResult=transfer(address,sizeof(address),header,sizeof(header));
    if(r.readResult){r.result=7;return r;}
    for(unsigned n=0;n<8;++n){r.after[n]=header[n];r.identity[n]=header[n+20];}r.flags|=8;
    unsigned after=unsigned(header[2])|(unsigned(header[3])<<8);
    if(header[0]!=0xa5 || header[1]!=0x70 || header[4]!=0 || after!=((before+1)&65535U)){r.result=8;return r;}r.flags|=16;
    const uint8_t expected[]={0x53,0x42,0x32,1,0x74,2,0x32,1};
    for(unsigned n=0;n<8;++n)if(r.identity[n]!=expected[n]){r.result=9;return r;}
    r.flags|=32;return r;
}
