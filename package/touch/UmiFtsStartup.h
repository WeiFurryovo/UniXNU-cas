// SPDX-License-Identifier: BSD-2-Clause-Patent
// Same 100 ms reset/READY window as umi's fts_521 GENERAL_TIMEOUT.
#pragma once
#include "UmiFtsCore.h"
#include "UmiFtsSysInfo.h"
// Native fts_init -> fts_crc_check -> post-version-check reset. All three
// GPIO resets must produce READY before sensing. Reject unsupported identity,
// CRC or AFE state instead of attempting native firmware/calibration writes.
static inline unsigned umi_fts_native_identity_check(const UmiFtsControllerRead &r,
                                             const UmiFtsSysInfo &s) {
    if(r.sysResult || r.bytesRead!=UMI_FTS_SYSINFO_SIZE || r.chunkMask!=0x1fff)return 1;
    if(!s.valid)return 2;
    if(r.crcResult)return 3;
    if(r.crc&3)return 4;
    if(s.chip0!=0x3639 || s.fw!=0x128 || s.svn!=0x4053 || s.config!=0x4b)return 5;
    if(s.cfgAfe!=s.cxAfe || s.cfgAfe!=s.panelAfe)return 6;
    return 0;
}
enum { UMI_FTS_WAIT_READY=5, UMI_FTS_READY_TIMEOUT=6 };
// ftsCore.c fts_system_reset fallback / ftsHardware.h FTM5 definition.
// This resets volatile IC state; it does not program flash or calibration.
template<class Write,class StartWait>
static unsigned umi_fts_software_reset(Write write,StartWait startWait) {
    const uint8_t command[]={0xfa,0x20,0x00,0x00,0x24,0x80};
    unsigned result=write(command,sizeof(command));
    if(!result)startWait(); // A successful write still requires a fresh READY.
    return result;
}
static inline bool umi_fts_ready_expired(uint64_t elapsedNs) {
    return elapsedNs>=100000000ULL;
}
static inline unsigned umi_fts_startup_event(UmiTouchFrame *frame,const uint8_t event[8],
                                             bool awaitingReady,uint64_t elapsedNs) {
    if(awaitingReady) {
        if(umi_fts_ready_expired(elapsedNs))return UMI_FTS_READY_TIMEOUT;
        // Until READY, never interpret reset-time bytes as contacts or as a
        // normal-mode protocol violation. Real controller errors still fail.
        if(event[0]!=0x03 && event[0]!=0xf3)return UMI_FTS_WAIT_READY;
    }
    return umi_fts_parse(frame,event);
}
