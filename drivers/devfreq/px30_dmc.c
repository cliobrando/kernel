// SPDX-License-Identifier: GPL-2.0-only
/*
 * PX30/RK3326 DDR frequency scaling.
 *
 * The rate change itself is done by Rockchip's BL31: the kernel puts the
 * request into a page shared with the firmware and issues
 * ROCKCHIP_SIP_DRAM_FREQ. ROUND_RATE lets the firmware prepare the switch,
 * SET_RATE performs it and raises an interrupt when it is done.
 */

#include <linux/arm-smccc.h>
#include <linux/completion.h>
#include <linux/devfreq.h>
#include <linux/devfreq-event.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>

#include <soc/rockchip/pm_domains.h>
#include <soc/rockchip/rockchip_sip.h>

#define PX30_SIP_SHARE_MEM		0x82000009
#define PX30_SHARE_PAGE_TYPE_DDR	2
#define PX30_DRAM_GET_VERSION		0x08
#define PX30_SET_RATE_PENDING		-6

/* Start of the request page shared with BL31 */
struct px30_dmc_params {
	u32 hz;
	u32 lcdc_type;
	u32 vop;
	u32 vop_dclk_mode;
	u32 sr_idle_en;
	u32 addr_mcu_el3;
	u32 wait_flag1;
	u32 wait_flag0;
	u32 complt_hwirq;
};

struct px30_ddr_timing {
	u32 ddr2_speed_bin;
	u32 ddr3_speed_bin;
	u32 ddr4_speed_bin;
	u32 pd_idle;
	u32 sr_idle;
	u32 sr_mc_gate_idle;
	u32 srpd_lite_idle;
	u32 standby_idle;
	u32 auto_pd_dis_freq;
	u32 auto_sr_dis_freq;
	u32 ddr2_dll_dis_freq;
	u32 ddr3_dll_dis_freq;
	u32 ddr4_dll_dis_freq;
	u32 phy_dll_dis_freq;
	u32 ddr2_odt_dis_freq;
	u32 phy_ddr2_odt_dis_freq;
	u32 ddr2_drv;
	u32 ddr2_odt;
	u32 phy_ddr2_ca_drv;
	u32 phy_ddr2_ck_drv;
	u32 phy_ddr2_dq_drv;
	u32 phy_ddr2_odt;
	u32 ddr3_odt_dis_freq;
	u32 phy_ddr3_odt_dis_freq;
	u32 ddr3_drv;
	u32 ddr3_odt;
	u32 phy_ddr3_ca_drv;
	u32 phy_ddr3_ck_drv;
	u32 phy_ddr3_dq_drv;
	u32 phy_ddr3_odt;
	u32 phy_lpddr2_odt_dis_freq;
	u32 lpddr2_drv;
	u32 phy_lpddr2_ca_drv;
	u32 phy_lpddr2_ck_drv;
	u32 phy_lpddr2_dq_drv;
	u32 phy_lpddr2_odt;
	u32 lpddr3_odt_dis_freq;
	u32 phy_lpddr3_odt_dis_freq;
	u32 lpddr3_drv;
	u32 lpddr3_odt;
	u32 phy_lpddr3_ca_drv;
	u32 phy_lpddr3_ck_drv;
	u32 phy_lpddr3_dq_drv;
	u32 phy_lpddr3_odt;
	u32 lpddr4_odt_dis_freq;
	u32 phy_lpddr4_odt_dis_freq;
	u32 lpddr4_drv;
	u32 lpddr4_dq_odt;
	u32 lpddr4_ca_odt;
	u32 phy_lpddr4_ca_drv;
	u32 phy_lpddr4_ck_cs_drv;
	u32 phy_lpddr4_dq_drv;
	u32 phy_lpddr4_odt;
	u32 ddr4_odt_dis_freq;
	u32 phy_ddr4_odt_dis_freq;
	u32 ddr4_drv;
	u32 ddr4_odt;
	u32 phy_ddr4_ca_drv;
	u32 phy_ddr4_ck_drv;
	u32 phy_ddr4_dq_drv;
	u32 phy_ddr4_odt;
	u32 ca_skew[15];
	u32 cs0_skew[44];
	u32 cs1_skew[44];
	u32 available;
};

