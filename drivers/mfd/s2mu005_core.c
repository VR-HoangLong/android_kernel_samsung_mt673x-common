/*
 * s2mu005.c - mfd core driver for the s2mu005
 *
 * Copyright (C) 2015 Samsung Electronics
 *
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * This driver is based on max77843.c
 */

 #define pr_fmt(fmt)	"[s2mu005 mfd] " fmt

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/mfd/core.h>
#include <linux/mfd/samsung/s2mu005.h>
#include <linux/mfd/samsung/s2mu005-private.h>
#include <linux/regulator/machine.h>

#if IS_ENABLED(CONFIG_OF)
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#endif /* CONFIG_OF */

#if 0
#define I2C_ADDR_PMIC	(0x92 >> 1)	/* Top sys, Haptic */
#define I2C_ADDR_MUIC	(0x4A >> 1)
#define I2C_ADDR_CHG    (0xD2 >> 1)
#define I2C_ADDR_FG     (0x6C >> 1)
#endif

static struct mfd_cell s2mu005_devs[] = {
#if IS_ENABLED(CONFIG_MUIC_S2MU005)
	{ .name = "s2mu005-muic", },
#endif /* CONFIG_MUIC_S2MU005 */
#if IS_ENABLED(CONFIG_REGULATOR_S2MU005)
	{ .name = "s2mu005-safeout", },
#endif /* CONFIG_REGULATOR_S2MU005 */
#if IS_ENABLED(CONFIG_CHARGER_S2MU005)
	{ .name = "s2mu005-charger", },
#endif
#if IS_ENABLED(CONFIG_MOTOR_DRV_S2MU005)
	{ .name = "s2mu005-haptic", },
#endif /* CONFIG_S2MU005_HAPTIC */
#if IS_ENABLED(CONFIG_LEDS_S2MU005_RGB)
	{ .name = "leds-s2mu005-rgb", },
#endif /* CONFIG_LEDS_S2MU005_RGB */
#if IS_ENABLED(CONFIG_LEDS_S2MU005_FLASH)
	{ .name = "s2mu005-flash", },
#endif
};

int s2mu005_read_reg(struct i2c_client *i2c, u8 reg, u8 *dest)
{
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);
	int ret;

	mutex_lock(&s2mu005->i2c_lock);
	ret = i2c_smbus_read_byte_data(i2c, reg);
	mutex_unlock(&s2mu005->i2c_lock);
	if (ret < 0) {
		pr_info("%s reg(0x%x), ret(%d)\n", __func__, reg, ret);
		return ret;
	}

	*dest = ret & 0xff;
	return 0;
}
EXPORT_SYMBOL_GPL(s2mu005_read_reg);

int s2mu005_bulk_read(struct i2c_client *i2c, u8 reg, int count, u8 *buf)
{
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);
	int ret;

	mutex_lock(&s2mu005->i2c_lock);
	ret = i2c_smbus_read_i2c_block_data(i2c, reg, count, buf);
	mutex_unlock(&s2mu005->i2c_lock);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(s2mu005_bulk_read);

int s2mu005_read_word(struct i2c_client *i2c, u8 reg)
{
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);
	int ret;

	mutex_lock(&s2mu005->i2c_lock);
	ret = i2c_smbus_read_word_data(i2c, reg);
	mutex_unlock(&s2mu005->i2c_lock);
	if (ret < 0)
		return ret;

	return ret;
}
EXPORT_SYMBOL_GPL(s2mu005_read_word);

int s2mu005_write_reg(struct i2c_client *i2c, u8 reg, u8 value)
{
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);
	int ret;

	mutex_lock(&s2mu005->i2c_lock);
	ret = i2c_smbus_write_byte_data(i2c, reg, value);
	mutex_unlock(&s2mu005->i2c_lock);
	if (ret < 0)
		pr_info("%s reg(0x%x), ret(%d)\n",
				__func__, reg, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(s2mu005_write_reg);

int s2mu005_bulk_write(struct i2c_client *i2c, u8 reg, int count, u8 *buf)
{
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);
	int ret;

	mutex_lock(&s2mu005->i2c_lock);
	ret = i2c_smbus_write_i2c_block_data(i2c, reg, count, buf);
	mutex_unlock(&s2mu005->i2c_lock);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(s2mu005_bulk_write);

int s2mu005_write_word(struct i2c_client *i2c, u8 reg, u16 value)
{
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);
	int ret;

	mutex_lock(&s2mu005->i2c_lock);
	ret = i2c_smbus_write_word_data(i2c, reg, value);
	mutex_unlock(&s2mu005->i2c_lock);
	if (ret < 0)
		return ret;
	return 0;
}
EXPORT_SYMBOL_GPL(s2mu005_write_word);

