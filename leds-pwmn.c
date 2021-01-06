// SPDX-License-Identifier: GPL-2.0-only
/*
 * leds-pwmn.c
 *
 * PWM based LED control using multiple channels
 *
 * Copyright 2021 Jan Pohanka (jan.pohanka@steinel.cz)
 *
 * based on leds-pwm.c by Luotao Fu <l.fu@pengutronix.de>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/leds.h>
#include <linux/err.h>
#include <linux/pwm.h>
#include <linux/leds_pwm.h>
#include <linux/slab.h>

struct led_pwmn_attribute {
	struct device_attribute dev_attr;
	int index;
	int val;
};

struct led_pwm_data {
	struct led_classdev	cdev;
	struct pwm_device	**pwm;
	unsigned int		active_low;
	unsigned int		period;
	int			duty;
	int 			channel_cnt;
	struct led_pwmn_attribute	*pattr;
};

struct led_pwm_priv {
	int num_leds;
	struct led_pwm_data leds[0];
};

static void __led_pwm_set(struct led_pwm_data *led_dat)
{
	int new_duty = led_dat->duty;
	int i;

	for (i = 0; i < led_dat->channel_cnt; i++) {
		unsigned long long rel_duty = new_duty;
		unsigned int max = led_dat->cdev.max_brightness;

		rel_duty *= led_dat->pattr[i].val;
		do_div(rel_duty, max);

		if (led_dat->active_low)
			rel_duty = led_dat->period - rel_duty;

		pwm_config(led_dat->pwm[i], rel_duty, led_dat->period);
	}

	for (i = 0; i < led_dat->channel_cnt; i++) {
		if (new_duty == 0)
			pwm_disable(led_dat->pwm[i]);
		else
			pwm_enable(led_dat->pwm[i]);
	}
}

static int led_pwm_set(struct led_classdev *led_cdev,
		       enum led_brightness brightness)
{
	struct led_pwm_data *led_dat =
		container_of(led_cdev, struct led_pwm_data, cdev);
	unsigned int max = led_dat->cdev.max_brightness;
	unsigned long long duty =  led_dat->period;

	duty *= brightness;
	do_div(duty, max);

	led_dat->duty = duty;

	__led_pwm_set(led_dat);

	return 0;
}

static ssize_t show_channel(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct led_pwmn_attribute *pattr =
			container_of(attr, struct led_pwmn_attribute, dev_attr);

	return scnprintf(buf, PAGE_SIZE, "%d\n", pattr->val);
}

static ssize_t store_channel(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct led_classdev *led_cdev = led_trigger_get_led(dev);
	struct led_pwmn_attribute *pattr =
			container_of(attr, struct led_pwmn_attribute, dev_attr);
	unsigned long state;
	ssize_t ret;

	mutex_lock(&led_cdev->led_access);

	if (led_sysfs_is_disabled(led_cdev)) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = kstrtoul(buf, 10, &state);
	if (ret)
		goto unlock;

	pattr->val = state;

	led_set_brightness(led_cdev, led_cdev->brightness);
	flush_work(&led_cdev->set_brightness_work);

	ret = count;
unlock:
	mutex_unlock(&led_cdev->led_access);
	return ret;
}

static int led_pwm_add(struct device *dev, struct led_pwm_priv *priv,
		       struct led_pwm *led, struct fwnode_handle *fwnode)
{
	struct led_pwm_data *led_data = &priv->leds[priv->num_leds];
	struct pwm_args pargs;
	struct property *prop;
	struct led_pwmn_attribute *pattr;
	struct attribute **attributes;
	struct attribute_group *attr_group;
	const struct attribute_group **groups;
	const char *name;
	int i;
	int cnt;
	int ret;

	/* fixme */
	if (!fwnode) {
		dev_err(dev, "not implemented");
	}

	led_data->active_low = led->active_low;
	led_data->cdev.name = led->name;
	led_data->cdev.default_trigger = led->default_trigger;
	led_data->cdev.brightness = LED_OFF;
	led_data->cdev.max_brightness = led->max_brightness;
	led_data->cdev.flags = LED_CORE_SUSPENDRESUME;

	cnt = of_count_phandle_with_args(to_of_node(fwnode), "pwms", "#pwm-cells");

	led_data->pwm = devm_kzalloc(dev, cnt * sizeof(struct pwm_device *), GFP_KERNEL);
	if (!led_data->pwm)
		return -ENOMEM;

	pattr = devm_kzalloc(dev, cnt * sizeof(struct led_pwmn_attribute), GFP_KERNEL);
	if (!pattr)
		return -ENOMEM;

	attributes = devm_kcalloc(dev, cnt + 1, sizeof(struct attribute *), GFP_KERNEL);
	if (!attributes )
		return -ENOMEM;

	attr_group = devm_kzalloc(dev, sizeof(struct attribute_group), GFP_KERNEL);
	if (!attr_group )
			return -ENOMEM;

	groups = devm_kcalloc(dev, 1 + 1, sizeof(struct attribute_group *), GFP_KERNEL);
	if (!groups )
		return -ENOMEM;

	i = 0;
	of_property_for_each_string(to_of_node(fwnode), "pwm-names", prop, name) {
		led_data->pwm[i] = devm_of_pwm_get(dev, to_of_node(fwnode), name);

		if (IS_ERR(led_data->pwm[i])) {
			ret = PTR_ERR(led_data->pwm[i]);
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "unable to request PWM for %s: %d\n",
						name, ret);
			return ret;
		}

		sysfs_bin_attr_init(&led_dat->attr[i]);
		pattr[i].dev_attr.attr.name = devm_kasprintf(dev, GFP_KERNEL,
				"%s",
				name);

		if (!pattr[i].dev_attr.attr.name)
			return -ENOMEM;

		pattr[i].dev_attr.attr.mode = S_IRUSR | S_IWUSR;
		pattr[i].dev_attr.show = show_channel;
		pattr[i].dev_attr.store = store_channel;
		pattr[i].index = i;
		pattr[i].val = led->max_brightness;

		attributes[i] = &pattr[i].dev_attr.attr;

		i++;
	}

	if (i != cnt) {
		dev_err(dev, "failed to get requested PWMs");
		return -EINVAL;
	}

	attr_group->attrs = attributes;
	groups[0] = attr_group;
	led_data->cdev.groups = groups;

	led_data->channel_cnt = i;
	led_data->cdev.brightness_set_blocking = led_pwm_set;
	led_data->pattr = pattr;

	/*
	 * FIXME: pwm_apply_args() should be removed when switching to the
	 * atomic PWM API.
	 */
	for (i = 0; i < led_data->channel_cnt; i++) {
		pwm_apply_args(led_data->pwm[i]);

		pwm_get_args(led_data->pwm[i], &pargs);

		led_data->period = pargs.period;
		if (!led_data->period && (led->pwm_period_ns > 0))
			led_data->period = led->pwm_period_ns;
	}

	ret = devm_led_classdev_register(dev, &led_data->cdev);
	if (ret == 0) {
		priv->num_leds++;
		led_pwm_set(&led_data->cdev, led_data->cdev.brightness);
	} else {
		dev_err(dev, "failed to register PWM led for %s: %d\n",
			led->name, ret);
	}

	return ret;
}

