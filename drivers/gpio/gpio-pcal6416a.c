/*
 *  pcal6416a.c - 16 bit I/O port
 *
 *  Copyright (C) 2005 Ben Gardner <bgardner@wabtec.com>
 *  Copyright (C) 2007 Marvell International Ltd.
 *  Copyright (C) 2013 NXP Semiconductors
 *
 *  Derived from drivers/i2c/chips/pca953x.c
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#ifdef USE_SEC_CLASS
#include <linux/sec_class.h>
#endif
#if defined(CONFIG_OF)
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#endif
#include <linux/i2c/pcal6416a.h>

struct device *pcal6416a_dev;
struct pcal6416a_chip *g_dev[NUM_PCAL6416A_CHIP] = { NULL, };
static int pcal6416a_probe_cnt;

/* read the 16-bit register from the PCAL6416A
*	 reg: register address
*   val: the value read back from the PCAL6416A
*/
static int pcal6416a_read_reg(struct pcal6416a_chip *chip, int reg, uint16_t *val)
{
	int ret = i2c_smbus_read_word_data(chip->client, reg);

	if (ret < 0) {
		dev_err(&chip->client->dev,
				"failed reading register %d\n", ret);
		return ret;
	}

	*val = (uint16_t)ret;
	return 0;
}

/* write a 16-bit value to the PCAL6416A
*	 reg: register address
*   val: the value read back from the PCAL6416A
*/
static int pcal6416a_write_reg(struct pcal6416a_chip *chip, int reg, uint16_t val)
{
	int ret = i2c_smbus_write_word_data(chip->client, reg, val);

	if (ret < 0) {
		dev_err(&chip->client->dev,
				"failed writing register %d\n", ret);
		return ret;
	}

	return 0;
}

/* read a port pin value (INPUT register) from the PCAL6416A
*   off: bit number (0..15)
*   return: bit value 0 or 1
*/
static int pcal6416a_gpio_get_value(struct gpio_chip *gc, unsigned off)
{
	uint16_t reg_val;
	int ret;
	struct pcal6416a_chip *chip
		= container_of(gc, struct pcal6416a_chip, gpio_chip);

	ret = pcal6416a_read_reg(chip, PCAL6416A_INPUT, &reg_val);
	if (ret < 0) {
		/* NOTE:  diagnostic already emitted; that's all we should
		 * do unless gpio_*_value_cansleep() calls become different
		 * from their nonsleeping siblings (and report faults).
		 */
		return ret;
	}

	return (reg_val & (1u << off)) ? 1 : 0;
}

/* write a port pin value (INPUT register) from the PCAL6416A
*   off: bit number (0..15)
*   val: 0 or 1
*   return: none
*/
static void pcal6416a_gpio_set_value(struct gpio_chip *gc,
					unsigned off, int val)
{
	struct pcal6416a_chip *chip
		= container_of(gc, struct pcal6416a_chip, gpio_chip);

	mutex_lock(&chip->lock);
	if (val)
		chip->reg_output |= (1u << off);
	else
		chip->reg_output &= ~(1u << off);

	pcal6416a_write_reg(chip, PCAL6416A_DAT_OUT, chip->reg_output);
	mutex_unlock(&chip->lock);
}

/* set the CONFIGURATION register of a port pin as an input
*   off: bit number (0..15)
*/
static int pcal6416a_gpio_direction_input(struct gpio_chip *gc, unsigned off)
{
	int ret;
	struct pcal6416a_chip *chip
		= container_of(gc, struct pcal6416a_chip, gpio_chip);

	pr_info("%s(off=[%d])\n", __func__, off);
	mutex_lock(&chip->lock);
	/* input set bit 1  */
	chip->reg_config |= (1u << off);
	ret = pcal6416a_write_reg(chip, PCAL6416A_CONFIG, chip->reg_config);
	mutex_unlock(&chip->lock);
	return ret;
}

