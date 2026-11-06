// SPDX-License-Identifier: BSD-2-Clause-Patent
// Single-owner FTS worker log. Formatting is bounded and never allocates.
#pragma once
#include <stdarg.h>
#include <stddef.h>
#if !defined(KERNEL)
#include <stdio.h>
#endif
struct UmiTouchLog {
    enum { Capacity=64, Width=200 };
    char lines[Capacity][Width];
    unsigned count,dropped;
    void append(const char *format,va_list args) {
        if(count==Capacity){++dropped;return;}
        vsnprintf(lines[count],Width,format,args);++count;
    }
    void flush(bool safe,void (*output)(const char *)) {
        // IOLog can sleep and asserts on an IRQ-masked caller. The caller
        // supplies its actual XNU IRQ/context check; never enable IRQs here.
        if(!safe)return;
        for(unsigned i=0;i<count;++i)output(lines[i]);
        count=0;
    }
};
