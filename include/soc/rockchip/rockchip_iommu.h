/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The BSP's hooks into the Rockchip IOMMU that the MPP driver uses: it has
 * to switch the IOMMU off and on around a VPU reset and mask a page fault
 * until the job times out.
 */
#ifndef __SOC_ROCKCHIP_IOMMU_H
#define __SOC_ROCKCHIP_IOMMU_H

struct device;

int rockchip_iommu_enable(struct device *dev);
int rockchip_iommu_disable(struct device *dev);
bool rockchip_iommu_is_enabled(struct device *dev);
void rockchip_iommu_mask_irq(struct device *dev);

#endif
