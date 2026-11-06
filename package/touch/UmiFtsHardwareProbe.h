// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
// FTM5 ftsHardware.h: chip ID, IER, ICR, GPIO input/direction, boot option,
// CRC. Address phases only; no register, flash or calibration writes.
static constexpr uint8_t umi_fts_hw_offsets[]={0x00,0x01,0x29,0x2d,0x30,0x32,0x25,0x78};
struct UmiFtsHardwareProbe { unsigned result,validMask; uint8_t values[8]; };
template<class Read>
static UmiFtsHardwareProbe umi_fts_hardware_probe(Read read) {
    UmiFtsHardwareProbe r={};
    for(unsigned n=0;n<8;++n){
        const uint8_t command[]={0xfa,0x20,0,0,umi_fts_hw_offsets[n]};
        uint8_t value=0;r.result=read(command,sizeof(command),&value,1);
        if(r.result)break;
        r.values[n]=value;r.validMask|=1U<<n;
    }
    return r;
}
