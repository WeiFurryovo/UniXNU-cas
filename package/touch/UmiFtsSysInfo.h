// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <string.h>
enum {UMI_FTS_SYSINFO_SIZE=208,UMI_FTS_SYSINFO_CHUNK=16};
struct UmiFtsSysInfo {
    bool valid;
    uint16_t chip0,chip1,fw,svn,config,cx,resX,resY,msRaw,ssTxRaw,ssRxRaw;
    uint16_t filtered[3],baseline[3];
    uint8_t cfgAfe,cxAfe,panelAfe,protocol,tx,rx;
};
static uint16_t umi_fts_le16(const uint8_t *p){return (uint16_t)(p[0]|(unsigned)p[1]<<8);}
static UmiFtsSysInfo umi_fts_sysinfo(const uint8_t *p,unsigned bytes) {
    UmiFtsSysInfo s={};
    if(bytes!=UMI_FTS_SYSINFO_SIZE || p[0]!=0xa5 || p[1]!=1)return s;
    // These byte offsets are checked against the pinned original readSysInfo
    // parser by check_umi_fts_sysinfo_layout.py (including both 16-byte arrays).
    s.valid=true;s.chip0=umi_fts_le16(p+10);s.chip1=umi_fts_le16(p+14);
    s.fw=umi_fts_le16(p+16);s.svn=umi_fts_le16(p+18);s.config=umi_fts_le16(p+20);s.cx=umi_fts_le16(p+24);
    s.cfgAfe=p[28];s.cxAfe=p[29];s.panelAfe=p[30];s.protocol=p[31];
    s.resX=umi_fts_le16(p+80);s.resY=umi_fts_le16(p+82);s.tx=p[84];s.rx=p[85];
    s.msRaw=umi_fts_le16(p+136);s.ssTxRaw=umi_fts_le16(p+144);s.ssRxRaw=umi_fts_le16(p+152);
    for(unsigned i=0;i<3;++i){s.filtered[i]=umi_fts_le16(p+138+8*i);s.baseline[i]=umi_fts_le16(p+142+8*i);}
    return s;
}
struct UmiFtsControllerRead {
    uint8_t bytes[UMI_FTS_SYSINFO_SIZE];
    unsigned bytesRead,chunkMask,sysResult,crcResult;
    uint8_t crc;
};
// Read-only commands only. Addressed chunks bound GPIO transport latency;
// failed/partial chunks never become valid data. No reload/reset/NVM command.
template<class Read>
static UmiFtsControllerRead umi_fts_read_controller(Read read) {
    UmiFtsControllerRead r={};
    for(unsigned offset=0;offset<UMI_FTS_SYSINFO_SIZE;offset+=UMI_FTS_SYSINFO_CHUNK) {
        uint8_t command[]={0xa6,(uint8_t)(offset>>8),(uint8_t)offset},chunk[UMI_FTS_SYSINFO_CHUNK]={};
        r.sysResult=read(command,sizeof(command),chunk,sizeof(chunk));
        if(r.sysResult)break;
        memcpy(r.bytes+offset,chunk,sizeof(chunk));r.bytesRead+=sizeof(chunk);r.chunkMask|=1U<<(offset/16);
    }
    const uint8_t crcCommand[]={0xfa,0x20,0,0,0x78};uint8_t crc=0;
    r.crcResult=read(crcCommand,sizeof(crcCommand),&crc,1);
    if(!r.crcResult)r.crc=crc;
    return r;
}