/* set the DIRECTION (CONFIGURATION register) of a port pin as an output
*   off: bit number (0..15)
*   val = 1 or 0
*   return: 0 if successful
*/
static int pcal6416a_gpio_direction_output(struct gpio_chip *gc,
					unsigned off, int val)
{
	int ret;
	struct pcal6416a_chip *chip
		= container_of(gc, struct pcal6416a_chip, gpio_chip);

	pr_info("%s(off=[%d], val=[%d])\n", __func__, off, val);
	mutex_lock(&chip->lock);
	/* set output level */
	if (val)
		chip->reg_output |= (1u << off);
	else
		chip->reg_output &= ~(1u << off);
	ret = pcal6416a_write_reg(chip, PCAL6416A_DAT_OUT, chip->reg_output);

	/* then direction */
	/* output set bit 0  */
	chip->reg_config &= ~(1u << off);
	ret |= pcal6416a_write_reg(chip, PCAL6416A_CONFIG, chip->reg_config);
	mutex_unlock(&chip->lock);

	return ret;
}

static int pcal6416a_gpio_request(struct gpio_chip *gc, unsigned off)
{
	struct pcal6416a_chip *chip
		= container_of(gc, struct pcal6416a_chip, gpio_chip);

	pr_info("%s\n", __func__);
	if (off >= chip->gpio_chip.ngpio) {
		pr_err("[%s] offset over max = [%d]\n", __func__, off);
		return 1;
	}
	/* to do*/
	return 0;
}

static void pcal6416a_gpio_free(struct gpio_chip *gc, unsigned off)
{
	struct pcal6416a_chip *chip
		= container_of(gc, struct pcal6416a_chip, gpio_chip);

	pr_info("%s\n", __func__);
	if (off >= chip->gpio_chip.ngpio)
		pr_err("[%s] offset over max = [%d]\n", __func__, off);

	/* to do*/
}

static int pcal6416a_gpio_setup(struct pcal6416a_chip *dev)
{
	int ret;
	uint16_t read_val;

	pr_info("[%s] GPIO Expander Init setting\n", __func__);

	dev->reg_outputconfig = 0x0000;
	ret = pcal6416a_write_reg(dev, PCAL6416A_OUTPUT_CONFIG,
			dev->reg_outputconfig);		/* push-pull */
	if (ret < 0) {
		pr_err("failed set output config\n");
		return ret;
	}

	ret = pcal6416a_write_reg(dev, PCAL6416A_DAT_OUT,
			dev->reg_output);		/* 1 : output high, 0 : output low */
	if (ret < 0) {
		pr_err("failed set data out\n");
		return ret;
	}
	ret = pcal6416a_write_reg(dev, PCAL6416A_CONFIG,
			dev->reg_config);		/* 1 : input, 0 : output */
	if (ret < 0) {
		pr_err("failed set config\n");
		return ret;
	}

	dev->reg_polarity = 0x0000;
	ret = pcal6416a_write_reg(dev, PCAL6416A_POLARITY,
			dev->reg_polarity);
	if (ret < 0) {
		pr_err("failed set polarity\n");
		return ret;
	}

	dev->reg_drive0 = 0x0000;
	ret = pcal6416a_write_reg(dev, PCAL6416A_DRIVE0,
			dev->reg_drive0);		/* drive 0.25x */
	if (ret < 0) {
		pr_err("failed set drive0\n");
		return ret;
	}
	dev->reg_drive1 = 0x0000;
	ret = pcal6416a_write_reg(dev, PCAL6416A_DRIVE1,
			dev->reg_drive1);		/* drive 0.25x */
	if (ret < 0) {
		pr_err("failed set drive1\n");
		return ret;
	}

	dev->reg_inputlatch = 0x0000;
	ret = pcal6416a_write_reg(dev, PCAL6416A_INPUT_LATCH,
			dev->reg_inputlatch);		/* not use latch */
	if (ret < 0) {
		pr_err("failed set input latch\n");
		return ret;
	}
	ret = pcal6416a_write_reg(dev, PCAL6416A_EN_PULLUPDOWN,
			dev->reg_enpullupdown);		/* 1 : enable, 0 : disable */
	if (ret < 0) {
		pr_err("failed set enable pullupdown\n");
		return ret;
	}
	ret = pcal6416a_write_reg(dev, PCAL6416A_SEL_PULLUPDOWN,
			dev->reg_selpullupdown);	/* 1 : pull-up, 0 : pull-down */
	if (ret < 0) {
		pr_err("failed set select pull\n");
		return ret;
	}
	dev->reg_intmask = 0xFFFF;
	ret = pcal6416a_write_reg(dev, PCAL6416A_INT_MASK,
			dev->reg_intmask);		/* not use int */
	if (ret < 0) {
		pr_err("failed set int mask\n");
		return ret;
	}
	ret = pcal6416a_read_reg(dev, PCAL6416A_INT_MASK,
			&read_val);
	if (ret < 0) {
		pr_err("failed read int mask\n");
		return ret;
	}

	return 0;
}

