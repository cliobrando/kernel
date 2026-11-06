// SPDX-License-Identifier: GPL-2.0
/*
 * RK3326 CPU quality grade for EmulationStation's info menu.
 *
 * The Rockchip BSP picks a voltage table from a ring oscillator measurement
 * (PVTM) taken at 408 MHz and 1.0 V, with the thresholds of px30.dtsi. This
 * kernel uses fixed voltages, so the grade is only reported, in the
 * "es_info:" line ES reads from dmesg. Same steps as the BSP's
 * rockchip_get_pvtm_specific_value(), for the "core" PVTM, ring 0.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/thermal.h>
#include <linux/units.h>
#include <soc/rockchip/rk3326_pvtm.h>

#define PVTM_CON		0x80
#define PVTM_CAL		(PVTM_CON + 0x4)
#define PVTM_STA		0x88
#define PVTM_FREQ		(PVTM_STA + 0x4)
#define PVTM_START		BIT(0)
#define PVTM_EN			BIT(1)
#define PVTM_RING_SEL_SHIFT	2
#define PVTM_RING_SEL_MASK	0x3
#define PVTM_DONE		BIT(0)
#define HIWORD(bits, val)	(((bits) << 16) | (val))

#define PVTM_CPU_HZ		408000000
#define PVTM_CPU_UV		1000000
#define PVTM_SAMPLE_US		1000
#define PVTM_SAMPLES		10
#define PVTM_MAX_SPREAD		1000
#define PVTM_REF_TEMP		40
#define PVTM_TEMP_PROP		(-56)

/* rockchip,pvtm-voltage-sel of the BSP's px30.dtsi: upper limit of each grade */
static const int pvtm_grade_max[] = { 50000, 54000, 60000 };

static int pvtm_sample(struct regmap *grf)
{
	unsigned int sta, val = 0;
	int tries = 100;

	regmap_read(grf, PVTM_CON, &sta);
	if (sta & PVTM_EN)
		regmap_write(grf, PVTM_CON, HIWORD(PVTM_START, 0));

	regmap_write(grf, PVTM_CON, HIWORD(PVTM_EN, PVTM_EN));
	regmap_write(grf, PVTM_CON,
		     HIWORD(PVTM_RING_SEL_MASK << PVTM_RING_SEL_SHIFT, 0));
	/* 24 MHz reference */
	regmap_write(grf, PVTM_CAL, PVTM_SAMPLE_US * 24);
	regmap_write(grf, PVTM_CON, HIWORD(PVTM_START, PVTM_START));

	usleep_range(PVTM_SAMPLE_US, PVTM_SAMPLE_US + 100);

	while (tries--) {
		regmap_read(grf, PVTM_STA, &sta);
		if (sta & PVTM_DONE) {
			regmap_read(grf, PVTM_FREQ, &val);
			break;
		}
		udelay(4);
	}

	if (!val) {
		unsigned int con, cal;

		regmap_read(grf, PVTM_CON, &con);
		regmap_read(grf, PVTM_CAL, &cal);
		regmap_read(grf, PVTM_STA, &sta);
		pr_warn("rk3326-pvtm: no result: con %#x cal %#x sta %#x\n", con, cal, sta);
	}

	regmap_write(grf, PVTM_CON, HIWORD(PVTM_START, 0));
	regmap_write(grf, PVTM_CON, HIWORD(PVTM_EN, 0));

	return val;
}

static int pvtm_measure(struct regmap *grf)
{
	int retry = 2, total = 0;

	/* The first values can be off: retry once if they spread too much */
	while (retry--) {
		int i, min = INT_MAX, max = 0;

		total = 0;
		for (i = 0; i < PVTM_SAMPLES; i++) {
			int val = pvtm_sample(grf);

			if (val <= 0)
				return -EIO;
			min = min(min, val);
			max = max(max, val);
			total += val;
		}
		if (max - min < PVTM_MAX_SPREAD)
			break;
	}

	return total / PVTM_SAMPLES;
}

void rk3326_pvtm_report(struct device *cpu_dev)
{
	struct thermal_zone_device *tz;
	struct regulator *reg;
	struct device_node *np;
	unsigned long old_rate;
	struct regmap *grf;
	struct clk *clk, *pclk;
	int old_uv, avg = 0, value, sel, temp = PVTM_REF_TEMP * MILLIDEGREE_PER_DEGREE;
	bool have_temp = false;

	if (!of_machine_is_compatible("rockchip,rk3326"))
		return;

	np = of_find_compatible_node(NULL, NULL, "rockchip,px30-pvtm");
	if (!np) {
		pr_warn("rk3326-pvtm: no pvtm node\n");
		return;
	}

	grf = syscon_regmap_lookup_by_compatible("rockchip,px30-grf");
	pclk = of_clk_get(np, 0);
	of_node_put(np);
	if (IS_ERR(grf) || IS_ERR(pclk))
		goto no_pvtm;

	clk = clk_get(cpu_dev, NULL);
	if (IS_ERR(clk)) {
		pr_warn("rk3326-pvtm: no CPU clock: %ld\n", PTR_ERR(clk));
		goto put_pclk;
	}
	reg = regulator_get(cpu_dev, "cpu");
	if (IS_ERR(reg)) {
		pr_warn("rk3326-pvtm: no CPU regulator: %ld\n", PTR_ERR(reg));
		goto put_clk;
	}

	old_rate = clk_get_rate(clk);
	old_uv = regulator_get_voltage(reg);

	/* Frequency down first, and back up last */
	if (clk_set_rate(clk, PVTM_CPU_HZ)) {
		pr_warn("rk3326-pvtm: can't set %d Hz\n", PVTM_CPU_HZ);
		goto put_reg;
	}
	if (regulator_set_voltage(reg, PVTM_CPU_UV, PVTM_CPU_UV)) {
		pr_warn("rk3326-pvtm: can't set %d uV\n", PVTM_CPU_UV);
		goto restore_clk;
	}

	if (clk_prepare_enable(pclk))
		goto restore_volt;
	avg = pvtm_measure(grf);
	clk_disable_unprepare(pclk);

	tz = thermal_zone_get_zone_by_name("soc-thermal");
	if (!IS_ERR(tz) && !thermal_zone_get_temp(tz, &temp))
		have_temp = true;

restore_volt:
	regulator_set_voltage(reg, old_uv, INT_MAX);
restore_clk:
	clk_set_rate(clk, old_rate);

	if (avg <= 0)
		pr_warn("rk3326-pvtm: measurement failed: %d\n", avg);

	if (avg > 0) {
		value = avg + (temp / MILLIDEGREE_PER_DEGREE - PVTM_REF_TEMP) *
			      PVTM_TEMP_PROP;
		for (sel = 0; sel < ARRAY_SIZE(pvtm_grade_max); sel++)
			if (value <= pvtm_grade_max[sel])
				break;
		pr_info("rk3326-pvtm: pvtm=%d (raw %d, temp %d%s)\n", value, avg,
			temp, have_temp ? "" : " assumed");
		pr_info("es_info: cpu_bin=-1 process=-1 scale=0 volt_sel=%d\n", sel);
	}

put_reg:
	regulator_put(reg);
put_clk:
	clk_put(clk);
put_pclk:
	clk_put(pclk);
	return;

no_pvtm:
	if (!IS_ERR(pclk))
		clk_put(pclk);
	pr_warn("rk3326-pvtm: no GRF or clock, CPU grade unknown\n");
}
