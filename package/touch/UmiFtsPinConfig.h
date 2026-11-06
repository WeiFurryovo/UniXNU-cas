// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
// running.dts ts_active: reset GPIO38 and IRQ GPIO39 pull-up, 8 mA.
// GPIO69 belongs to a regulator-fixed without a pinconf state: gpio_direction
// must preserve the inherited drive/pull fields, as Linux pinctrl-msm does.
static uint32_t umi_fts_power_pin_config(uint32_t ctl,unsigned pin) {
    ctl &= ~(0x3cU | (1U<<9)); // GPIO mux; temporarily disable output.
    if(pin==38)ctl=(ctl&~0x1c3U)|3U|(3U<<6);
    if(ctl&(1U<<11))ctl|=1U<<12;
    return ctl;
}