#ifdef CONFIG_OF
static int pcal6416a_parse_dt(struct device *dev,
		struct pcal6416a_platform_data *pdata)
{
	struct device_node *np = dev->of_node;
	/*struct pinctrl *reset_pinctrl; */
	int ret, i;
	u32 pull_reg;

	ret = of_property_read_u32(np, "pcal6416a,gpio_start", &pdata->gpio_start);
	if (ret < 0) {
		pr_err("[%s]: Unable to read pcal6416a,gpio_start\n", __func__);
		return ret;
	}

	ret = of_property_read_u32(np, "pcal6416a,ngpio", &pdata->ngpio);
	if (ret < 0) {
		pr_err("[%s]: Unable to read pcal6416a,ngpio\n", __func__);
		return ret;
	}
	pdata->reset_gpio = of_get_named_gpio(np, "pcal6416a,reset-gpio", 0);

#if 0
	/* Get pinctrl if target uses pinctrl */
	reset_pinctrl = devm_pinctrl_get_select(dev, "expander_reset_setting");
	if (IS_ERR(reset_pinctrl)) {
		if (PTR_ERR(reset_pinctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		pr_debug("Target does not use pinctrl\n");
		reset_pinctrl = NULL;
	}
#endif

	ret = of_property_read_u32(np, "pcal6416a,support_initialize", (u32 *)&pdata->support_init);
	if (ret < 0) {
		pr_err("[%s]: Unable to read pcal6416a,support_init\n", __func__);
		pdata->support_init = 0;
	}

#if 0
	pdata->vdd = devm_regulator_get(dev, "pcal6416a,vdd");
	if (IS_ERR(pdata->vdd)) {
		pr_err("%s: cannot get pcal6416a,vdd\n", __func__);
		ret = -ENOMEM;
		return ret;
	} else if (!regulator_get_voltage(pdata->vdd)) {
		ret = regulator_set_voltage(pdata->vdd, 1800000, 1800000);
	}
#endif

	if (pdata->support_init) {
		ret = of_property_read_u32(np, "pcal6416a,config", (u32 *)&pdata->init_config);
		if (ret < 0) {
			pr_err("[%s]: Unable to read pcal6416a,support_init\n", __func__);
			pdata->init_config = 0x0000;
		}
		ret = of_property_read_u32(np, "pcal6416a,data_out", (u32 *)&pdata->init_data_out);
		if (ret < 0) {
			pr_err("[%s]: Unable to read pcal6416a,support_init\n", __func__);
			pdata->init_data_out = 0x0000;
		}
		ret = of_property_read_u32(np, "pcal6416a,pull_reg", &pull_reg);
		if (ret < 0) {
			pr_err("[%s]: Unable to read pcal6416a,pull_reg\n", __func__);
			pdata->init_en_pull = 0x0000;
			pdata->init_sel_pull = 0x0000;
		}
		pr_info("[%s] 0x%08x\n", __func__, pull_reg);
		pdata->init_en_pull = 0x0000;
		pdata->init_sel_pull = 0x0000;
		for (i = 0; i < 16; i++) {
			if (((pull_reg>>(i*2))&0x3) == NO_PULL) {
				pdata->init_en_pull &= ~(1<<i);
				pdata->init_sel_pull &= ~(1<<i);
			} else if (((pull_reg>>(i*2))&0x3) == PULL_DOWN) {
				pdata->init_en_pull |= (1<<i);
				pdata->init_sel_pull &= ~(1<<i);
			} else if (((pull_reg>>(i*2))&0x3) == PULL_UP) {
				pdata->init_en_pull |= (1<<i);
				pdata->init_sel_pull |= (1<<i);
			}
		}
	} else {
		pdata->init_config = 0x0000;
		pdata->init_data_out = 0x0400;
		pdata->init_en_pull = 0x0000;
		pdata->init_sel_pull = 0x0000;
	}

	pr_info("[%s] initialize reg 0x%04x 0x%04x 0x%04x 0x%04x\n", __func__,
			pdata->init_config, pdata->init_data_out,
			pdata->init_en_pull, pdata->init_sel_pull);
	dev->platform_data = pdata;
	pr_info("[%s]gpio_start=[%d]ngpio=[%d]reset-gpio=[%d]\n",
			__func__, pdata->gpio_start, pdata->ngpio,
			pdata->reset_gpio);
	return 0;
}
#endif

#if 0
static void pcal6416a_power_ctrl(struct pcal6416a_platform_data *pdata, char enable)
{
	int ret = 0;
	struct regulator *reg_power = pdata->vdd;

	if (enable) {
		if (regulator_is_enabled(reg_power))
			pr_err("%s: power regulator(1.8V) is enabled\n", __func__);
		else
			ret = regulator_enable(reg_power);
		if (ret) {
			pr_err("%s: power regulator enable failed, rc=%d\n",
					__func__, ret);
			return;
		}
		pr_info("%s: gpio expander 1.8V on is finished.\n", __func__);
	} else {
		if (regulator_is_enabled(reg_power))
			ret = regulator_disable(reg_power);
		else
			pr_err("%s: power regulator(1.8V) is disabled\n", __func__);
		if (ret) {
			pr_err("%s: disable power regulator failed, rc=%d\n",
					__func__, ret);
			return;
		}
		pr_info("%s: gpio expander 1.8V off is finished.\n", __func__);
	}
	pr_err("[pcal6416a gpio expander] %s enable(%d)\n", __func__, enable);

}
#endif

static int pcal6416a_reset_chip(struct pcal6416a_chip *dev)
{
	int retval;
	int reset_gpio = dev->pdata->reset_gpio;
#if 0
	/*struct pinctrl *reset_pinctrl; */

	reset_pinctrl = devm_pinctrl_get_select(&dev->client->dev,
				 "reset");
	if (IS_ERR(reset_pinctrl)) {
		if (PTR_ERR(reset_pinctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		pr_err("[%s] not use reset pinctrl\n", __func__);
		return -EIO;
	}

	usleep_range(300, 301);

	reset_pinctrl = devm_pinctrl_get_select(&dev->client->dev,
				 "default");
	if (IS_ERR(reset_pinctrl)) {
		if (PTR_ERR(reset_pinctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		pr_err("[%s] not use default pinctrl\n", __func__);
		return -EIO;
	}

	pr_info("[%s]: gpio expander reset.\n", __func__);
#endif

#if 1
	if (gpio_is_valid(reset_gpio)) {
		retval = gpio_request(reset_gpio,
				"pcal6416a_reset_gpio");
		if (retval) {
			pr_err("[%s]: unable to request gpio [%d]\n",
					__func__, reset_gpio);
			return retval;
		}

		retval = gpio_direction_output(reset_gpio, 1);
		if (retval) {
			pr_err("[%s]: unable to set direction for gpio [%d]\n",
					__func__, reset_gpio);
			gpio_free(reset_gpio);
			return retval;
		}

		usleep_range(100, 101);
		gpio_set_value(reset_gpio, 0);
		usleep_range(100, 101);
		gpio_set_value(reset_gpio, 1);
		pr_info("[%s]: gpio expander reset.\n", __func__);

		gpio_free(reset_gpio);
		return 0;
	}
	pr_err("[%s]: gpio_is_valid fail\n", __func__);
	return -EIO;

#endif
	return 0;
}

static ssize_t store_pcal6416a_gpio_inout(struct device *dev,
		struct device_attribute *devattr,
		const char *buf, size_t count)
{
	int retval, off, val, gpio_pcal6416a;
	char in_out, msg[13];

	retval = sscanf(buf, "%1c %3d %1d", &in_out, &off, &val);
	if (retval == 0) {
		pr_err("[%s] fail to pcal6416a out.\n", __func__);
		return count;
	}
	if (!(in_out == 'i' || in_out == 'o')) {
		pr_err("[%s] wrong in_out value [%c]\n", __func__,  in_out);
		return count;
	}
	if ((off < 0) || (off > 31)) {
		pr_err("[%s] wrong offset value [%d]\n", __func__, off);
		return count;
	}
	if (!(val == 0 || val == 1)) {
		pr_err("[%s] wrong val value [%d]\n", __func__, val);
		return count;
	}

	gpio_pcal6416a = 300 + off;
	snprintf(msg, sizeof(msg)/sizeof(char), "exp-gpio%d\n", off);
	if (gpio_is_valid(gpio_pcal6416a)) {
		retval = gpio_request(gpio_pcal6416a, msg);
		if (retval) {
			pr_err("[%s] unable to request gpio=[%d] err=[%d]\n",
					__func__, gpio_pcal6416a, retval);
			return count;
		}

		if (in_out == 'i') {
			retval = gpio_direction_input(gpio_pcal6416a);
			val = gpio_get_value(gpio_pcal6416a);
		} else
			retval = gpio_direction_output(gpio_pcal6416a, val);

		if (retval)
			pr_err("%s: unable to set direction for gpio [%d]\n",
						__func__, gpio_pcal6416a);

		gpio_free(gpio_pcal6416a);
	}

	pr_info("pcal6416a mode set to dir[%c], offset[%d], val[%d]\n", in_out, off, val);

	return count;
}

static ssize_t show_pcal6416a_gpio_state(struct device *dev,
		struct device_attribute *devattr, char *buf)
{
	struct pcal6416a_chip chip_state;
	int n, i, drv_str;
	uint16_t read_input;
	char *bufp = buf;

	for (n = 0; n < NUM_PCAL6416A_CHIP; n++) {
		pcal6416a_read_reg(g_dev[n], PCAL6416A_INPUT, &read_input);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_DAT_OUT, &chip_state.reg_output);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_CONFIG, &chip_state.reg_config);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_DRIVE0, &chip_state.reg_drive0);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_DRIVE1, &chip_state.reg_drive1);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_EN_PULLUPDOWN,
						&chip_state.reg_enpullupdown);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_SEL_PULLUPDOWN,
						&chip_state.reg_selpullupdown);

		for (i = 0; i < 16; i++) {
			bufp += sprintf(bufp, "Expander[3%02d]", i);
			if ((chip_state.reg_config>>i)&0x1)
				bufp += sprintf(bufp, " IN");
			else {
				if ((chip_state.reg_output>>i)&0x1)
					bufp += sprintf(bufp, " OUT_HIGH");
				else
					bufp += sprintf(bufp, " OUT_LOW");
			}
			if ((chip_state.reg_enpullupdown>>i)&0x1) {
				if ((chip_state.reg_selpullupdown>>i)&0x1)
					bufp += sprintf(bufp, " PULL_UP");
				else
					bufp += sprintf(bufp, " PULL_DOWN");
			} else
				bufp += sprintf(bufp, " PULL_NONE");
			if (i > 7)
				drv_str = (chip_state.reg_drive1>>((i-8)*2)) & 0x3;
			else
				drv_str = (chip_state.reg_drive0>>(i*2)) & 0x3;
			switch (drv_str) {
			case GPIO_CFG_6_25MA:
				bufp += sprintf(bufp, " DRV_6.25mA");
				break;
			case GPIO_CFG_12_5MA:
				bufp += sprintf(bufp, " DRV_12.5mA");
				break;
			case GPIO_CFG_18_75MA:
				bufp += sprintf(bufp, " DRV_18.75mA");
				break;
			case GPIO_CFG_25MA:
				bufp += sprintf(bufp, " DRV_25mA");
				break;
			}
			if ((read_input>>i)&0x1)
				bufp += sprintf(bufp, " VAL_HIGH\n");
			else
				bufp += sprintf(bufp, " VAL_LOW\n");
		}
	}