static int led_pwm_create_fwnode(struct device *dev, struct led_pwm_priv *priv)
{
	struct fwnode_handle *fwnode;
	struct led_pwm led;
	int ret = 0;

	memset(&led, 0, sizeof(led));

	device_for_each_child_node(dev, fwnode) {
		ret = fwnode_property_read_string(fwnode, "label", &led.name);
		if (ret && is_of_node(fwnode))
			led.name = to_of_node(fwnode)->name;

		if (!led.name) {
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		fwnode_property_read_string(fwnode, "linux,default-trigger",
					    &led.default_trigger);

		led.active_low = fwnode_property_read_bool(fwnode,
							   "active-low");
		fwnode_property_read_u32(fwnode, "max-brightness",
					 &led.max_brightness);

		ret = led_pwm_add(dev, priv, &led, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			break;
		}
	}

	return ret;
}

static int led_pwm_probe(struct platform_device *pdev)
{
	struct led_pwm_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct led_pwm_priv *priv;
	int count, i;
	int ret = 0;

	if (pdata)
		count = pdata->num_leds;
	else
		count = device_get_child_node_count(&pdev->dev);

	if (!count)
		return -EINVAL;

	priv = devm_kzalloc(&pdev->dev, struct_size(priv, leds, count),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (pdata) {
		for (i = 0; i < count; i++) {
			ret = led_pwm_add(&pdev->dev, priv, &pdata->leds[i],
					  NULL);
			if (ret)
				break;
		}
	} else {
		ret = led_pwm_create_fwnode(&pdev->dev, priv);
	}

	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	return 0;
}

static const struct of_device_id of_pwm_leds_match[] = {
	{ .compatible = "pwm-ledsn", },
	{},
};
MODULE_DEVICE_TABLE(of, of_pwm_leds_match);

static struct platform_driver led_pwmm_driver = {
	.probe		= led_pwm_probe,
	.driver		= {
		.name	= "leds_pwmn",
		.of_match_table = of_pwm_leds_match,
	},
};

module_platform_driver(led_pwmm_driver);

MODULE_AUTHOR("Jan Pohanka <jan.pohanka@steinel.com>");
MODULE_DESCRIPTION("generic multichannel PWM LED driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:leds-pwmn");