static const struct px30_ddr_timing px30_default_timing = {
	.ddr3_speed_bin = 21,
	.ddr4_speed_bin = 12,
	.pd_idle = 13,
	.sr_idle = 93,
	.auto_pd_dis_freq = 1066,
	.auto_sr_dis_freq = 800,
	.ddr2_dll_dis_freq = 300,
	.ddr3_dll_dis_freq = 300,
	.ddr4_dll_dis_freq = 625,
	.phy_dll_dis_freq = 400,
	.ddr2_odt_dis_freq = 100,
	.phy_ddr2_odt_dis_freq = 100,
	.ddr2_drv = 1,
	.ddr2_odt = 150,
	.phy_ddr2_ca_drv = 21,
	.phy_ddr2_ck_drv = 18,
	.phy_ddr2_dq_drv = 21,
	.phy_ddr2_odt = 2,
	.ddr3_odt_dis_freq = 400,
	.phy_ddr3_odt_dis_freq = 400,
	.ddr3_drv = 40,
	.ddr3_odt = 120,
	.phy_ddr3_ca_drv = 21,
	.phy_ddr3_ck_drv = 18,
	.phy_ddr3_dq_drv = 21,
	.phy_ddr3_odt = 2,
	.phy_lpddr2_odt_dis_freq = 666,
	.lpddr2_drv = 40,
	.phy_lpddr2_ca_drv = 22,
	.phy_lpddr2_ck_drv = 19,
	.phy_lpddr2_dq_drv = 22,
	.lpddr3_odt_dis_freq = 400,
	.phy_lpddr3_odt_dis_freq = 400,
	.lpddr3_drv = 40,
	.lpddr3_odt = 240,
	.phy_lpddr3_ca_drv = 22,
	.phy_lpddr3_ck_drv = 19,
	.phy_lpddr3_dq_drv = 22,
	.phy_lpddr3_odt = 2,
	.lpddr4_odt_dis_freq = 800,
	.phy_lpddr4_odt_dis_freq = 800,
	.lpddr4_drv = 60,
	.lpddr4_dq_odt = 40,
	.lpddr4_ca_odt = 40,
	.phy_lpddr4_ca_drv = 20,
	.phy_lpddr4_ck_cs_drv = 6,
	.phy_lpddr4_dq_drv = 6,
	.phy_lpddr4_odt = 16,
	.ddr4_odt_dis_freq = 666,
	.phy_ddr4_odt_dis_freq = 666,
	.ddr4_drv = 34,
	.ddr4_odt = 240,
	.phy_ddr4_ca_drv = 22,
	.phy_ddr4_ck_drv = 19,
	.phy_ddr4_dq_drv = 22,
	.phy_ddr4_odt = 2,
	.ca_skew = {
		0x67, 0x77, 0x77, 0x76, 0x77, 0x77, 0x77, 0x77,
		0x77, 0x77, 0x77, 0x77, 0x77, 0x67, 0x67,
	},
	.cs0_skew = {
		0x77, 0x88, 0x98, 0x88, 0x88, 0x98, 0x98, 0x98,
		0x88, 0x69, 0x09, 0x76, 0x87, 0x97, 0x88, 0x87,
		0x88, 0x97, 0x98, 0x97, 0x79, 0x09, 0x77, 0x99,
		0x79, 0x78, 0x79, 0x99, 0x99, 0x89, 0x89, 0x69,
		0x09, 0x77, 0x88, 0x99, 0x98, 0x98, 0x99, 0x99,
		0x88, 0x88, 0x79, 0x09,
	},
	.cs1_skew = {
		0x77, 0x88, 0x98, 0x88, 0x88, 0x88, 0x98, 0x98,
		0x88, 0x69, 0x09, 0x77, 0x88, 0x87, 0x78, 0x88,
		0x87, 0x88, 0x88, 0x87, 0x79, 0x09, 0x78, 0x89,
		0x89, 0x78, 0x89, 0x99, 0x99, 0x89, 0x89, 0x69,
		0x09, 0x77, 0x89, 0x99, 0x98, 0x88, 0x99, 0x99,
		0x98, 0x88, 0x79, 0x09,
	},
	.available = 1,
};

struct px30_dmc {
	struct device *dev;
	struct devfreq *devfreq;
	struct devfreq_dev_profile profile;
	struct devfreq_simple_ondemand_data ondemand;
	struct devfreq_event_dev *edev;
	struct regulator *vdd;
	struct px30_dmc_params __iomem *params;
	struct completion done;
	struct pm_qos_request qos;
	unsigned long rate;
	unsigned long volt;
};

static struct arm_smccc_res px30_dmc_sip(unsigned long arg0, unsigned long cmd)
{
	struct arm_smccc_res res;

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, arg0, 0, cmd, 0, 0, 0, 0, &res);
	return res;
}

static irqreturn_t px30_dmc_done_irq(int irq, void *data)
{
	struct px30_dmc *dmc = data;

	complete(&dmc->done);
	return IRQ_HANDLED;
}

