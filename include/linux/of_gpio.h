/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * OF helpers for the legacy GPIO API
 *
 * Copyright (c) 2007-2008  MontaVista Software, Inc.
 *
 * Author: Anton Vorontsov <avorontsov@ru.mvista.com>
 *
 * Removed from mainline in 7.1; restored here for the rocknix-joypad
 * drivers. Deliberately minimal — just enum of_gpio_flags and the
 * of_get_named_gpio_flags()/of_get_gpio_flags() helpers the drivers use —
 * so it can be included by gpiolib-of.c without clashing with the
 * fwnode-based helpers in gpiolib-of.h. of_gpio_legacy.h shares the
 * include guard so including both (as the joypad drivers do) is harmless.
 */

#ifndef __LINUX_OF_GPIO_H
#define __LINUX_OF_GPIO_H

#include <linux/compiler.h>
#include <linux/gpio/driver.h>
#include <linux/gpio.h>
#include <linux/of.h>

struct device_node;

/*
 * This is Linux-specific flags. By default controllers' and Linux' mapping
 * match, but GPIO controllers are free to translate their own flags to
 * Linux-specific in their .xlate callback. Though, 1:1 mapping is recommended.
 */
enum of_gpio_flags {
	OF_GPIO_ACTIVE_LOW = 0x1,
	OF_GPIO_SINGLE_ENDED = 0x2,
	OF_GPIO_OPEN_DRAIN = 0x4,
	OF_GPIO_TRANSITORY = 0x8,
	OF_GPIO_PULL_UP = 0x10,
	OF_GPIO_PULL_DOWN = 0x20,
	OF_GPIO_PULL_DISABLE = 0x40,
};

#ifdef CONFIG_OF_GPIO

extern int of_get_named_gpio_flags(const struct device_node *np,
		const char *list_name, int index, enum of_gpio_flags *flags);

#else /* CONFIG_OF_GPIO */

#include <linux/errno.h>

/* Drivers may not strictly depend on the GPIO support, so let them link. */
static inline int of_get_named_gpio_flags(const struct device_node *np,
		const char *list_name, int index, enum of_gpio_flags *flags)
{
	if (flags)
		*flags = 0;

	return -ENOSYS;
}

#endif /* CONFIG_OF_GPIO */

static inline int of_get_gpio_flags(const struct device_node *np, int index,
		      enum of_gpio_flags *flags)
{
	return of_get_named_gpio_flags(np, "gpios", index, flags);
}

#endif /* __LINUX_OF_GPIO_H */
