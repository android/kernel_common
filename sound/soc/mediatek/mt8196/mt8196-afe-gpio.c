// SPDX-License-Identifier: GPL-2.0
/*
 *  mt8196-afe-gpio.c  --  Mediatek 8196 afe gpio ctrl
 *
 *  Copyright (c) 2024 MediaTek Inc.
 *  Author: Darren Ye <darren.ye@mediatek.com>
 */

#include <linux/gpio.h>
#include <linux/pinctrl/consumer.h>

#include "mt8196-afe-common.h"
#include "mt8196-afe-gpio.h"

struct pinctrl *aud_pinctrl;
struct audio_gpio_attr {
	const char *name;
	bool gpio_prepare;
	struct pinctrl_state *gpioctrl;
};

static struct audio_gpio_attr aud_gpios[MT8196_AFE_GPIO_GPIO_NUM] = {
	[MT8196_AFE_GPIO_DAT_MISO0_OFF] = {"aud-dat-miso0-off", false, NULL},
	[MT8196_AFE_GPIO_DAT_MISO0_ON] = {"aud-dat-miso0-on", false, NULL},
	[MT8196_AFE_GPIO_DAT_MISO1_OFF] = {"aud-dat-miso1-off", false, NULL},
	[MT8196_AFE_GPIO_DAT_MISO1_ON] = {"aud-dat-miso1-on", false, NULL},
	[MT8196_AFE_GPIO_DAT_MOSI_OFF] = {"aud-dat-mosi-off", false, NULL},
	[MT8196_AFE_GPIO_DAT_MOSI_ON] = {"aud-dat-mosi-on", false, NULL},
	[MT8196_AFE_GPIO_I2SOUT0_OFF] = {"aud-gpio-i2sout0-off", false, NULL},
	[MT8196_AFE_GPIO_I2SOUT0_ON] = {"aud-gpio-i2sout0-on", false, NULL},
	[MT8196_AFE_GPIO_I2SIN0_OFF] = {"aud-gpio-i2sin0-off", false, NULL},
	[MT8196_AFE_GPIO_I2SIN0_ON] = {"aud-gpio-i2sin0-on", false, NULL},
	[MT8196_AFE_GPIO_I2SOUT1_OFF] = {"aud-gpio-i2sout1-off", false, NULL},
	[MT8196_AFE_GPIO_I2SOUT1_ON] = {"aud-gpio-i2sout1-on", false, NULL},
	[MT8196_AFE_GPIO_I2SIN1_OFF] = {"aud-gpio-i2sin1-off", false, NULL},
	[MT8196_AFE_GPIO_I2SIN1_ON] = {"aud-gpio-i2sin1-on", false, NULL},
	[MT8196_AFE_GPIO_I2SOUT4_OFF] = {"aud-gpio-i2sout4-off", false, NULL},
	[MT8196_AFE_GPIO_I2SOUT4_ON] = {"aud-gpio-i2sout4-on", false, NULL},
	[MT8196_AFE_GPIO_I2SIN4_OFF] = {"aud-gpio-i2sin4-off", false, NULL},
	[MT8196_AFE_GPIO_I2SIN4_ON] = {"aud-gpio-i2sin4-on", false, NULL},
	[MT8196_AFE_GPIO_I2SOUT6_OFF] = {"aud-gpio-i2sout6-off", false, NULL},
	[MT8196_AFE_GPIO_I2SOUT6_ON] = {"aud-gpio-i2sout6-on", false, NULL},
	[MT8196_AFE_GPIO_I2SIN6_OFF] = {"aud-gpio-i2sin6-off", false, NULL},
	[MT8196_AFE_GPIO_I2SIN6_ON] = {"aud-gpio-i2sin6-on", false, NULL},
	[MT8196_AFE_GPIO_AP_DMIC_OFF] = {"aud-gpio-ap-dmic-off", false, NULL},
	[MT8196_AFE_GPIO_AP_DMIC_ON] = {"aud-gpio-ap-dmic-on", false, NULL},
	[MT8196_AFE_GPIO_AP_DMIC1_OFF] = {"aud-gpio-ap-dmic1-off", false, NULL},
	[MT8196_AFE_GPIO_AP_DMIC1_ON] = {"aud-gpio-ap-dmic1-on", false, NULL},
	[MT8196_AFE_GPIO_DAT_MOSI_CH34_OFF] = {"aud-dat-mosi-ch34-off", false, NULL},
	[MT8196_AFE_GPIO_DAT_MOSI_CH34_ON] = {"aud-dat-mosi-ch34-on", false, NULL},
	[MT8196_AFE_GPIO_DAT_MISO_ONLY_OFF] = {"aud-dat-miso-only-off", false, NULL},
	[MT8196_AFE_GPIO_DAT_MISO_ONLY_ON] = {"aud-dat-miso-only-on", false, NULL},
	[MT8196_AFE_GPIO_I2SOUT3_OFF] = {"aud-gpio-i2sout3-off", false, NULL},
	[MT8196_AFE_GPIO_I2SOUT3_ON] = {"aud-gpio-i2sout3-on", false, NULL},
	[MT8196_AFE_GPIO_I2SIN3_OFF] = {"aud-gpio-i2sin3-off", false, NULL},
	[MT8196_AFE_GPIO_I2SIN3_ON] = {"aud-gpio-i2sin3-on", false, NULL},
};