static int px30_dmc_set_rate(struct px30_dmc *dmc, unsigned long rate)
{
	struct arm_smccc_res res;

	writel(rate, &dmc->params->hz);
	res = px30_dmc_sip(PX30_SHARE_PAGE_TYPE_DDR,
			   ROCKCHIP_SIP_CONFIG_DRAM_ROUND_RATE);
	if (res.a0 || !res.a1)
		return -EINVAL;
	rate = res.a1;

	writel(rate, &dmc->params->hz);

	reinit_completion(&dmc->done);
	res = px30_dmc_sip(PX30_SHARE_PAGE_TYPE_DDR,
			   ROCKCHIP_SIP_CONFIG_DRAM_SET_RATE);
	if ((int)res.a1 == PX30_SET_RATE_PENDING) {
		/* BL31 finishes from FIQ, keep the CPUs out of deep idle */
		cpu_latency_qos_update_request(&dmc->qos, 0);
		if (!wait_for_completion_timeout(&dmc->done, msecs_to_jiffies(85)))
			dev_warn(dmc->dev, "timed out switching to %lu Hz\n", rate);
		cpu_latency_qos_update_request(&dmc->qos, PM_QOS_DEFAULT_VALUE);
	}
	if (res.a0)
		return -EIO;

	res = px30_dmc_sip(PX30_SHARE_PAGE_TYPE_DDR,
			   ROCKCHIP_SIP_CONFIG_DRAM_GET_RATE);
	if (res.a0)
		return -EIO;

	dmc->rate = res.a1;
	return dmc->rate == rate ? 0 : -EIO;
}

static int px30_dmc_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct px30_dmc *dmc = dev_get_drvdata(dev);
	unsigned long old = dmc->rate, rate, volt;
	struct dev_pm_opp *opp;
	int ret;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	rate = dev_pm_opp_get_freq(opp);
	volt = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	if (rate == old)
		return 0;

	/* vdd_logic also feeds the GPU, so only ask for a minimum */
	if (rate > old) {
		ret = regulator_set_voltage(dmc->vdd, volt, INT_MAX);
		if (ret)
			return ret;
	}

	ret = rockchip_pmu_block();
	if (!ret) {
		ret = px30_dmc_set_rate(dmc, rate);
		rockchip_pmu_unblock();
	}
	if (ret) {
		if (rate > old)
			regulator_set_voltage(dmc->vdd, dmc->volt, INT_MAX);
		return ret;
	}

	if (rate < old)
		regulator_set_voltage(dmc->vdd, volt, INT_MAX);

	dmc->volt = volt;
	*freq = dmc->rate;
	return 0;
}

static int px30_dmc_get_dev_status(struct device *dev,
				   struct devfreq_dev_status *stat)
{
	struct px30_dmc *dmc = dev_get_drvdata(dev);
	struct devfreq_event_data edata;
	int ret;

	ret = devfreq_event_get_event(dmc->edev, &edata);
	if (ret < 0)
		return ret;

	stat->current_frequency = dmc->rate;
	stat->busy_time = edata.load_count;
	stat->total_time = edata.total_count;
	return 0;
}

static int px30_dmc_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct px30_dmc *dmc = dev_get_drvdata(dev);

	*freq = dmc->rate;
	return 0;
}

static void px30_dmc_disable_edev(void *data)
{
	devfreq_event_disable_edev(data);
}

static void px30_dmc_remove_qos(void *data)
{
	cpu_latency_qos_remove_request(data);
}

