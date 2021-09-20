/*
 * Synopsys DesignWare I2C adapter driver.
 *
 * Based on the TI DAVINCI I2C adapter driver.
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 *
 * ----------------------------------------------------------------------------
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * ----------------------------------------------------------------------------
 *
 */
#include <linux/acpi.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_data/i2c-designware.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "i2c-designware-core.h"

static u32 i2c_dw_get_clk_rate_khz(struct dw_i2c_dev *dev)
{
	return clk_get_rate(dev->clk)/1000;
}

#ifdef CONFIG_ACPI
/*
 * The HCNT/LCNT information coming from ACPI should be the most accurate
 * for given platform. However, some systems get it wrong. On such systems
 * we get better results by calculating those based on the input clock.
 */
static const struct dmi_system_id dw_i2c_no_acpi_params[] = {
	{
		.ident = "Dell Inspiron 7348",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Inspiron 7348"),
		},
	},
	{ }
};

static void dw_i2c_acpi_params(struct platform_device *pdev, char method[],
			       u16 *hcnt, u16 *lcnt, u32 *sda_hold)
{
	struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER };
	acpi_handle handle = ACPI_HANDLE(&pdev->dev);
	union acpi_object *obj;

	if (dmi_check_system(dw_i2c_no_acpi_params))
		return;

	if (ACPI_FAILURE(acpi_evaluate_object(handle, method, NULL, &buf)))
		return;

	obj = (union acpi_object *)buf.pointer;
	if (obj->type == ACPI_TYPE_PACKAGE && obj->package.count == 3) {
		const union acpi_object *objs = obj->package.elements;

		*hcnt = (u16)objs[0].integer.value;
		*lcnt = (u16)objs[1].integer.value;
		*sda_hold = (u32)objs[2].integer.value;
	}

	kfree(buf.pointer);
}

static int dw_i2c_acpi_configure(struct platform_device *pdev)
{
	struct dw_i2c_dev *dev = platform_get_drvdata(pdev);
	u32 ss_ht = 0, fp_ht = 0, hs_ht = 0, fs_ht = 0;
	acpi_handle handle = ACPI_HANDLE(&pdev->dev);
	const struct acpi_device_id *id;
	struct acpi_device *adev;
	const char *uid;

	dev->adapter.nr = -1;
	dev->tx_fifo_depth = 32;
	dev->rx_fifo_depth = 32;

	/*
	 * Try to get SDA hold time and *CNT values from an ACPI method for
	 * selected speed modes.
	 */
	dw_i2c_acpi_params(pdev, "SSCN", &dev->ss_hcnt, &dev->ss_lcnt, &ss_ht);
	dw_i2c_acpi_params(pdev, "FPCN", &dev->fp_hcnt, &dev->fp_lcnt, &fp_ht);
	dw_i2c_acpi_params(pdev, "HSCN", &dev->hs_hcnt, &dev->hs_lcnt, &hs_ht);
	dw_i2c_acpi_params(pdev, "FMCN", &dev->fs_hcnt, &dev->fs_lcnt, &fs_ht);

	switch (dev->clk_freq) {
	case 100000:
		dev->sda_hold_time = ss_ht;
		break;
	case 1000000:
		dev->sda_hold_time = fp_ht;
		break;
	case 3400000:
		dev->sda_hold_time = hs_ht;
		break;
	case 400000:
	default:
		dev->sda_hold_time = fs_ht;
		break;
	}

	id = acpi_match_device(pdev->dev.driver->acpi_match_table, &pdev->dev);
	if (id && id->driver_data)
		dev->flags |= (u32)id->driver_data;

	if (acpi_bus_get_device(handle, &adev))
		return -ENODEV;

	/*
	 * Cherrytrail I2C7 gets used for the PMIC which gets accessed
	 * through ACPI opregions during late suspend / early resume
	 * disable pm for it.
	 */
	uid = adev->pnp.unique_id;
	if ((dev->flags & MODEL_CHERRYTRAIL) && !strcmp(uid, "7"))
		dev->pm_disabled = true;

	return 0;
}