int s2mu005_update_reg(struct i2c_client *i2c, u8 reg, u8 val, u8 mask)
{
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);
	int ret;

	mutex_lock(&s2mu005->i2c_lock);
	ret = i2c_smbus_read_byte_data(i2c, reg);
	if (ret >= 0) {
		u8 old_val = ret & 0xff;
		u8 new_val = (val & mask) | (old_val & (~mask));

		ret = i2c_smbus_write_byte_data(i2c, reg, new_val);
	}
	mutex_unlock(&s2mu005->i2c_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(s2mu005_update_reg);

#if IS_ENABLED(CONFIG_OF)
static int of_s2mu005_dt(struct device *dev, struct s2mu005_platform_data *pdata)
{
	struct device_node *np_s2mu005 = dev->of_node;

	if (!np_s2mu005)
		return -EINVAL;

	pdata->irq_gpio = of_get_named_gpio(np_s2mu005, "s2mu005,irq-gpio", 0);
	pdata->wakeup = of_property_read_bool(np_s2mu005, "s2mu005,wakeup");

	pr_info("%s: irq-gpio: %u\n", __func__, pdata->irq_gpio);

	return 0;
}
#else
static int of_s2mu005_dt(struct device *dev, struct max77834_platform_data *pdata)
{
	return 0;
}
#endif /* CONFIG_OF */

static int s2mu005_i2c_probe(struct i2c_client *i2c,
				const struct i2c_device_id *dev_id)
{
	struct s2mu005_dev *s2mu005;
	struct s2mu005_platform_data *pdata = i2c->dev.platform_data;

	int ret = 0;
	u8 temp = 0;

	pr_err("==================================================\n");
	pr_err("%s\n", __func__);

	s2mu005 = kzalloc(sizeof(struct s2mu005_dev), GFP_KERNEL);
	if (!s2mu005) {
		pr_err("%s Failed to alloc mem for s2mu005\n", __func__);
		return -ENOMEM;
	}

	if (i2c->dev.of_node) {
		pdata = devm_kzalloc(&i2c->dev, sizeof(struct s2mu005_platform_data),
				GFP_KERNEL);
		if (!pdata) {
			dev_err(&i2c->dev, "Failed to allocate memory\n");
			ret = -ENOMEM;
			goto err;
		}

		ret = of_s2mu005_dt(&i2c->dev, pdata);
		if (ret < 0) {
			dev_err(&i2c->dev, "Failed to get device of_node\n");
			goto err;
		}

		i2c->dev.platform_data = pdata;
	} else
		pdata = i2c->dev.platform_data;

	s2mu005->dev = &i2c->dev;
	s2mu005->i2c = i2c;
	s2mu005->irq = i2c->irq;
	if (pdata) {
		s2mu005->pdata = pdata;

		pdata->irq_base = irq_alloc_descs(-1, 0, S2MU005_IRQ_NR, -1);
		if (pdata->irq_base < 0) {
			pr_err("%s irq_alloc_descs Fail! ret(%d)\n",
					__func__, pdata->irq_base);
			ret = -EINVAL;
			goto err;
		} else
			s2mu005->irq_base = pdata->irq_base;

		s2mu005->irq_gpio = pdata->irq_gpio;
		s2mu005->wakeup = pdata->wakeup;
	} else {
		ret = -EINVAL;
		goto err;
	}
	mutex_init(&s2mu005->i2c_lock);

	i2c_set_clientdata(i2c, s2mu005);

	s2mu005_read_reg(s2mu005->i2c, S2MU005_REG_REV_ID, &temp);
	if (temp < 0)
		pr_err("[s2mu005 mfd] %s : i2c read error\n", __func__);

	s2mu005->pmic_ver = temp & 0x0F;
	pr_err("[s2mu005 mfd] %s : ver=0x%x\n", __func__, s2mu005->pmic_ver);

	ret = s2mu005_irq_init(s2mu005);

	if (ret < 0)
		goto err_irq_init;

	ret = mfd_add_devices(s2mu005->dev, -1, s2mu005_devs,
			ARRAY_SIZE(s2mu005_devs), NULL, 0, NULL);
	if (ret < 0)
		goto err_mfd;

	device_init_wakeup(s2mu005->dev, pdata->wakeup);

	pr_info("[s2mu005 mfd] %s : Done\n", __func__);
	return ret;

err_mfd:
	mutex_destroy(&s2mu005->i2c_lock);
	mfd_remove_devices(s2mu005->dev);
err_irq_init:
	i2c_unregister_device(s2mu005->i2c);
err:
	kfree(s2mu005);
	return ret;
}

static int s2mu005_i2c_remove(struct i2c_client *i2c)
{
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);

	mfd_remove_devices(s2mu005->dev);
	i2c_unregister_device(s2mu005->i2c);
	kfree(s2mu005);

	return 0;
}

