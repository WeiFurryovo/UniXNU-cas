// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
// fts_touchgame_show / fts_read_touchmode_data: addressed, seven-byte reads.
// Failed reads never publish their partial DMA buffer as valid mode values.
struct UmiFtsModes {
    unsigned modeResult,gripResult,validMask;
    uint8_t mode[8],grip[8];
};
template<class Read>
static UmiFtsModes umi_fts_read_modes(Read read) {
    UmiFtsModes out={};out.modeResult=out.gripResult=0x100;
    const uint8_t command[]={0xc1,0x05},grip[]={0xc1,0x08};
    uint8_t data[7]={};
    out.modeResult=read(command,sizeof(command),data,sizeof(data));
    if(out.modeResult)return out;
    for(unsigned n=0;n<7;++n)out.mode[n]=data[n];out.validMask=1;
    out.gripResult=read(grip,sizeof(grip),data,sizeof(data));
    if(out.gripResult)return out;
    for(unsigned n=0;n<7;++n)out.grip[n]=data[n];out.validMask=3;
    return out;
}
template<class Write>
static unsigned umi_fts_default_charge_mode(Write write) {
    // fts_write_charge_status(NOT_CHARGING), observed on this Recovery.
    // Volatile noise-rejection mode only; this does not control phone charging.
    const uint8_t command[]={0xa2,0x02,0x00};
    return write(command,sizeof(command));
}
// umi fts_mode_handler followed by fts_init_sensing's CONFIG_TOUCHSCREEN_FOD
// branch. WAIT_AFTER_SENSEOFF is 50 ms. All commands change volatile scan state.
template<class Write, class Sleep>
static unsigned umi_fts_start_sensing(bool initial, Write write, Sleep sleep,
                                      unsigned *completed) {
    const uint8_t on[]={0xa0,0x00,0x01};
    const uint8_t reports[]={0xa2,0x03,0,0,0,0x03};
    const uint8_t off[]={0xa0,0x00,0x00};
    *completed=0;
    if(write(on,sizeof(on)))return 9;
    *completed=1;
    if(write(reports,sizeof(reports)))return 10;
    *completed=2;
    if(!initial)return 0;
    if(write(off,sizeof(off)))return 13;
    *completed=3;
    sleep(50);
    if(write(on,sizeof(on)))return 14;
    *completed=4;
    return 0;
}