static DEFINE_MUTEX(gpio_request_mutex);

int mt8196_afe_gpio_init(struct mtk_base_afe *afe)
{
	int ret;
	int i = 0;

	dev_dbg(afe->dev, "%s()\n", __func__);

	aud_pinctrl = devm_pinctrl_get(afe->dev);
	if (IS_ERR(aud_pinctrl)) {
		ret = PTR_ERR(aud_pinctrl);
		dev_info(afe->dev, "%s(), ret %d, cannot get aud_pinctrl!\n",
			__func__, ret);
		return -ENODEV;
	}

	for (i = 0; i < MT8196_AFE_GPIO_GPIO_NUM; i++) {
		if (aud_gpios[i].name == NULL) {
			dev_info(afe->dev, "%s(), gpio id %d not define!!!\n",
			 __func__, i);
		}
	}

	for (i = 0; i < ARRAY_SIZE(aud_gpios); i++) {
		aud_gpios[i].gpioctrl = pinctrl_lookup_state(aud_pinctrl,
					aud_gpios[i].name);
		if (IS_ERR(aud_gpios[i].gpioctrl)) {
			ret = PTR_ERR(aud_gpios[i].gpioctrl);
			dev_info(afe->dev, "%s(), pinctrl_lookup_state %s fail, ret %d\n",
				__func__, aud_gpios[i].name, ret);
		} else
			aud_gpios[i].gpio_prepare = true;
	}

	/* gpio status init */
	mt8196_afe_gpio_request(afe, false, MT8196_DAI_ADDA, 0);
	mt8196_afe_gpio_request(afe, false, MT8196_DAI_ADDA, 1);

	dev_dbg(afe->dev, "%s()\n", __func__);

	return 0;
}

static int mt8196_afe_gpio_select(struct mtk_base_afe *afe,
				  enum mt8196_afe_gpio type)
{
	int ret = 0;

	dev_info(afe->dev, "%s(), type: %d.\n", __func__, type);

	if (type >= MT8196_AFE_GPIO_GPIO_NUM) {
		dev_info(afe->dev, "%s(), error, invalid gpio type %d\n",
			__func__, type);
		return -EINVAL;
	}

	if (!aud_gpios[type].gpio_prepare) {
		dev_info(afe->dev, "%s(), error, gpio type %d not prepared\n",
			 __func__, type);
		return -EIO;
	}

	ret = pinctrl_select_state(aud_pinctrl,
				   aud_gpios[type].gpioctrl);
	if (ret)
		dev_info(afe->dev, "%s(), error, can not set gpio type %d\n",
			__func__, type);

	return ret;
}