static const struct i2c_device_id s2mu005_i2c_id[] = {
	{ MFD_DEV_NAME, TYPE_S2MU005 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, s2mu005_i2c_id);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id s2mu005_i2c_dt_ids[] = {
	{ .compatible = "samsung,s2mu005mfd" },
	{ },
};
#endif /* CONFIG_OF */

#if IS_ENABLED(CONFIG_PM)
static int s2mu005_suspend(struct device *dev)
{
	struct i2c_client *i2c = container_of(dev, struct i2c_client, dev);
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);

	if (device_may_wakeup(dev))
		enable_irq_wake(s2mu005->irq);

	disable_irq(s2mu005->irq);

	return 0;
}

static int s2mu005_resume(struct device *dev)
{
	struct i2c_client *i2c = container_of(dev, struct i2c_client, dev);
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);

#if !IS_ENABLED(CONFIG_SAMSUNG_PRODUCT_SHIP)
	pr_info("%s\n", __func__);
#endif /* CONFIG_SAMSUNG_PRODUCT_SHIP */

	if (device_may_wakeup(dev))
		disable_irq_wake(s2mu005->irq);

	enable_irq(s2mu005->irq);

	return 0;
}
#else
#define s2mu005_suspend	NULL
#define s2mu005_resume	NULL
#endif /* CONFIG_PM */

#if IS_ENABLED(CONFIG_HIBERNATION)

#if 0
u8 s2mu005_dumpaddr_pmic[] = {
	s2mu005_LED_REG_IFLASH,
	s2mu005_LED_REG_IFLASH1,
	s2mu005_LED_REG_IFLASH2,
	s2mu005_LED_REG_ITORCH,
	s2mu005_LED_REG_ITORCHTORCHTIMER,
	s2mu005_LED_REG_FLASH_TIMER,
	s2mu005_LED_REG_FLASH_EN,
	s2mu005_LED_REG_MAX_FLASH1,
	s2mu005_LED_REG_MAX_FLASH2,
	s2mu005_LED_REG_VOUT_CNTL,
	s2mu005_LED_REG_VOUT_FLASH,
	s2mu005_LED_REG_VOUT_FLASH1,
	s2mu005_LED_REG_FLASH_INT_STATUS,
	s2mu005_PMIC_REG_PMICID1,
	s2mu005_PMIC_REG_PMICREV,
	s2mu005_PMIC_REG_MAINCTRL1,
	s2mu005_PMIC_REG_MCONFIG,
};
#endif

u8 s2mu005_dumpaddr_muic[] = {
	s2mu005_MUIC_REG_INTMASK1,
	s2mu005_MUIC_REG_INTMASK2,
	s2mu005_MUIC_REG_INTMASK3,
	s2mu005_MUIC_REG_CDETCTRL1,
	s2mu005_MUIC_REG_CDETCTRL2,
	s2mu005_MUIC_REG_CTRL1,
	s2mu005_MUIC_REG_CTRL2,
	s2mu005_MUIC_REG_CTRL3,
};

#if 0
u8 s2mu005_dumpaddr_haptic[] = {
	s2mu005_HAPTIC_REG_CONFIG1,
	s2mu005_HAPTIC_REG_CONFIG2,
	s2mu005_HAPTIC_REG_CONFIG_CHNL,
	s2mu005_HAPTIC_REG_CONFG_CYC1,
	s2mu005_HAPTIC_REG_CONFG_CYC2,
	s2mu005_HAPTIC_REG_CONFIG_PER1,
	s2mu005_HAPTIC_REG_CONFIG_PER2,
	s2mu005_HAPTIC_REG_CONFIG_PER3,
	s2mu005_HAPTIC_REG_CONFIG_PER4,
	s2mu005_HAPTIC_REG_CONFIG_DUTY1,
	s2mu005_HAPTIC_REG_CONFIG_DUTY2,
	s2mu005_HAPTIC_REG_CONFIG_PWM1,
	s2mu005_HAPTIC_REG_CONFIG_PWM2,
	s2mu005_HAPTIC_REG_CONFIG_PWM3,
	s2mu005_HAPTIC_REG_CONFIG_PWM4,
};
#endif