static int px30_dmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct arm_smccc_res res;
	struct dev_pm_opp *opp;
	struct px30_dmc *dmc;
	unsigned long freq;
	int irq, ret;

	res = px30_dmc_sip(0, PX30_DRAM_GET_VERSION);
	if (res.a0 || res.a1 < 0x103)
		return dev_err_probe(dev, -ENODEV,
				     "BL31 without DRAM DVFS (0x%lx)\n", res.a1);

	dmc = devm_kzalloc(dev, sizeof(*dmc), GFP_KERNEL);
	if (!dmc)
		return -ENOMEM;
	dmc->dev = dev;
	init_completion(&dmc->done);
	platform_set_drvdata(pdev, dmc);

	dmc->vdd = devm_regulator_get(dev, "center");
	if (IS_ERR(dmc->vdd))
		return dev_err_probe(dev, PTR_ERR(dmc->vdd),
				     "failed to get center supply\n");

	dmc->edev = devfreq_event_get_edev_by_phandle(dev, "devfreq-events", 0);
	if (IS_ERR(dmc->edev))
		return -EPROBE_DEFER;

	arm_smccc_smc(PX30_SIP_SHARE_MEM, 2, PX30_SHARE_PAGE_TYPE_DDR, 0,
		      0, 0, 0, 0, &res);
	if (res.a0)
		return dev_err_probe(dev, -ENOMEM, "no BL31 share memory\n");
	dmc->params = devm_ioremap(dev, res.a1, 2 * SZ_4K);
	if (!dmc->params)
		return -ENOMEM;
	memset_io(dmc->params, 0, 2 * SZ_4K);
	memcpy_toio((void __iomem *)dmc->params + SZ_4K, &px30_default_timing,
		    sizeof(px30_default_timing));

	irq = platform_get_irq_byname(pdev, "complete");
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, px30_dmc_done_irq, 0, dev_name(dev), dmc);
	if (ret)
		return ret;
	writel(irqd_to_hwirq(irq_get_irq_data(irq)), &dmc->params->complt_hwirq);

	res = px30_dmc_sip(PX30_SHARE_PAGE_TYPE_DDR,
			   ROCKCHIP_SIP_CONFIG_DRAM_INIT);
	if (res.a0)
		return dev_err_probe(dev, -EIO, "BL31 DRAM init failed\n");

	res = px30_dmc_sip(PX30_SHARE_PAGE_TYPE_DDR,
			   ROCKCHIP_SIP_CONFIG_DRAM_GET_RATE);
	if (res.a0)
		return dev_err_probe(dev, -EIO, "BL31 DRAM get rate failed\n");
	dmc->rate = res.a1;

	ret = devm_pm_opp_of_add_table(dev);
	if (ret)
		return dev_err_probe(dev, ret, "invalid operating points\n");

	ret = devfreq_event_enable_edev(dmc->edev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, px30_dmc_disable_edev, dmc->edev);
	if (ret)
		return ret;

	cpu_latency_qos_add_request(&dmc->qos, PM_QOS_DEFAULT_VALUE);
	ret = devm_add_action_or_reset(dev, px30_dmc_remove_qos, &dmc->qos);
	if (ret)
		return ret;

	freq = dmc->rate;
	opp = devfreq_recommended_opp(dev, &freq, DEVFREQ_FLAG_LEAST_UPPER_BOUND);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dmc->volt = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);
	ret = regulator_set_voltage(dmc->vdd, dmc->volt, INT_MAX);
	if (ret)
		return ret;

	dmc->ondemand.upthreshold = 40;
	dmc->ondemand.downdifferential = 20;
	dmc->profile = (struct devfreq_dev_profile) {
		.polling_ms	= 50,
		.target		= px30_dmc_target,
		.get_dev_status	= px30_dmc_get_dev_status,
		.get_cur_freq	= px30_dmc_get_cur_freq,
		.initial_freq	= freq,
	};

	dmc->devfreq = devm_devfreq_add_device(dev, &dmc->profile,
					       DEVFREQ_GOV_SIMPLE_ONDEMAND,
					       &dmc->ondemand);
	if (IS_ERR(dmc->devfreq))
		return PTR_ERR(dmc->devfreq);

	return devm_devfreq_register_opp_notifier(dev, dmc->devfreq);
}

static int px30_dmc_suspend(struct device *dev)
{
	struct px30_dmc *dmc = dev_get_drvdata(dev);
	int ret;

	ret = devfreq_event_disable_edev(dmc->edev);
	if (ret < 0)
		return ret;

	return devfreq_suspend_device(dmc->devfreq);
}

static int px30_dmc_resume(struct device *dev)
{
	struct px30_dmc *dmc = dev_get_drvdata(dev);
	int ret;

	ret = devfreq_event_enable_edev(dmc->edev);
	if (ret < 0)
		return ret;

	return devfreq_resume_device(dmc->devfreq);
}

static DEFINE_SIMPLE_DEV_PM_OPS(px30_dmc_pm, px30_dmc_suspend, px30_dmc_resume);

static const struct of_device_id px30_dmc_of_match[] = {
	{ .compatible = "rockchip,px30-dmc" },
	{ }
};
MODULE_DEVICE_TABLE(of, px30_dmc_of_match);

static struct platform_driver px30_dmc_driver = {
	.probe	= px30_dmc_probe,
	.driver = {
		.name		= "px30-dmc",
		.pm		= pm_sleep_ptr(&px30_dmc_pm),
		.of_match_table	= px30_dmc_of_match,
	},
};
module_platform_driver(px30_dmc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PX30 DDR frequency scaling through Rockchip BL31");