	return strlen(buf);
}

static DEVICE_ATTR(expgpio, 0644,
		show_pcal6416a_gpio_state, store_pcal6416a_gpio_inout);

#ifdef CONFIG_SEC_PM_DEBUG
int expander_print_all(void)
{
	struct pcal6416a_chip chip_state;
	int n, i, drv_str;
	uint16_t read_input;

	if (!g_dev[0])
		return -ENODEV;

	for (n = 0; n < NUM_PCAL6416A_CHIP; n++) {
		pcal6416a_read_reg(g_dev[n], PCAL6416A_INPUT, &read_input);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_DAT_OUT, &chip_state.reg_output);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_CONFIG, &chip_state.reg_config);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_DRIVE0, &chip_state.reg_drive0);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_DRIVE1, &chip_state.reg_drive1);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_EN_PULLUPDOWN,
				&chip_state.reg_enpullupdown);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_SEL_PULLUPDOWN,
				&chip_state.reg_selpullupdown);

		for (i = 0; i < 16; i++) {
			pr_cont("Expander[3%02d]", i + 16 * n);
			if ((chip_state.reg_config>>i)&0x1)
				pr_cont("\tIN");
			else {
				if ((chip_state.reg_output>>i)&0x1)
					pr_cont("\tOUT_HIGH");
				else
					pr_cont("\tOUT_LOW");
			}
			if ((chip_state.reg_enpullupdown>>i)&0x1) {
				if ((chip_state.reg_selpullupdown>>i)&0x1)
					pr_cont("\tPULL_UP");
				else
					pr_cont("\tPULL_DOWN");
			} else
				pr_cont("\tPULL_NONE");
			if (i > 7)
				drv_str = (chip_state.reg_drive1>>((i-8)*2)) & 0x3;
			else
				drv_str = (chip_state.reg_drive0>>(i*2)) & 0x3;
			switch (drv_str) {
			case GPIO_CFG_6_25MA:
				pr_cont("\tDRV_6.25mA");
				break;
			case GPIO_CFG_12_5MA:
				pr_cont("\tDRV_12.5mA");
				break;
			case GPIO_CFG_18_75MA:
				pr_cont("\tDRV_18.75mA");
				break;
			case GPIO_CFG_25MA:
				pr_cont("\tDRV_25mA");
				break;
			}
			if ((read_input>>i)&0x1)
				pr_cont("\tVAL_HIGH\n");
			else
				pr_cont("\tVAL_LOW\n");
		}
	}

	return 0;
}
#endif
static int expander_show(struct seq_file *s, void *unused)
{
	struct pcal6416a_chip chip_state;
	int n, i, drv_str;
	uint16_t read_input;

	for (n = 0; n < NUM_PCAL6416A_CHIP; n++) {
		pcal6416a_read_reg(g_dev[n], PCAL6416A_INPUT, &read_input);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_DAT_OUT, &chip_state.reg_output);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_CONFIG, &chip_state.reg_config);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_DRIVE0, &chip_state.reg_drive0);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_DRIVE1, &chip_state.reg_drive1);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_EN_PULLUPDOWN,
					&chip_state.reg_enpullupdown);
		pcal6416a_read_reg(g_dev[n], PCAL6416A_SEL_PULLUPDOWN,
					&chip_state.reg_selpullupdown);

		for (i = 0; i < 16; i++) {
			seq_printf(s, "Expander[3%02d]", i + 16 * n);
			if ((chip_state.reg_config>>i)&0x1)
				seq_puts(s, " IN");
			else {
				if ((chip_state.reg_output>>i)&0x1)
					seq_puts(s, " OUT_HIGH");
				else
					seq_puts(s, " OUT_LOW");
			}
			if ((chip_state.reg_enpullupdown>>i)&0x1) {
				if ((chip_state.reg_selpullupdown>>i)&0x1)
					seq_puts(s, " PULL_UP");
				else
					seq_puts(s, " PULL_DOWN");
			} else
				seq_puts(s, " PULL_NONE");
			if (i > 7)
				drv_str = (chip_state.reg_drive1>>((i-8)*2)) & 0x3;
			else
				drv_str = (chip_state.reg_drive0>>(i*2)) & 0x3;
			switch (drv_str) {
			case GPIO_CFG_6_25MA:
				seq_puts(s, " DRV_6.25mA");
				break;
			case GPIO_CFG_12_5MA:
				seq_puts(s, " DRV_12.5mA");
				break;
			case GPIO_CFG_18_75MA:
				seq_puts(s, " DRV_18.75mA");
				break;
			case GPIO_CFG_25MA:
				seq_puts(s, " DRV_25mA");
				break;
			}
			if ((read_input>>i)&0x1)
				seq_puts(s, " VAL_HIGH\n");
			else
				seq_puts(s, " VAL_LOW\n");
		}
	}

	return 0;
}
static int expander_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, expander_show, NULL);
}

