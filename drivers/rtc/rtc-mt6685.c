// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 MediaTek Inc.
 * Author: Amber Lin <Mw.lin@mediatek.com>
 */

#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/mfd/mt6685/rtc.h>
#include <linux/mfd/mt6685/core.h>
#include <linux/mfd/mt6685/registers.h>
#include <linux/sched/clock.h>
#include <linux/spmi.h>
#include <linux/jiffies.h>

void power_on_mclk(struct mt6685_rtc *rtc)
{
	guard(mutex)(&rtc->clk_lock);
	/* Power on RTC MCLK and 32k clk before write RTC register */
	regmap_write(rtc->regmap, RG_RTC_32K_CK_PDN_CLR, RG_RTC_32K_CK_PDN_MASK);
	regmap_write(rtc->regmap, RG_RTC_MCLK_PDN_CLR, RG_RTC_MCLK_PDN_MASK);

	rtc->clk_cnt++;

	mdelay(1);
}

static void power_down_mclk(struct mt6685_rtc *rtc)
{
	guard(mutex)(&rtc->clk_lock);
	rtc->clk_cnt--;

	if (rtc->clk_cnt == 0) {
		/* Power down RTC MCLK and 32k clk after write RTC register */
		regmap_write(rtc->regmap, RG_RTC_32K_CK_PDN_SET, RG_RTC_32K_CK_PDN_MASK);
		regmap_write(rtc->regmap, RG_RTC_MCLK_PDN_SET, RG_RTC_MCLK_PDN_MASK);
		mdelay(1);
	}
}

static inline int rtc_read(struct mt6685_rtc *rtc, unsigned int reg,
		    unsigned int *val)
{
	return regmap_bulk_read(rtc->regmap, reg, val, 2);
}

static inline int rtc_write(struct mt6685_rtc *rtc, unsigned int reg,
		     unsigned int val)
{
	return regmap_bulk_write(rtc->regmap, reg, &val, 2);
}

static int rtc_update_bits(struct mt6685_rtc *rtc, unsigned int reg,
			   unsigned int mask, unsigned int val)
{
	int ret;
	unsigned int tmp, orig = 0;

	ret = rtc_read(rtc, reg, &orig);
	if (ret != 0)
		return ret;

	tmp = orig & ~mask;
	tmp |= val & mask;
	ret = rtc_write(rtc, reg, tmp);

	return ret;
}

static int mtk_rtc_write_trigger(struct mt6685_rtc *rtc)
{
	unsigned long timeout = jiffies + HZ;
	int ret;
	u32 data;

	ret = rtc_write(rtc,
			rtc->addr_base + rtc->data->wrtgr, 1);
	if (ret < 0)
		return ret;

	while (1) {
		ret = rtc_read(rtc, rtc->addr_base + RTC_BBPU,
				  &data);
		if (ret < 0)
			break;
		if (!(data & RTC_BBPU_CBUSY))
			break;
		if (time_after(jiffies, timeout)) {
			ret = -ETIMEDOUT;
			break;
		}
		cpu_relax();
	}

	return ret;
}

static void mtk_rtc_reset_bbpu_alarm_status(struct mt6685_rtc *rtc)
{
	u32 bbpu;
	int ret;

	power_on_mclk(rtc);

	bbpu = RTC_BBPU_KEY | RTC_BBPU_PWREN | RTC_BBPU_RESET_AL;
	ret = rtc_write(rtc, rtc->addr_base + RTC_BBPU, bbpu);
	if (ret < 0) {
		dev_err(rtc->rtc_dev->dev.parent, "%s: write rtc bbpu error\n", __func__);
		goto exit;
	}

	mtk_rtc_write_trigger(rtc);
exit:
	power_down_mclk(rtc);
	return;
}