u8 s2mu005_dumpaddr_led[] = {
	s2mu005_RGBLED_REG_LEDEN,
	s2mu005_RGBLED_REG_LED0BRT,
	s2mu005_RGBLED_REG_LED1BRT,
	s2mu005_RGBLED_REG_LED2BRT,
	s2mu005_RGBLED_REG_LED3BRT,
	s2mu005_RGBLED_REG_LEDBLNK,
	s2mu005_RGBLED_REG_LEDRMP,
};

static int s2mu005_freeze(struct device *dev)
{
	struct i2c_client *i2c = container_of(dev, struct i2c_client, dev);
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);
	int i;

	for (i = 0; i < ARRAY_SIZE(s2mu005_dumpaddr_pmic); i++)
		s2mu005_read_reg(i2c, s2mu005_dumpaddr_pmic[i],
				&s2mu005->reg_pmic_dump[i]);

	for (i = 0; i < ARRAY_SIZE(s2mu005_dumpaddr_muic); i++)
		s2mu005_read_reg(i2c, s2mu005_dumpaddr_muic[i],
				&s2mu005->reg_muic_dump[i]);

	for (i = 0; i < ARRAY_SIZE(s2mu005_dumpaddr_led); i++)
		s2mu005_read_reg(i2c, s2mu005_dumpaddr_led[i],
				&s2mu005->reg_led_dump[i]);

	disable_irq(s2mu005->irq);

	return 0;
}

static int s2mu005_restore(struct device *dev)
{
	struct i2c_client *i2c = container_of(dev, struct i2c_client, dev);
	struct s2mu005_dev *s2mu005 = i2c_get_clientdata(i2c);
	int i;

	enable_irq(s2mu005->irq);

	for (i = 0; i < ARRAY_SIZE(s2mu005_dumpaddr_pmic); i++)
		s2mu005_write_reg(i2c, s2mu005_dumpaddr_pmic[i],
				s2mu005->reg_pmic_dump[i]);

	for (i = 0; i < ARRAY_SIZE(s2mu005_dumpaddr_muic); i++)
		s2mu005_write_reg(i2c, s2mu005_dumpaddr_muic[i],
				s2mu005->reg_muic_dump[i]);

	for (i = 0; i < ARRAY_SIZE(s2mu005_dumpaddr_led); i++)
		s2mu005_write_reg(i2c, s2mu005_dumpaddr_led[i],
				s2mu005->reg_led_dump[i]);

	return 0;
}
#endif

const struct dev_pm_ops s2mu005_pm = {
	.suspend = s2mu005_suspend,
	.resume = s2mu005_resume,
#if IS_ENABLED(CONFIG_HIBERNATION)
	.freeze =  s2mu005_freeze,
	.thaw = s2mu005_restore,
	.restore = s2mu005_restore,
#endif
};

static struct i2c_driver s2mu005_i2c_driver = {
	.driver		= {
		.name	= MFD_DEV_NAME,
		.owner	= THIS_MODULE,
#if IS_ENABLED(CONFIG_PM)
		.pm	= &s2mu005_pm,
#endif /* CONFIG_PM */
#if IS_ENABLED(CONFIG_OF)
		.of_match_table	= s2mu005_i2c_dt_ids,
#endif /* CONFIG_OF */
	},
	.probe		= s2mu005_i2c_probe,
	.remove		= s2mu005_i2c_remove,
	.id_table	= s2mu005_i2c_id,
};

static int __init s2mu005_i2c_init(void)
{
	pr_info("%s\n", __func__);
	return i2c_add_driver(&s2mu005_i2c_driver);
}
/* init early so consumer devices can complete system boot */
subsys_initcall(s2mu005_i2c_init);

static void __exit s2mu005_i2c_exit(void)
{
	i2c_del_driver(&s2mu005_i2c_driver);
}
module_exit(s2mu005_i2c_exit);

MODULE_DESCRIPTION("s2mu005 multi-function core driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
