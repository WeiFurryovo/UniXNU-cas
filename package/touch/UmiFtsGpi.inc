// SPDX-License-Identifier: BSD-2-Clause-Patent
// Worker-only SM8250 QUP1/SE5 native I2C. No firmware loading or GPIO fallback.
// Reference provenance: gpi-reference.json. Rings/buffers stay allocated until reboot.
static constexpr uintptr_t se=0xa94000,gcc=0x100000,smmu=0x15000000;
static constexpr unsigned dmaBytes=0x4000,ringCount=64,eventCount=256;
static IOBufferMemoryDescriptor *memory;
static uint8_t *data;
static uint64_t physical;
static uintptr_t base;
static unsigned instance=~0U,phase,result,clockLevel,smmuResult,completed,transactions;
static unsigned clockStep;
static uint32_t clockBranches[4],clockLevels[8];
static unsigned wp[2],eventRp,allocatedMask;
static UmiTouchSmmuPlan smmuPlan;
static UmiGpiTre lastEvent;
static uint32_t errorLog,globalIrq,generalIrq,seIrq;
static bool ready,poisoned,configSent,eventAllocated;
static uint32_t rd(unsigned off){return MmioRead32(base+off);}
static void wr(unsigned off,uint32_t v){MmioWrite32(base+off,v);}
static unsigned ch(unsigned c){return c*0x80;}
static unsigned ringOff(unsigned c){return 0x1000+c*0x400;}
static unsigned state(unsigned off){return (rd(off)>>20)&15;}
static void sync(unsigned off,unsigned bytes) {
    umiGpiDmaSync(data+off,bytes);
}
static bool expired(uint64_t start,unsigned us){return GetTimeInNanoSecond(GetPerformanceCounter()-start)>=uint64_t(us)*1000;}
static void capture(void) {
    if(base){errorLog=rd(0x3200);globalIrq=rd(0x3100);generalIrq=rd(0x3118);}
    seIrq=MmioRead32(se+0x610);
}
static bool command(bool event,unsigned channel,unsigned opcode,unsigned wanted) {
    unsigned status=event?0x3094:0x3090,clear=event?0x30a4:0x30a0,bit=event?1:1U<<channel;
    wr(clear,bit);wr(event?0x3010:0x3008,(opcode<<24)|channel);
    uint64_t start=GetPerformanceCounter();
    do {if(rd(status)&bit){unsigned got=state(event?0x1000:ch(channel));wr(clear,bit);
            if(got==wanted)return true;if(got==15)break;}
        MicroSecondDelay(10);
    }while(!expired(start,50000));
    capture();return false;
}
static bool clocks(void) {
    phase=1;clockStep=0;
    MmioWrite32(gcc+0x52008,MmioRead32(gcc+0x52008)|0x083c0000U);
    if((MmioRead32(gcc+0x52008)&0x083c0000U)!=0x083c0000U){result=100;return false;}
    const unsigned branches[]={0x18004,0x18008,0x23138,0x23140};
    // Snapshot every branch, including those after a failed wait.
    for(unsigned i=0;i<4;++i)clockBranches[i]=MmioRead32(gcc+branches[i]);
    for(unsigned i=0;i<8;++i)clockLevels[i]=MmioRead32(gcc+0x1861c+4*i);
    for(unsigned i=0;i<4;++i){clockStep=i+1;uint64_t start=GetPerformanceCounter();
        do {clockBranches[i]=MmioRead32(gcc+branches[i]);
            if(umiGpiBranchReady(clockBranches[i],i==1))break;
            if(expired(start,10000)){result=101+i;return false;}MicroSecondDelay(10);
        }while(true);
    }
    // Use the hardware's existing XO DFS entry, including bypass encoding.
    unsigned r=0x18600;clockLevel=0;clockStep=5;
    if(MmioRead32(gcc+r+0x14)&1){
        while(clockLevel<8 && !umiGpiXoUndivided(clockLevels[clockLevel]))++clockLevel;
        if(clockLevel==8){result=105;return false;}
    }else{
        clockStep=6;
        MmioWrite32(gcc+r+4,(MmioRead32(gcc+r+4)&~(0x371fU|(1U<<20)))|1);
        MmioWrite32(gcc+r,MmioRead32(gcc+r)|1);uint64_t start=GetPerformanceCounter();
        while(MmioRead32(gcc+r)&1){if(expired(start,5000)){result=106;return false;}MicroSecondDelay(10);}
        if(!umiGpiXoUndivided(MmioRead32(gcc+r+4))){result=107;return false;}
    }
    clockStep=7;return true;
}