static const struct file_operations expander_operations = {
	.open		= expander_debug_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int pcal6416a_gpio_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	struct device_node *np = client->dev.of_node;
	struct pcal6416a_platform_data *pdata = NULL;
	struct pcal6416a_chip *dev;
	struct gpio_chip *gc;
	struct dentry *debugfs_dir, *debugfs_file;
	int ret;
	int retry = 0;

	pr_info("[%s]\n", __func__);
	if (!i2c_check_functionality(client->adapter,
				I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev, "SMBUS Byte Data not Supported\n");
		return -EIO;
	}
#ifdef CONFIG_OF
	if (np) {
		pdata = devm_kzalloc(&client->dev,
				sizeof(struct pcal6416a_platform_data),
				GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}

		ret = pcal6416a_parse_dt(&client->dev, pdata);
		if (ret) {
			pr_err("[%s] pcal6416a parse dt failed\n", __func__);
			return ret;
		}

	} else {
		pdata = client->dev.platform_data;
		pr_info("GPIO Expender failed to align dtsi %s",
				__func__);
	}
#else
	pdata = client->dev.platform_data;
#endif
	if (pdata == NULL) {
		dev_err(&client->dev, "missing platform data\n");
		return -ENODEV;
	}

	/*pcal6416a_power_ctrl(pdata, POWER_ON); */

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (dev == NULL)
		return -ENOMEM;


	dev->client = client;
	dev->pdata = pdata;
	dev->gpio_start = pdata->gpio_start;

	gc = &dev->gpio_chip;
	gc->direction_input  = pcal6416a_gpio_direction_input;
	gc->direction_output = pcal6416a_gpio_direction_output;
	gc->get = pcal6416a_gpio_get_value;
	gc->set = pcal6416a_gpio_set_value;
	gc->request = pcal6416a_gpio_request;
	gc->free = pcal6416a_gpio_free;
	gc->can_sleep = 0;
	/*Require dev to use the of_gpio api*/
	gc->parent = &client->dev;

	/*gc->base = pdata->gpio_start;*/
	gc->base = -1;	/*auto assign base by system*/
	gc->ngpio = pdata->ngpio;
	gc->label = client->name;
	gc->owner = THIS_MODULE;

	mutex_init(&dev->lock);

	dev->reg_config = pdata->init_config;
	dev->reg_output = pdata->init_data_out;
	dev->reg_enpullupdown = pdata->init_en_pull;
	dev->reg_selpullupdown = pdata->init_sel_pull;

	g_dev[pcal6416a_probe_cnt] = dev;

	while (1) {
		if (!pcal6416a_probe_cnt)
			ret = pcal6416a_reset_chip(dev);
		else
			ret = 0;

		if (ret)
			dev_err(dev->gpio_chip.parent,
				 "[%s]reset failed\n", __func__);
		else {
			ret = pcal6416a_gpio_setup(dev);
			if (ret)
				dev_err(dev->gpio_chip.parent,
					"expander setup i2c retry [%d]\n", retry);
			else {
				dev_info(dev->gpio_chip.parent,
					"expander setup success [%d]\n", retry);
				break;
			}
		}
		if (retry++ > 5) {
			dev_err(dev->gpio_chip.parent,
				"Failed to setup expander retry[%d]\n", retry);
			WARN_ON(1);
			goto err;
		}
		usleep_range(100, 200);
	}

	ret = gpiochip_add(&dev->gpio_chip);
	if (ret)
		goto err;

	dev_info(&client->dev, "gpios %d..%d on a %s\n",
			gc->base, gc->base + gc->ngpio - 1,
			client->name);

	if (pcal6416a_probe_cnt == NUM_PCAL6416A_CHIP - 1) {
#ifdef USE_SEC_CLASS
		pcal6416a_dev = sec_device_create(NULL, "expander");
		if (IS_ERR(pcal6416a_dev)) {
			dev_err(&client->dev,
					"Failed to create device for expander\n");
			ret = -ENODEV;
			goto err;
		}
#else
		pcal6416a_dev = &client->dev;
#endif
		ret = sysfs_create_file(&pcal6416a_dev->kobj, &dev_attr_expgpio.attr);
		if (ret) {
			dev_err(&client->dev,
					"Failed to create sysfs group for expander\n");
			goto err_destroy;
		}

		debugfs_dir = debugfs_create_dir("expander", NULL);
		if (IS_ERR_OR_NULL(debugfs_dir)) {
			dev_err(&client->dev,
					"Failed to create debugfs dir for expander\n");
			goto err_debug_dir;
		}

		debugfs_file = debugfs_create_file("gpio", S_IFREG | S_IRUGO,
					debugfs_dir, NULL, &expander_operations);
		if (IS_ERR_OR_NULL(debugfs_file)) {
			dev_err(&client->dev,
					"Failed to create debugfs file for gpio\n");
			goto err_debug_file;
		}
	}

	i2c_set_clientdata(client, dev);
	/*g_dev = dev; */
	if (pcal6416a_probe_cnt < NUM_PCAL6416A_CHIP - 1)
		pcal6416a_probe_cnt++;

	return 0;

err_debug_file:
	debugfs_remove_recursive(debugfs_dir);
err_debug_dir:
	sysfs_remove_file(&pcal6416a_dev->kobj, &dev_attr_expgpio.attr);
err_destroy:
#ifdef USE_SEC_CLASS
	sec_device_destroy(pcal6416a_dev->devt);
#endif
err:
	/*pcal6416a_power_ctrl(pdata, POWER_OFF); */
	mutex_destroy(&dev->lock);
	kfree(dev);
	return ret;
}