static int mtk_rtc_is_alarm_irq(struct mt6685_rtc *rtc)
{
	u32 irqsta = 0, bbpu = 0, sck = 0, sck_check = 0, irqsta_check = 0;
	int ret;

	power_on_mclk(rtc);

	/* read clear */
	ret = rtc_read(rtc, rtc->addr_base + RTC_IRQ_STA, &irqsta);
	dev_dbg(rtc->rtc_dev->dev.parent, "%s: IRQ STA 0x%x\n", __func__, irqsta);

	/* clear SCK_TOP rtc interrupt */
	rtc_read(rtc, SCK_TOP_INT_STATUS0, &sck);
	rtc_write(rtc, SCK_TOP_INT_STATUS0, sck);

	rtc_read(rtc, SCK_TOP_INT_STATUS0, &sck_check);
	if (sck_check) {
		udelay(70);
		rtc_write(rtc, SCK_TOP_INT_STATUS0, 1);

		rtc_read(rtc, SCK_TOP_INT_STATUS0, &sck_check);
		if (sck_check) {
			dev_notice(rtc->rtc_dev->dev.parent,
				   "%s: TOP INT STA 0x%x\n", __func__, sck_check);

			rtc_read(rtc, rtc->addr_base + RTC_IRQ_STA, &irqsta_check);
			dev_notice(rtc->rtc_dev->dev.parent,
				   "%s: IRQ STA 0x%x\n", __func__, irqsta_check);
		}
	}

	if ((ret == 0) && (irqsta & RTC_IRQ_STA_AL)) {
		bbpu = RTC_BBPU_KEY | RTC_BBPU_PWREN;
		ret = rtc_write(rtc,
				rtc->addr_base + RTC_BBPU, bbpu);
		if (ret < 0)
			dev_err(rtc->rtc_dev->dev.parent,
				"%s: %d error\n", __func__, __LINE__);

		ret =  rtc_update_bits(rtc,
				       rtc->addr_base + RTC_IRQ_EN,
				       RTC_IRQ_EN_AL, 0);
		if (ret < 0)
			dev_err(rtc->rtc_dev->dev.parent,
				"%s: %d error\n", __func__, __LINE__);

		mtk_rtc_write_trigger(rtc);
		power_down_mclk(rtc);

		return RTC_ALSTA;
	}

	power_down_mclk(rtc);

	return RTC_NONE;
}

static irqreturn_t mtk_rtc_irq_handler_thread(int irq, void *data)
{
	struct mt6685_rtc *rtc = data;
	int status = RTC_NONE;

	guard(mutex)(&rtc->lock);

	status = mtk_rtc_is_alarm_irq(rtc);

	if (rtc->rtc_dev == NULL)
		return IRQ_NONE;

	if (status == RTC_NONE)
		return IRQ_NONE;

	mtk_rtc_reset_bbpu_alarm_status(rtc);

	if (rtc->rtc_dev != NULL)
		rtc_update_irq(rtc->rtc_dev, 1, RTC_IRQF | RTC_AF);

	return IRQ_HANDLED;
}

static int __mtk_rtc_read_time(struct mt6685_rtc *rtc,
			       struct rtc_time *tm, int *sec)
{
	int ret;
	unsigned int reload = 0;
	u16 data[RTC_OFFSET_COUNT] = { 0 };

	power_on_mclk(rtc);

	rtc_read(rtc, rtc->addr_base + RTC_BBPU, &reload);
	reload = reload | RTC_BBPU_KEY | RTC_BBPU_RELOAD;
	rtc_write(rtc, rtc->addr_base + RTC_BBPU, reload);
	mtk_rtc_write_trigger(rtc);
	power_down_mclk(rtc);

	guard(mutex)(&rtc->lock);
	ret = regmap_bulk_read(rtc->regmap, rtc->addr_base + RTC_TC_SEC,
			       data, RTC_OFFSET_COUNT * 2);
	if (ret < 0)
		return ret;

	tm->tm_sec = data[RTC_OFFSET_SEC] & RTC_TC_SEC_MASK;
	tm->tm_min = data[RTC_OFFSET_MIN] & RTC_TC_MIN_MASK;
	tm->tm_hour = data[RTC_OFFSET_HOUR] & RTC_TC_HOU_MASK;
	tm->tm_mday = data[RTC_OFFSET_DOM] & RTC_TC_DOM_MASK;
	tm->tm_mon = data[RTC_OFFSET_MTH] & RTC_TC_MTH_MASK;
	tm->tm_year = data[RTC_OFFSET_YEAR] & RTC_TC_YEA_MASK;

	ret = rtc_read(rtc, rtc->addr_base + RTC_TC_SEC, sec);
	*sec &= RTC_TC_SEC_MASK;

	return ret;
}

static int mtk_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	time64_t time;
	struct mt6685_rtc *rtc = dev_get_drvdata(dev);
	int days, sec, ret;

	do {
		ret = __mtk_rtc_read_time(rtc, tm, &sec);
		if (ret < 0)
			return ret;
	} while (sec < tm->tm_sec);

	/* HW register use 7 bits to store year data, minus
	 * RTC_MIN_YEAR_OFFSET before write year data to register, and plus
	 * RTC_MIN_YEAR_OFFSET back after read year from register
	 */

	/* HW register start mon from one, but tm_mon start from zero. */
	tm->tm_mon--;
	time = rtc_tm_to_time64(tm);

	/* rtc_tm_to_time64 covert Gregorian date to seconds since
	 * 01-01-1970 00:00:00, and this date is Thursday.
	 */
	days = div_s64(time, 86400);
	tm->tm_wday = (days + 4) % 7;

	return ret;
}

