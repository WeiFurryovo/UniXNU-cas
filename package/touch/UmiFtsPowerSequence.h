// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
struct UmiFtsAvddCycle {
    unsigned result,flags,lowLevels,highLevels;
};
struct UmiFtsSupplyState {
    unsigned result,valid,enable,status,mv;
};
struct UmiFtsFullCycle {
    unsigned result,flags;
    UmiFtsSupplyState before,off,on;
};
// Stock fts_chip_powercycle restarts both supplies. Restrict the digital
// withdrawal to a completed enable vote acquired from an observed OFF rail.
// RESET and AVDD are held low first; no voltage, mode or NVM writes occur.
template<class Drive,class Levels,class Vote,class Read,class Sleep>
static UmiFtsFullCycle umi_fts_full_cycle(bool owned,Drive drive,Levels levels,
                                        Vote vote,Read read,Sleep sleep) {
    UmiFtsFullCycle r={};
    if(!owned){r.result=1;return r;}
    r.flags|=1;r.before=read();
    if(r.before.result || r.before.valid!=15 || r.before.enable!=0x80 ||
       !(r.before.status&0x80) || r.before.mv!=1800){r.result=2;return r;}
    drive(38,false);drive(69,false);
    if(levels()!=0){r.result=3;return r;}
    r.flags|=2;
    if(vote(false)){r.result=4;return r;}
    r.flags|=4;
    for(unsigned n=0;n<20;++n){
        r.off=read();
        if(r.off.result || r.off.valid!=15){r.result=5;return r;}
        if(r.off.enable==0 && !(r.off.status&0x80))break;
        sleep(1);
    }
    if(r.off.enable!=0 || (r.off.status&0x80)){r.result=6;return r;}
    r.flags|=8;
    sleep(20); // Bounded discharge window; voltage is not measured here.
    if(levels()!=0){r.result=7;return r;}
    if(vote(true)){r.result=8;return r;}
    r.flags|=16;
    for(unsigned n=0;n<100;++n){
        r.on=read();
        if(r.on.result || r.on.valid!=15){r.result=9;return r;}
        if(r.on.enable==0x80 && (r.on.status&0x80) && r.on.mv==1800)break;
        sleep(1);
    }
    if(r.on.enable!=0x80 || !(r.on.status&0x80) || r.on.mv!=1800){r.result=10;return r;}
    r.flags|=32;
    sleep(15); // fts_enable_reg: DVDD -> 15 ms -> AVDD.
    if(levels()!=0){r.result=11;return r;}
    drive(69,true);sleep(15);
    if(levels()!=2){r.result=12;return r;}
    drive(38,true);
    if(levels()!=3){r.result=13;return r;}
    r.flags|=64;
    return r;
}
// A bounded analog-rail restart with RESET held low. The caller must own
// GPIO38/69, have a validated enabled 1.8 V supply, and restore saved pins on
// failure. This deliberately does not withdraw inherited digital-rail votes.
// Recovery's failed Goodix probe switches both supplies off before FTS probe;
// this AVDD-only diagnostic is not a claim of an identical full power cycle.
template<class Drive,class Levels,class Sleep>
static UmiFtsAvddCycle umi_fts_avdd_cycle(Drive drive,Levels levels,Sleep sleep) {
    UmiFtsAvddCycle r={0,0,UINT32_MAX,UINT32_MAX};
    drive(38,false);
    if(levels()&1){r.result=1;return r;}
    r.flags|=1;
    drive(69,false);r.lowLevels=levels();
    if(r.lowLevels!=0){r.result=2;return r;}
    r.flags|=2;
    sleep(20); // Bounded discharge interval; not a measured rail voltage.
    // Check RESET is still asserted before reapplying the analog supply.
    if(levels()!=0){r.result=2;return r;}
    drive(69,true);r.highLevels=levels();
    if(r.highLevels!=2){r.result=3;return r;}
    r.flags|=4;
    sleep(15); // Vendor power-cycle uses 5 ms + 10 ms before reset release.
    if(levels()!=2){r.result=3;return r;}
    drive(38,true);r.highLevels=levels();
    if(r.highLevels!=3){r.result=4;return r;}
    r.flags|=8;
    return r;
}
