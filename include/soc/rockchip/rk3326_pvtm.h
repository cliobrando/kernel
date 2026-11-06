/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_ROCKCHIP_RK3326_PVTM_H
#define __SOC_ROCKCHIP_RK3326_PVTM_H

struct device;

#if IS_ENABLED(CONFIG_ROCKCHIP_RK3326_PVTM)
void rk3326_pvtm_report(struct device *cpu_dev);
#else
static inline void rk3326_pvtm_report(struct device *cpu_dev) { }
#endif

#endif