static int mtk_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct mt6685_rtc *rtc = dev_get_drvdata(dev);
	int ret;
	u16 data[RTC_OFFSET_COUNT];

	power_on_mclk(rtc);

	tm->tm_mon++;

	data[RTC_OFFSET_SEC] = tm->tm_sec;
	data[RTC_OFFSET_MIN] = tm->tm_min;
	data[RTC_OFFSET_HOUR] = tm->tm_hour;
	data[RTC_OFFSET_DOM] = tm->tm_mday;
	data[RTC_OFFSET_MTH] = tm->tm_mon;
	data[RTC_OFFSET_YEAR] = tm->tm_year;

	guard(mutex)(&rtc->lock);
	ret = regmap_bulk_write(rtc->regmap, rtc->addr_base + RTC_TC_SEC,
				data, RTC_OFFSET_COUNT * 2);
	if (ret < 0)
		goto exit;

	/* Time register write to hardware after call trigger function */
	ret = mtk_rtc_write_trigger(rtc);

exit:
	power_down_mclk(rtc);

	return ret;
}

static int mtk_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
	struct rtc_time *tm = &alm->time;
	struct mt6685_rtc *rtc = dev_get_drvdata(dev);
	u32 irqen = 0, pdn2 = 0;
	int ret;
	u16 data[RTC_OFFSET_COUNT] = { 0 };

	guard(mutex)(&rtc->lock);
	ret = rtc_read(rtc, rtc->addr_base + RTC_IRQ_EN, &irqen);
	if (ret < 0)
		return ret;

	ret = rtc_read(rtc, rtc->addr_base + RTC_PDN2, &pdn2);
	if (ret < 0)
		return ret;

	ret = regmap_bulk_read(rtc->regmap, rtc->addr_base + RTC_AL_SEC,
			       data, RTC_OFFSET_COUNT * 2);
	if (ret < 0)
		return ret;

	alm->enabled = !!(irqen & RTC_IRQ_EN_AL);
	alm->pending = !!(pdn2 & RTC_PDN2_PWRON_ALARM);

	tm->tm_sec = data[RTC_OFFSET_SEC] & RTC_AL_SEC_MASK;
	tm->tm_min = data[RTC_OFFSET_MIN] & RTC_AL_MIN_MASK;
	tm->tm_hour = data[RTC_OFFSET_HOUR] & RTC_AL_HOU_MASK;
	tm->tm_mday = data[RTC_OFFSET_DOM] & RTC_AL_DOM_MASK;
	tm->tm_mon = data[RTC_OFFSET_MTH] & RTC_AL_MTH_MASK;
	tm->tm_year = data[RTC_OFFSET_YEAR] & RTC_AL_YEA_MASK;

	tm->tm_mon--;

	return 0;
}

static int mtk_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
	struct rtc_time *tm = &alm->time;
	struct mt6685_rtc *rtc = dev_get_drvdata(dev);
	int ret;
	u16 data[RTC_OFFSET_COUNT];

	power_on_mclk(rtc);

	tm->tm_mon++;
	guard(mutex)(&rtc->lock);
	ret = regmap_bulk_read(rtc->regmap, rtc->addr_base + RTC_AL_SEC,
			       data, RTC_OFFSET_COUNT * 2);
	if (ret < 0)
		goto exit;

	data[RTC_OFFSET_SEC] = ((data[RTC_OFFSET_SEC] & ~(RTC_AL_SEC_MASK)) |
				(tm->tm_sec & RTC_AL_SEC_MASK));
	data[RTC_OFFSET_MIN] = ((data[RTC_OFFSET_MIN] & ~(RTC_AL_MIN_MASK)) |
				(tm->tm_min & RTC_AL_MIN_MASK));
	data[RTC_OFFSET_HOUR] = ((data[RTC_OFFSET_HOUR] & ~(RTC_AL_HOU_MASK)) |
				(tm->tm_hour & RTC_AL_HOU_MASK));
	data[RTC_OFFSET_DOM] = ((data[RTC_OFFSET_DOM] & ~(RTC_AL_DOM_MASK)) |
				(tm->tm_mday & RTC_AL_DOM_MASK));
	data[RTC_OFFSET_MTH] = ((data[RTC_OFFSET_MTH] & ~(RTC_AL_MTH_MASK)) |
				(tm->tm_mon & RTC_AL_MTH_MASK));
	data[RTC_OFFSET_YEAR] = ((data[RTC_OFFSET_YEAR] & ~(RTC_AL_YEA_MASK)) |
				(tm->tm_year & RTC_AL_YEA_MASK));

	if (alm->enabled) {
		ret = regmap_bulk_write(rtc->regmap,
					rtc->addr_base + RTC_AL_SEC,
					data, RTC_OFFSET_COUNT * 2);
		if (ret < 0)
			goto exit;

		ret = rtc_write(rtc, rtc->addr_base + RTC_AL_MASK,
				RTC_AL_MASK_DOW);
		if (ret < 0)
			goto exit;

		ret =  rtc_update_bits(rtc,
				       rtc->addr_base + RTC_IRQ_EN,
				       RTC_IRQ_EN_AL, RTC_IRQ_EN_AL);
		if (ret < 0)
			goto exit;
		} else {
			ret =  rtc_update_bits(rtc,
					       rtc->addr_base + RTC_IRQ_EN,
					       RTC_IRQ_EN_AL, 0);
			if (ret < 0)
				goto exit;
		}

	/* All alarm time register write to hardware after calling
	 * mtk_rtc_write_trigger. This can avoid race condition if alarm
	 * occur happen during writing alarm time register.
	 */
	ret = mtk_rtc_write_trigger(rtc);