static int pcal6416a_gpio_remove(struct i2c_client *client)
{
	struct pcal6416a_chip *dev = i2c_get_clientdata(client);

	/*pcal6416a_power_ctrl(dev->pdata, POWER_OFF); */
	gpiochip_remove(&dev->gpio_chip);
	if (!IS_ERR_OR_NULL(dev->dentry))
		debugfs_remove_recursive(dev->dentry);
	sysfs_remove_file(&pcal6416a_dev->kobj, &dev_attr_expgpio.attr);
	mutex_destroy(&dev->lock);
	kfree(dev);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id pcal6416a_dt_ids[] = {
	{ .compatible = "pcal6416a,gpio-expander",},
};
#endif
static const struct i2c_device_id pcal6416a_gpio_id[] = {
	{DRV_NAME, 0, },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pcal6416a_gpio_id);

static struct i2c_driver pcal6416a_gpio_driver = {
	.driver = {
		   .name = DRV_NAME,
#ifdef CONFIG_OF
		   .of_match_table = of_match_ptr(pcal6416a_dt_ids),
#endif
		   },
	.probe = pcal6416a_gpio_probe,
	.remove = pcal6416a_gpio_remove,
	.id_table = pcal6416a_gpio_id,
};

static int __init pcal6416a_gpio_init(void)
{
	return i2c_add_driver(&pcal6416a_gpio_driver);
}

subsys_initcall(pcal6416a_gpio_init);

static void __exit pcal6416a_gpio_exit(void)
{
	i2c_del_driver(&pcal6416a_gpio_driver);
}

module_exit(pcal6416a_gpio_exit);

MODULE_DESCRIPTION("GPIO expander driver for PCAL6416A");
MODULE_LICENSE("GPL");
