// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <string.h>
struct UmiFtsSyncFrame {
    unsigned result,flags,beforeCount,afterCount,polls,readResult,writeResult;
    uint8_t before[8],after[8];
};
// ftsCore.c requestSyncFrame: request volatile host data, then wait for a new
// header count. This does not turn sensing off or write firmware/calibration.
// A new count proves a load request completed, not that analog sensing works.
template<class Write,class Read,class Sleep,class Ready>
static UmiFtsSyncFrame umi_fts_sync_frame(Write write,Read read,Sleep sleep,Ready ready) {
    UmiFtsSyncFrame r={};r.result=4;r.readResult=r.writeResult=UINT32_MAX;
    const uint8_t address[]={0xa6,0,0},request[]={0xa4,0x06,0x30};
    uint8_t bytes[8]={};
    if(!ready())return r;
    r.readResult=read(address,sizeof(address),bytes,sizeof(bytes));
    if(r.readResult){r.result=1;return r;}
    memcpy(r.before,bytes,8);
    if(bytes[0]!=0xa5){r.result=2;return r;}
    r.flags=1;r.beforeCount=bytes[2]|unsigned(bytes[3])<<8;r.afterCount=r.beforeCount;
    if(!ready())return r;
    r.writeResult=write(request,sizeof(request));
    if(r.writeResult){r.result=1;return r;}
    r.flags|=2;
    for(unsigned i=0;i<20;++i){
        sleep(10);
        if(!ready()){r.result=4;return r;}
        r.readResult=read(address,sizeof(address),bytes,sizeof(bytes));++r.polls;
        if(r.readResult){r.result=1;return r;}
        memcpy(r.after,bytes,8);r.afterCount=bytes[2]|unsigned(bytes[3])<<8;
        if(bytes[0]==0xa5 && bytes[1]==0x30 && r.afterCount!=r.beforeCount){
            r.flags|=4;r.result=0;return r;
        }
    }
    r.result=3;return r;
}