exit:
	power_down_mclk(rtc);
	return ret;
}

static const struct rtc_class_ops mtk_rtc_ops = {
	.read_time  = mtk_rtc_read_time,
	.set_time   = mtk_rtc_set_time,
	.read_alarm = mtk_rtc_read_alarm,
	.set_alarm  = mtk_rtc_set_alarm,
};

static int mtk_rtc_probe(struct platform_device *pdev)
{
	struct mt6685_rtc *rtc;
	struct device_node *np = pdev->dev.of_node;
	int ret;

	if (!of_device_is_available(np)) {
		dev_err(&pdev->dev, "rtc disabled\n");
		return -ENODEV;
	}

	rtc = devm_kzalloc(&pdev->dev, sizeof(struct mt6685_rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	rtc->data = of_device_get_match_data(&pdev->dev);
	if (!rtc->data) {
		dev_err(&pdev->dev, "of_device_get_match_data failed\n");
		return -ENODEV;
	}

	if (of_property_read_u32(pdev->dev.of_node, "base", &rtc->addr_base))
		rtc->addr_base = RTC_DSN_ID;

	mutex_init(&rtc->lock);
	mutex_init(&rtc->clk_lock);

	platform_set_drvdata(pdev, rtc);

	rtc->rtc_dev = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc->rtc_dev))
		return PTR_ERR(rtc->rtc_dev);

	rtc->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!rtc->regmap)
		return -ENODEV;

	/* Obtain interrupt ID from DTS or MFD */
	rtc->irq = platform_get_irq(pdev, 0);
	if (rtc->irq < 0)
		return rtc->irq;

	rtc->clk_cnt = 0;
	ret = devm_request_threaded_irq(&pdev->dev, rtc->irq, NULL,
					mtk_rtc_irq_handler_thread,
					IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
					"mt6685-rtc", rtc);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request alarm IRQ: %d: %d\n",
			rtc->irq, ret);
		return ret;
	}

	/*Enable SCK_TOP rtc interrupt*/
	rtc_update_bits(rtc, SCK_TOP_INT_CON0, EN_RTC_INTERRUPT, 1);

	device_init_wakeup(&pdev->dev, 1);

	rtc->rtc_dev->ops = &mtk_rtc_ops;
	ret = devm_rtc_register_device(rtc->rtc_dev);
	if (ret) {
		dev_err(&pdev->dev, "register rtc device failed\n");
		return ret;
	};

	rtc->rtc_dev->range_min = RTC_TIMESTAMP_BEGIN_1900;
	rtc->rtc_dev->range_max = mktime64(2027, 12, 31, 23, 59, 59);
	rtc->rtc_dev->start_secs = mktime64(1968, 1, 2, 0, 0, 0);
	rtc->rtc_dev->set_start_time = true;

	power_on_mclk(rtc);
	power_down_mclk(rtc);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int mt6685_rtc_suspend(struct device *dev)
{
	struct mt6685_rtc *rtc = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		enable_irq_wake(rtc->irq);

	return 0;
}

static int mt6685_rtc_resume(struct device *dev)
{
	struct mt6685_rtc *rtc = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(rtc->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(mt6685_pm_ops, mt6685_rtc_suspend,
			 mt6685_rtc_resume);
#endif

static const struct mtk_rtc_data mt6685_rtc_data = {
	.wrtgr = RTC_WRTGR,
};

static const struct of_device_id mt6685_rtc_of_match[] = {
	{ .compatible = "mediatek,mt6685-rtc", .data = &mt6685_rtc_data },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6685_rtc_of_match);

static struct platform_driver mtk_rtc_driver = {
	.driver = {
		.name = "mt6685-rtc",
		.of_match_table = mt6685_rtc_of_match,
#ifdef CONFIG_PM_SLEEP
		.pm = &mt6685_pm_ops,
#endif
	},
	.probe	= mtk_rtc_probe,
};

module_platform_driver(mtk_rtc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mw Lin <Mw.Lin@mediatek.com>");
MODULE_DESCRIPTION("RTC Driver for MediaTek MT6685 Clock IC");