int mt8196_afe_gpio_request(struct mtk_base_afe *afe, bool enable,
			    int dai, int uplink)
{
	dev_info(afe->dev, "%s(), enable: %d, dai: %d, uplink: %d.\n", __func__,
		enable, dai, uplink);

	mutex_lock(&gpio_request_mutex);
	switch (dai) {
	case MT8196_DAI_ADDA:
		break;
	case MT8196_DAI_ADDA_CH34:
		break;
	case MT8196_DAI_ADDA_CH56:
		break;
	case MT8196_DAI_I2S_IN0:
	case MT8196_DAI_I2S_OUT0:
		dev_info(afe->dev, "%s(), set enable: %d, DAI_I2S_IN0, DAI_I2S_OUT0.\n",
			 __func__, enable);
		if (enable) {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SIN0_ON);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SOUT0_ON);
		} else {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SIN0_OFF);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SOUT0_OFF);
		}
		break;
	case MT8196_DAI_I2S_IN1:
	case MT8196_DAI_I2S_OUT1:
		dev_info(afe->dev, "%s(), set enable: %d, DAI_I2S_IN1,DAI_I2S_OUT1.\n",
			 __func__, enable);
		if (enable) {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SIN1_ON);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SOUT1_ON);
		} else {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SIN1_OFF);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SOUT1_OFF);
		}
		break;
	case MT8196_DAI_I2S_IN3:
	case MT8196_DAI_I2S_OUT3:
		dev_info(afe->dev, "%s(), set enable: %d, DAI_I2S_IN3,DAI_I2S_OUT3.\n",
			 __func__, enable);
		if (enable) {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SIN3_ON);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SOUT3_ON);
		} else {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SIN3_OFF);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SOUT3_OFF);
		}
		break;
	case MT8196_DAI_I2S_IN4:
	case MT8196_DAI_I2S_OUT4:
		dev_info(afe->dev, "%s(), set enable: %d, DAI_I2S_IN4, DAI_I2S_OUT4.\n",
			 __func__, enable);
		if (enable) {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SIN4_ON);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SOUT4_ON);
		} else {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SIN4_OFF);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SOUT4_OFF);
		}
		break;
	case MT8196_DAI_I2S_IN6:
	case MT8196_DAI_I2S_OUT6:
		dev_info(afe->dev, "%s(), set enable: %d, DAI_I2S_IN6, DAI_I2S_OUT6.\n",
			 __func__, enable);
		if (enable) {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SIN6_ON);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SOUT6_ON);
		} else {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SIN6_OFF);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_I2SOUT6_OFF);
		}
		break;
	case MT8196_DAI_VOW:
		break;
	case MT8196_DAI_VOW_SCP_DMIC:
		break;
	case MT8196_DAI_MTKAIF:
		if (enable) {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_DAT_MISO1_ON);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_DAT_MISO0_ON);
		} else {
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_DAT_MISO1_OFF);
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_DAT_MISO0_OFF);
		}
		break;
	case MT8196_DAI_MISO_ONLY:
		if (enable)
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_DAT_MISO_ONLY_ON);
		else
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_DAT_MISO_ONLY_OFF);
		break;
	case MT8196_DAI_AP_DMIC:
		dev_info(afe->dev, "%s(), DMIC0 set gpio enable: %d, MT8196_DAI_AP_DMIC.\n",
			 __func__, enable);
		if (enable)
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_AP_DMIC_ON);
		else
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_AP_DMIC_OFF);
		break;
	case MT8196_DAI_AP_DMIC_CH34:
		dev_info(afe->dev, "%s(), DMIC1 set gpio enable: %d, MT8196_DAI_AP_DMIC_CH34.\n",
			 __func__, enable);
		if (enable)
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_AP_DMIC1_ON);
		else
			mt8196_afe_gpio_select(afe, MT8196_AFE_GPIO_AP_DMIC1_OFF);
		break;
	default:
		mutex_unlock(&gpio_request_mutex);
		dev_info(afe->dev, "%s(), invalid dai %d\n", __func__, dai);
		return -EINVAL;
	}
	mutex_unlock(&gpio_request_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(mt8196_afe_gpio_request);

bool mt8196_afe_gpio_is_prepared(enum mt8196_afe_gpio type)
{
	return aud_gpios[type].gpio_prepare;
}
EXPORT_SYMBOL(mt8196_afe_gpio_is_prepared);