static bool idleInstances(void) {
    // DT assigns GPII0..5 to HLOS. Streams are shared, so require every pair
    // to be inactive before changing their common SMMU route.
    for(unsigned n=0;n<6;++n){uintptr_t b=0xa1a000+n*0x4000;
        for(unsigned c=0;c<2;++c){unsigned s=(MmioRead32(b+ch(c))>>20)&15;
            if(s==2 || s==4 || s==15)return false;}}
    return true;
}
static bool initialize(void) {
    if(ready)return true;if(poisoned)return false;
    result=1;if(!clocks())return false;
    phase=2;
    if(((MmioRead32(se+0x68)>>8)&255)!=3 || !(MmioRead32(se+0x64)&1) ||
       (MmioRead32(se+0x40)&0x1001))return false;
    if(!idleInstances())return false;
    for(unsigned n=0;n<6;++n){uintptr_t b=0xa1a000+n*0x4000;
        if(((MmioRead32(b)|MmioRead32(b+0x80)|MmioRead32(b+0x1000))&0xf00000)==0){instance=n;base=b;break;}}
    if(!base)return false; // Never steal/reset another firmware channel.
    phase=3;
    memory=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryPhysicallyContiguous)|IOOptionBits(kIOMemoryMapperNone),dmaBytes,dmaBytes);
    if(!memory)return false;data=(uint8_t *)memory->getBytesNoCopy();IOByteCount contiguous=0;
    physical=memory->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);
    if(!data || (physical&(dmaBytes-1)) || contiguous<dmaBytes ||
       physical<gPhysBase || dmaBytes>gPhysSize || physical-gPhysBase>gPhysSize-dmaBytes)return false;
    phase=4;
    auto read=[](unsigned off){return MmioRead32(smmu+off);};
    smmuPlan=umiTouchSmmuPlan(physical,dmaBytes,read);smmuResult=smmuPlan.error;
    if(smmuResult || !idleInstances())return false;
    smmuResult=umiTouchSmmuApply(smmuPlan,read,[](unsigned off,uint32_t value){MmioWrite32(smmu+off,value);});
    if(smmuResult)return false;
    bzero(data,dmaBytes);sync(0,dmaBytes);
    phase=5;
    // Match Linux GPI contexts. Poll the event ring; no XNU GIC handler required.
    wr(0x3088,0x4f);wr(0x30b8,1);wr(0x3098,3);wr(0x309c,1);
    wr(0x3108,1);wr(0x3120,0xf);wr(0x3188,0);wr(0x318c,0);
    wr(0x3400,0);wr(0x3404,0);wr(0x3180,1);wr(0x3200,0);
    if(!command(true,0,0,1))return false;eventAllocated=true;
    wr(0x1000,0x10010002);wr(0x1004,eventCount*16);
    wr(0x1008,uint32_t(physical));wr(0x100c,uint32_t(physical>>32));
    wr(0x2104,uint32_t(physical>>32));for(unsigned off=0x1020;off<=0x1034;off+=4)wr(off,0);
    wr(0x2100,uint32_t(physical+(eventCount-1)*16));
    for(unsigned c=0;c<2;++c){phase=6+c;
        if(!command(false,c,0,1))return false;allocatedMask|=1U<<c;
        uint64_t p=physical+ringOff(c);unsigned ctx=ch(c);
        wr(ctx,c?0x10000002:0x1000000a);wr(ctx+4,ringCount*16);
        wr(ctx+8,uint32_t(p));wr(ctx+12,uint32_t(p>>32));wr(0x2004+c*8,uint32_t(p>>32));
        wr(ctx+0x60,((!c)<<16)|(3<<4)|5); // pair, I2C, local SE5.
        for(unsigned off=0x64;off<=0x6c;off+=4)wr(ctx+off,0);wr(ctx+0x5c,1);
    }
    phase=8;
    MmioWrite32(se+0xe18,0);
    const unsigned clears[]={0x618,0x648,0xc44,0xd44};
    for(unsigned off:clears)MmioWrite32(se+off,0xffffffff);
    MmioWrite32(se+0x28,MmioRead32(se+0x28)|0x7f);MmioWrite32(se+0xe30,MmioRead32(se+0xe30)|0xf);
    MmioWrite32(se+0x24,0x7f);MmioWrite32(se+0x20,1);MmioWrite32(se+0xe1c,0);
    MmioWrite32(se+0x614,MmioRead32(se+0x614)&~0x4c000001U);
    MmioWrite32(se+0x644,MmioRead32(se+0x644)&~1U);
    MmioWrite32(se+0x258,MmioRead32(se+0x258)|1);MmioWrite32(se+0xe18,0xf);
    for(unsigned c=0;c<2;++c)if(!command(false,c,1,2))return false;
    phase=9;result=0;ready=true;return true;
}
static bool shutdown(void) {
    ready=false;bool stopped=true;
    if(base)for(unsigned c=0;c<2;++c)if(allocatedMask&(1U<<c)){
        unsigned s=state(ch(c));if(s==2 || s==4)stopped=command(false,c,2,3)&&stopped;
        else if(s==15)stopped=false;}
    if(stopped && base)MmioWrite32(se+0xe18,0);
    // A timeout never permits freeing/reusing DMA memory or switching to GPIO.
    poisoned=true;return stopped;
}
static uint64_t enqueue(unsigned c,const UmiGpiTre &tre) {
    unsigned off=ringOff(c)+wp[c]*16;memcpy(data+off,&tre,16);sync(off,16);
    wp[c]=(wp[c]+1)%ringCount;return physical+off;
}
static void doorbell(unsigned c){wr(0x2000+c*8,uint32_t(physical+ringOff(c)+wp[c]*16));}
static unsigned wait(unsigned wanted,const uint64_t expected[2],const unsigned bytes[2],bool read) {
    completed=0;uint64_t start=GetPerformanceCounter();
    do{
        unsigned producerIndex;
        uint64_t ptr=(uint64_t(rd(0x1014))<<32)|rd(0x1010);
        if(!umiGpiRingIndex(ptr,physical,eventCount,producerIndex))return 21;
        unsigned budget=eventCount;
        while(eventRp!=producerIndex && budget--){
            sync(eventRp*16,16);memcpy(&lastEvent,data+eventRp*16,16);
            unsigned c=lastEvent.d[3]>>24;
            if(c>=2 || !(wanted&(1U<<c)) || (completed&(1U<<c)) ||
               umiGpiCompletion(lastEvent,c,expected[c],bytes[c],read && c==0))return 22;
            completed|=1U<<c;unsigned consumed=eventRp;eventRp=(eventRp+1)%eventCount;
            wr(0x2100,uint32_t(physical+consumed*16));wr(0x30c0,1);
        }
        if(rd(0x3100)&1 || rd(0x3118)&15)return 23;
        if(completed==wanted)return 0;
        MicroSecondDelay(10);
    }while(!expired(start,20000));
    return 24;
}
static unsigned transfer(const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn) {
    if(!ready || poisoned)return EFI_NOT_READY;
    if(!w || !wn || wn>0x800 || rn>0x1000 || (rn && !r))return EFI_INVALID_PARAMETER;
    if((MmioRead32(se+0x40)&0x1001) || state(0)!=2 || state(0x80)!=2)return EFI_ACCESS_DENIED;
    // Synchronous messages have at most three TREs; the previous completion
    // consumed the entire previous chain, including after ring wrap.
    phase=10;memcpy(data+0x2000,w,wn);sync(0x2000,wn);
    if(!configSent)enqueue(0,umiGpiConfig(clockLevel));
    enqueue(0,umiGpiGo(false,rn!=0,0));
    uint64_t expected[2]={enqueue(0,umiGpiBuffer(physical+0x2000,wn)),0};unsigned lengths[2]={wn,0};
    doorbell(0);result=wait(1,expected,lengths,false);
    if(!result && rn){phase=11;
        bzero(data+0x3000,rn);sync(0x3000,rn);
        expected[1]=enqueue(1,umiGpiBuffer(physical+0x3000,rn));lengths[1]=rn;doorbell(1);
        expected[0]=enqueue(0,umiGpiGo(true,false,rn));lengths[0]=0;doorbell(0);
        result=wait(3,expected,lengths,true);
    }
    if(result){capture();shutdown();return EFI_DEVICE_ERROR;}
    configSent=true;++transactions;
    if(rn){sync(0x3000,rn);memcpy(r,data+0x3000,rn);}
    phase=12;return EFI_SUCCESS;
}
