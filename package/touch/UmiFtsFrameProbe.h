// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "UmiFtsFrameSample.h"
struct UmiFtsFrameProbe {
    unsigned mode,flags,startResult,offResult,frameResult,restoreOnResult,restoreGestureResult;
    bool restored() const { return (flags&48)==48 && !restoreOnResult && !restoreGestureResult; }
};
// Original fresh-frame lifecycle: active or locked-active, wait200ms, sense-off,
// wait50ms, read. Restore the ordinary screen-on/FOD mode on every safe exit.
// No FIFO flush: keep returned command/status events for the normal worker.
template<class Write,class Read,class Sleep,class Ready>
static UmiFtsFrameProbe umi_fts_probe_frame(const UmiFtsSysInfo &info,UmiFtsFrameSample &sample,
                                          unsigned mode,Write write,Read read,Sleep sleep,Ready ready) {
    UmiFtsFrameProbe r={mode,0,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    if(!sample.begin(info) || (mode!=0 && mode!=3)) {sample.active=false;sample.result=3;r.frameResult=3;return r;}
    r.flags=128;
    if(!ready()){sample.active=false;sample.result=UINT32_MAX;return r;}
    r.flags|=1;
    const uint8_t start[]={0xa0,(uint8_t)mode,(uint8_t)(mode==0?1:0)};
    const uint8_t off[]={0xa0,0,0},on[]={0xa0,0,1},gesture[]={0xa2,3,0,0,0,3};
    r.startResult=write(start,sizeof(start));
    if(!r.startResult){
        r.flags|=2;sleep(200);r.offResult=write(off,sizeof(off));
        if(!r.offResult){
            r.flags|=4;sleep(50);while(!sample.step(read)){}
            r.frameResult=sample.result;if(!r.frameResult && sample.validMask==7)r.flags|=8;
        }
    }
    sample.active=false;sample.result=r.frameResult;
    if(ready()){
        r.flags|=64;r.restoreOnResult=write(on,sizeof(on));
        if(!r.restoreOnResult){
            r.flags|=16;
            if(ready()){
                r.restoreGestureResult=write(gesture,sizeof(gesture));
                if(!r.restoreGestureResult)r.flags|=32;
            }
        }
    }
    return r;
}
