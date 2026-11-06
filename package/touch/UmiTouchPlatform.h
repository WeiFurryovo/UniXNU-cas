// SPDX-License-Identifier: BSD-2-Clause-Patent
// XNU adapters for the handset-qualified UEFI transport. No EFI protocols.
#pragma once
#include <IOKit/IOLib.h>
#include <machine/machine_routines.h>
#include <stdint.h>
#define STATIC static
#define CONST const
#define VOID void
#define BIT0 (1U<<0)
#define BIT1 (1U<<1)
#define BIT2 (1U<<2)
#define BIT3 (1U<<3)
#define BIT7 (1U<<7)
#define BIT8 (1U<<8)
#define BIT9 (1U<<9)
#define BIT11 (1U<<11)
#define BIT12 (1U<<12)
#define BIT16 (1U<<16)
#define BIT24 (1U<<24)
#define MAX_UINT32 UINT32_MAX
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
typedef uint8_t UINT8;typedef uint16_t UINT16;typedef uint32_t UINT32;typedef uint64_t UINT64;
typedef uintptr_t UINTN;typedef intptr_t INTN;typedef bool BOOLEAN;typedef int EFI_STATUS;
enum {EFI_SUCCESS=0,EFI_TIMEOUT=1,EFI_DEVICE_ERROR=2,EFI_INVALID_PARAMETER=3,EFI_UNSUPPORTED=4,
 EFI_NOT_FOUND=5,EFI_ACCESS_DENIED=6,EFI_COMPROMISED_DATA=7,EFI_NOT_READY=8,EFI_ALREADY_STARTED=9,
 EFI_WRITE_PROTECTED=10,EFI_PROTOCOL_ERROR=11,EFI_NO_RESPONSE=12};
#define EFI_ERROR(x) ((x)!=EFI_SUCCESS)
#define DEBUG_WARN 0
static void touchDebug(unsigned,const char *format,...) __printflike(2,3);
#define DEBUG(x) touchDebug x
static uint32_t MmioRead32(uintptr_t);
static uint16_t MmioRead16(uintptr_t);
static void MmioWrite32(uintptr_t,uint32_t);
static uint64_t frequency;
static inline uint64_t GetPerformanceCounter(void) {uint64_t n;__asm__ volatile("isb\n\tmrs %0,cntpct_el0":"=r"(n));return n;}
static inline uint64_t GetTimeInNanoSecond(uint64_t delta) {return delta/frequency*1000000000ULL+(delta%frequency)*1000000000ULL/frequency;}
static inline void MicroSecondDelay(unsigned us) {
    // GPIO clock edges must not deliberately block the worker in IODelay.
    // Keep power/reset waits sleepable; leave IRQs and preemption enabled.
    if(us>50 || !frequency){IODelay(us);return;}
    uint64_t start=GetPerformanceCounter(),ticks=(frequency*us+999999)/1000000;
    while(GetPerformanceCounter()-start<ticks){}
}
static inline void MemoryFence(void) {__asm__ volatile("dsb sy":::"memory");}
static inline bool SaveAndDisableInterrupts(void) {return ml_set_interrupts_enabled(false);}
static inline void SetInterruptState(bool state) {ml_set_interrupts_enabled(state);}
