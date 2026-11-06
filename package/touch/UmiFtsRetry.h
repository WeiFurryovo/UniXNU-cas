// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Original umi I2C driver permits three attempts, separated by 2 ms.
// Only a FIFO read may retry, after its bounded STOP actually released the bus.
static inline bool umi_fts_retry_read(unsigned result,unsigned consecutiveFailures,
                                      bool stopSucceeded,bool linesHigh,
                                      bool pinsAndPowerValid,bool geniIdle) {
    return (result==1 || result==12) && consecutiveFailures>0 && consecutiveFailures<3 &&
           stopSucceeded && linesHigh && pinsAndPowerValid && geniIdle;
}