static const struct acpi_device_id dw_i2c_acpi_match[] = {
	{ "INT33C2", 0 },
	{ "INT33C3", 0 },
	{ "INT3432", 0 },
	{ "INT3433", 0 },
	{ "80860F41", 0 },
	{ "808622C1", MODEL_CHERRYTRAIL },
	{ "AMD0010", ACCESS_INTR_MASK },
	{ "AMDI0010", ACCESS_INTR_MASK },
	{ "AMDI0510", 0 },
	{ "APMC0D0F", 0 },
	{ "HISI02A1", 0 },
	{ "HISI02A2", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, dw_i2c_acpi_match);
#else
static inline int dw_i2c_acpi_configure(struct platform_device *pdev)
{
	return -ENODEV;
}
#endif

static void i2c_dw_configure_master(struct dw_i2c_dev *dev)
{
	dev->functionality = I2C_FUNC_10BIT_ADDR | DW_IC_DEFAULT_FUNCTIONALITY;

	dev->master_cfg = DW_IC_CON_MASTER | DW_IC_CON_SLAVE_DISABLE |
			  DW_IC_CON_RESTART_EN;

	dev->mode = DW_IC_MASTER;

	switch (dev->clk_freq) {
	case 100000:
		dev->master_cfg |= DW_IC_CON_SPEED_STD;
		break;
	case 3400000:
		dev->master_cfg |= DW_IC_CON_SPEED_HIGH;
		break;
	default:
		dev->master_cfg |= DW_IC_CON_SPEED_FAST;
	}
}

static void i2c_dw_configure_slave(struct dw_i2c_dev *dev)
{
	dev->functionality = I2C_FUNC_SLAVE | DW_IC_DEFAULT_FUNCTIONALITY;

	dev->slave_cfg = DW_IC_CON_RX_FIFO_FULL_HLD_CTRL |
			 DW_IC_CON_RESTART_EN | DW_IC_CON_STOP_DET_IFADDRESSED;

	dev->mode = DW_IC_SLAVE;

	switch (dev->clk_freq) {
	case 100000:
		dev->slave_cfg |= DW_IC_CON_SPEED_STD;
		break;
	case 3400000:
		dev->slave_cfg |= DW_IC_CON_SPEED_HIGH;
		break;
	default:
		dev->slave_cfg |= DW_IC_CON_SPEED_FAST;
	}
}

static int i2c_dw_plat_prepare_clk(struct dw_i2c_dev *i_dev, bool prepare)
{
	if (IS_ERR(i_dev->clk))
		return PTR_ERR(i_dev->clk);

	if (prepare)
		return clk_prepare_enable(i_dev->clk);

	clk_disable_unprepare(i_dev->clk);
	return 0;
}

static void dw_i2c_set_fifo_size(struct dw_i2c_dev *dev, int id)
{
	u32 param, tx_fifo_depth, rx_fifo_depth;

	/*
	 * Try to detect the FIFO depth if not set by interface driver,
	 * the depth could be from 2 to 256 from HW spec.
	 */
	param = i2c_dw_read_comp_param(dev);
	tx_fifo_depth = ((param >> 16) & 0xff) + 1;
	rx_fifo_depth = ((param >> 8)  & 0xff) + 1;
	if (!dev->tx_fifo_depth) {
		dev->tx_fifo_depth = tx_fifo_depth;
		dev->rx_fifo_depth = rx_fifo_depth;
		dev->adapter.nr = id;
	} else if (tx_fifo_depth >= 2) {
		dev->tx_fifo_depth = min_t(u32, dev->tx_fifo_depth,
				tx_fifo_depth);
		dev->rx_fifo_depth = min_t(u32, dev->rx_fifo_depth,
				rx_fifo_depth);
	}
}

static void dw_i2c_plat_pm_cleanup(struct dw_i2c_dev *dev)
{
	pm_runtime_disable(dev->dev);

	if (dev->pm_disabled)
		pm_runtime_put_noidle(dev->dev);
}

static int dw_i2c_plat_probe(struct platform_device *pdev)
{
	struct dw_i2c_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct i2c_adapter *adap;
	struct dw_i2c_dev *dev;
	u32 acpi_speed, ht = 0;
	struct resource *mem;
	int i, irq, ret;
	static const int supported_speeds[] = {
		0, 100000, 400000, 1000000, 3400000
	};

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	dev = devm_kzalloc(&pdev->dev, sizeof(struct dw_i2c_dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dev->base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(dev->base))
		return PTR_ERR(dev->base);

	dev->dev = &pdev->dev;
	dev->irq = irq;
	platform_set_drvdata(pdev, dev);

	dev->rst = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (IS_ERR(dev->rst)) {
		if (PTR_ERR(dev->rst) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
	} else {
		reset_control_deassert(dev->rst);
	}

	if (pdata) {
		dev->clk_freq = pdata->i2c_scl_freq;
	} else {
		device_property_read_u32(&pdev->dev, "i2c-sda-hold-time-ns",
					 &ht);
		device_property_read_u32(&pdev->dev, "i2c-sda-falling-time-ns",
					 &dev->sda_falling_time);
		device_property_read_u32(&pdev->dev, "i2c-scl-falling-time-ns",
					 &dev->scl_falling_time);
		device_property_read_u32(&pdev->dev, "clock-frequency",
					 &dev->clk_freq);
	}

	acpi_speed = i2c_acpi_find_bus_speed(&pdev->dev);
	/*
	 * Some DSTDs use a non standard speed, round down to the lowest
	 * standard speed.
	 */
	for (i = 1; i < ARRAY_SIZE(supported_speeds); i++) {
		if (acpi_speed < supported_speeds[i])
			break;
	}
	acpi_speed = supported_speeds[i - 1];

	/*
	 * Find bus speed from the "clock-frequency" device property, ACPI
	 * or by using fast mode if neither is set.
	 */
	if (acpi_speed && dev->clk_freq)
		dev->clk_freq = min(dev->clk_freq, acpi_speed);
	else if (acpi_speed || dev->clk_freq)
		dev->clk_freq = max(dev->clk_freq, acpi_speed);
	else
		dev->clk_freq = 400000;

	if (has_acpi_companion(&pdev->dev))
		dw_i2c_acpi_configure(pdev);

	/*
	 * Only standard mode at 100kHz, fast mode at 400kHz,
	 * fast mode plus at 1MHz and high speed mode at 3.4MHz are supported.
	 */
	if (dev->clk_freq != 100000 && dev->clk_freq != 400000
	    && dev->clk_freq != 1000000 && dev->clk_freq != 3400000) {
		dev_err(&pdev->dev,
			"%d Hz is unsupported, only 100kHz, 400kHz, 1MHz and 3.4MHz are supported\n",
			dev->clk_freq);
		ret = -EINVAL;
		goto exit_reset;
	}

	ret = i2c_dw_probe_lock_support(dev);
	if (ret)
		goto exit_reset;

	if (i2c_detect_slave_mode(&pdev->dev))
		i2c_dw_configure_slave(dev);
	else
		i2c_dw_configure_master(dev);

	dev->clk = devm_clk_get(&pdev->dev, NULL);
	if (!i2c_dw_plat_prepare_clk(dev, true)) {
		dev->get_clk_rate_khz = i2c_dw_get_clk_rate_khz;

		if (!dev->sda_hold_time && ht)
			dev->sda_hold_time = div_u64(
				(u64)dev->get_clk_rate_khz(dev) * ht + 500000,
				1000000);
	}

	dw_i2c_set_fifo_size(dev, pdev->id);

	adap = &dev->adapter;
	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_DEPRECATED;
	ACPI_COMPANION_SET(&adap->dev, ACPI_COMPANION(&pdev->dev));
	adap->dev.of_node = pdev->dev.of_node;

	/* The code below assumes runtime PM to be disabled. */
	WARN_ON(pm_runtime_enabled(&pdev->dev));

	pm_runtime_set_autosuspend_delay(&pdev->dev, 1000);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);

	if (dev->pm_disabled)
		pm_runtime_get_noresume(&pdev->dev);

	pm_runtime_enable(&pdev->dev);

	if (dev->mode == DW_IC_SLAVE)
		ret = i2c_dw_probe_slave(dev);
	else
		ret = i2c_dw_probe(dev);

	if (ret)
		goto exit_probe;

	return ret;

exit_probe:
	dw_i2c_plat_pm_cleanup(dev);
exit_reset:
	if (!IS_ERR_OR_NULL(dev->rst))
		reset_control_assert(dev->rst);
	return ret;
}

static int dw_i2c_plat_remove(struct platform_device *pdev)
{
	struct dw_i2c_dev *dev = platform_get_drvdata(pdev);

	pm_runtime_get_sync(&pdev->dev);

	i2c_del_adapter(&dev->adapter);

	dev->disable(dev);

	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_put_sync(&pdev->dev);
	dw_i2c_plat_pm_cleanup(dev);

	if (!IS_ERR_OR_NULL(dev->rst))
		reset_control_assert(dev->rst);

	i2c_dw_remove_lock_support(dev);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id dw_i2c_of_match[] = {
	{ .compatible = "snps,designware-i2c", },
	{},
};
MODULE_DEVICE_TABLE(of, dw_i2c_of_match);
#endif

#ifdef CONFIG_PM_SLEEP
static int dw_i2c_plat_prepare(struct device *dev)
{
	return pm_runtime_suspended(dev);
}

static void dw_i2c_plat_complete(struct device *dev)
{
	if (dev->power.direct_complete)
		pm_request_resume(dev);
}
#else
#define dw_i2c_plat_prepare	NULL
#define dw_i2c_plat_complete	NULL
#endif

#ifdef CONFIG_PM
static int dw_i2c_plat_suspend(struct device *dev)
{
	struct dw_i2c_dev *i_dev = dev_get_drvdata(dev);

	if (i_dev->suspended) {
		i_dev->skip_resume = true;
		return 0;
	}

	i_dev->disable(i_dev);
	i2c_dw_plat_prepare_clk(i_dev, false);

	i_dev->suspended = true;

	return 0;
}

static int dw_i2c_plat_resume(struct device *dev)
{
	struct dw_i2c_dev *i_dev = dev_get_drvdata(dev);

	if (!i_dev->suspended)
		return 0;

	if (i_dev->skip_resume) {
		i_dev->skip_resume = false;
		return 0;
	}

	i2c_dw_plat_prepare_clk(i_dev, true);
	i_dev->init(i_dev);

	i_dev->suspended = false;

	return 0;
}

static const struct dev_pm_ops dw_i2c_dev_pm_ops = {
	.prepare = dw_i2c_plat_prepare,
	.complete = dw_i2c_plat_complete,
	SET_LATE_SYSTEM_SLEEP_PM_OPS(dw_i2c_plat_suspend, dw_i2c_plat_resume)
	SET_RUNTIME_PM_OPS(dw_i2c_plat_suspend, dw_i2c_plat_resume, NULL)
};

#define DW_I2C_DEV_PMOPS (&dw_i2c_dev_pm_ops)
#else
#define DW_I2C_DEV_PMOPS NULL
#endif

/* Work with hotplug and coldplug */
MODULE_ALIAS("platform:i2c_designware");

static struct platform_driver dw_i2c_driver = {
	.probe = dw_i2c_plat_probe,
	.remove = dw_i2c_plat_remove,
	.driver		= {
		.name	= "i2c_designware",
		.of_match_table = of_match_ptr(dw_i2c_of_match),
		.acpi_match_table = ACPI_PTR(dw_i2c_acpi_match),
		.pm	= DW_I2C_DEV_PMOPS,
	},
};

static int __init dw_i2c_init_driver(void)
{
	return platform_driver_register(&dw_i2c_driver);
}
subsys_initcall(dw_i2c_init_driver);

static void __exit dw_i2c_exit_driver(void)
{
	platform_driver_unregister(&dw_i2c_driver);
}
module_exit(dw_i2c_exit_driver);

MODULE_AUTHOR("Baruch Siach <baruch@tkos.co.il>");
MODULE_DESCRIPTION("Synopsys DesignWare I2C bus adapter");
MODULE_LICENSE("GPL");
