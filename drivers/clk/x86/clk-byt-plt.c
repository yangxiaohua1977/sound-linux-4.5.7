/*
 * intel Atom platform clocks driver for Baytrail and CherryTrail SoC.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Irina Tirdea <irina.tirdea@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/clkdev.h>

#include <asm/pmc_atom.h>

#define PLT_CLK_NAME_BASE	"pmc_plt_clk_"
#define PLT_CLK_DRIVER_NAME	"clk-byt-plt"

#define PMC_CLK_CTL_0		0x60
#define PMC_CLK_CTL_SIZE	4
#define PMC_CLK_NUM		6
#define PMC_MASK_CLK_CTL	GENMASK(1, 0)
#define PMC_MASK_CLK_FREQ	BIT(2)
#define PMC_CLK_CTL_GATED_ON_D3	0x0
#define PMC_CLK_CTL_FORCE_ON	0x1
#define PMC_CLK_CTL_FORCE_OFF	0x2
#define PMC_CLK_CTL_RESERVED	0x3
#define PMC_CLK_FREQ_XTAL	0x0	/* 25 MHz */
#define PMC_CLK_FREQ_PLL	0x4	/* 19.2 MHz */

struct clk_plt_fixed {
	struct clk *clk;
	struct clk_lookup *lookup;
};

struct clk_plt {
	struct clk_hw hw;
	u8 id;
	u32 offset;
	struct clk_lookup *lookup;
	spinlock_t lock;
};

#define to_clk_plt(_hw) container_of(_hw, struct clk_plt, hw)

struct clk_plt_data {
	struct clk_plt_fixed **parents;
	u8 nparents;
	struct clk *clks[PMC_CLK_NUM];
};

static inline int plt_reg_to_parent(int reg)
{
	switch (reg & PMC_MASK_CLK_FREQ) {
	case PMC_CLK_FREQ_XTAL:
		return 0;	/* index 0 in parents[] */
	case PMC_CLK_FREQ_PLL:
		return 1;	/* index 1 in parents[] */
	}

	return 0;
}

static inline int plt_parent_to_reg(int index)
{
	switch (index) {
	case 0:	/* index 0 in parents[] */
		return PMC_CLK_FREQ_XTAL;
	case 1:	/* index 0 in parents[] */
		return PMC_CLK_FREQ_PLL;
	}

	return PMC_CLK_FREQ_XTAL;
}

static inline int plt_reg_to_enabled(int reg)
{
	switch (reg & PMC_MASK_CLK_CTL) {
	case PMC_CLK_CTL_GATED_ON_D3:
	case PMC_CLK_CTL_FORCE_ON:
		return 1;	/* enabled */
	case PMC_CLK_CTL_FORCE_OFF:
	case PMC_CLK_CTL_RESERVED:
	default:
		return 0;	/* disabled */
	}
}

static int plt_pmc_atom_update(struct clk_plt *clk, u32 mask, u32 val)
{
	int ret;
	u32 orig, tmp;
	unsigned long flags = 0;

	spin_lock_irqsave(&clk->lock, flags);

	ret = pmc_atom_read(clk->offset, &orig);
	if (ret)
		goto out;

	tmp = orig & ~mask;
	tmp |= val & mask;

	if (tmp == orig)
		goto out;

	ret = pmc_atom_write(clk->offset, tmp);
	if (ret)
		goto out;

out:
	spin_unlock_irqrestore(&clk->lock, flags);

	return ret;
}

static int plt_clk_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_plt *clk = to_clk_plt(hw);

	return plt_pmc_atom_update(clk, PMC_MASK_CLK_FREQ,
				   plt_parent_to_reg(index));
}

static u8 plt_clk_get_parent(struct clk_hw *hw)
{
	struct clk_plt *clk = to_clk_plt(hw);
	u32 value;
	int ret;

	ret = pmc_atom_read(clk->offset, &value);
	if (ret)
		return ret;

	return plt_reg_to_parent(value);
}

static int plt_clk_enable(struct clk_hw *hw)
{
	struct clk_plt *clk = to_clk_plt(hw);

	return plt_pmc_atom_update(clk, PMC_MASK_CLK_CTL, PMC_CLK_CTL_FORCE_ON);
}

static void plt_clk_disable(struct clk_hw *hw)
{
	struct clk_plt *clk = to_clk_plt(hw);

	plt_pmc_atom_update(clk, PMC_MASK_CLK_CTL, PMC_CLK_CTL_FORCE_OFF);
}

static int plt_clk_is_enabled(struct clk_hw *hw)
{
	struct clk_plt *clk = to_clk_plt(hw);
	u32 value;
	int ret;

	ret = pmc_atom_read(clk->offset, &value);
	if (ret)
		return ret;

	return plt_reg_to_enabled(value);
}

static const struct clk_ops plt_clk_ops = {
	.enable = plt_clk_enable,
	.disable = plt_clk_disable,
	.is_enabled = plt_clk_is_enabled,
	.get_parent = plt_clk_get_parent,
	.set_parent = plt_clk_set_parent,
	.determine_rate = __clk_mux_determine_rate,
};

static struct clk *plt_clk_register(struct platform_device *pdev, int id,
				    const char **parent_names, int num_parents)
{
	struct clk_plt *pclk;
	struct clk *clk;
	struct clk_init_data init;
	int ret = 0;

	pclk = devm_kzalloc(&pdev->dev, sizeof(*pclk), GFP_KERNEL);
	if (!pclk)
		return ERR_PTR(-ENOMEM);

	init.name =  kasprintf(GFP_KERNEL, "%s%d", PLT_CLK_NAME_BASE, id);
	init.ops = &plt_clk_ops;
	init.flags = 0;
	init.parent_names = parent_names;
	init.num_parents = num_parents;

	pclk->hw.init = &init;
	pclk->id = id;
	pclk->offset = PMC_CLK_CTL_0 + id * PMC_CLK_CTL_SIZE;
	spin_lock_init(&pclk->lock);

	clk = clk_register(&pdev->dev, &pclk->hw);
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		goto err_free_pclk;
	}

	pclk->lookup = clkdev_create(clk, init.name, NULL);
	if (!pclk->lookup) {
		ret = -ENOMEM;
		goto err_clk_unregister;
	}

	kfree(init.name);

	return clk;

err_clk_unregister:
	clk_unregister(clk);
err_free_pclk:
	kfree(init.name);
	return ERR_PTR(ret);
}

static void plt_clk_unregister(struct clk *clk)
{
	struct clk_plt *pclk;
	struct clk_hw *hw;

	hw = __clk_get_hw(clk);
	if (!hw)
		return;

	pclk = to_clk_plt(hw);

	clkdev_drop(pclk->lookup);
	clk_unregister(clk);
}

static struct clk_plt_fixed *plt_clk_register_fixed_rate(struct platform_device *pdev,
						 const char *name,
						 const char *parent_name,
						 unsigned long fixed_rate)
{
	struct clk_plt_fixed *pclk;
	int ret = 0;

	pclk = devm_kzalloc(&pdev->dev, sizeof(*pclk), GFP_KERNEL);
	if (!pclk)
		return ERR_PTR(-ENOMEM);

	pclk->clk = clk_register_fixed_rate(&pdev->dev, name, parent_name,
					    0, fixed_rate);
	if (IS_ERR(pclk->clk)) {
		ret = PTR_ERR(pclk->clk);
		return ERR_PTR(ret);
	}

	pclk->lookup = clkdev_create(pclk->clk, name, NULL);
	if (!pclk->lookup) {
		ret = -ENOMEM;
		goto err_clk_unregister;
	}

	return pclk;

err_clk_unregister:
	//clk_unregister_fixed_rate(pclk->clk);
	clk_unregister(pclk->clk);
	return ERR_PTR(ret);
}

static void plt_clk_unregister_fixed_rate(struct clk_plt_fixed *pclk)
{
	clkdev_drop(pclk->lookup);
	//clk_unregister_fixed_rate(pclk->clk);
	clk_unregister(pclk->clk);
}

static const char **plt_clk_register_parents(struct platform_device *pdev,
					     struct clk_plt_data *data)
{
	struct pmc_clk **pclks, *clks;
	const char **parent_names;
	int i, err;

	data->nparents = 0;
	pclks = dev_get_platdata(&pdev->dev);
	if (!pclks)
		return NULL;

	clks = *pclks;
	while (clks[data->nparents].name)
		data->nparents++;

	data->parents = devm_kzalloc(&pdev->dev,
				     sizeof(*data->parents) * data->nparents,
				     GFP_KERNEL);
	if (!data->parents) {
		err = -ENOMEM;
		goto err_out;
	}

	parent_names = kcalloc(data->nparents, sizeof(*parent_names),
			       GFP_KERNEL);
	if (!parent_names) {
		err = -ENOMEM;
		goto err_out;
	}

	for (i = 0; i < data->nparents; i++) {
		data->parents[i] =
			plt_clk_register_fixed_rate(pdev, clks[i].name,
						    clks[i].parent_name,
						    clks[i].freq);
		if (IS_ERR(data->parents[i])) {
			err = PTR_ERR(data->parents[i]);
		goto err_unreg;
		}
		parent_names[i] = kstrdup_const(clks[i].name, GFP_KERNEL);
	}

	return parent_names;

err_unreg:
	for (i--; i >= 0; i--) {
		plt_clk_unregister_fixed_rate(data->parents[i]);
		kfree_const(parent_names[i]);
	}
	kfree(parent_names);
err_out:
	data->nparents = 0;
	return ERR_PTR(err);
}

static void plt_clk_unregister_parents(struct clk_plt_data *data)
{
	int i;

	for (i = 0; i < data->nparents; i++)
		plt_clk_unregister_fixed_rate(data->parents[i]);
}

static int plt_clk_probe(struct platform_device *pdev)
{
	struct clk_plt_data *data;
	int i, err;
	const char **parent_names;
	printk("Enter into %s\n", __func__);
	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	parent_names = plt_clk_register_parents(pdev, data);
	if (IS_ERR(parent_names))
		return PTR_ERR(parent_names);

	for (i = 0; i < PMC_CLK_NUM; i++) {
		data->clks[i] = plt_clk_register(pdev, i, parent_names,
						 data->nparents);
		if (IS_ERR(data->clks[i])) {
			err = PTR_ERR(data->clks[i]);
			goto err_unreg_clk_plt;
		}
	}

	for (i = 0; i < data->nparents; i++)
		kfree_const(parent_names[i]);
	kfree(parent_names);

	dev_set_drvdata(&pdev->dev, data);
	return 0;
	printk("exit %s\n", __func__);
err_unreg_clk_plt:
	for (i--; i >= 0; i--)
		plt_clk_unregister(data->clks[i]);
	plt_clk_unregister_parents(data);
	for (i = 0; i < data->nparents; i++)
		kfree_const(parent_names[i]);
	kfree(parent_names);
	return err;
}

static int plt_clk_remove(struct platform_device *pdev)
{
	struct clk_plt_data *data;
	int i;

	data = dev_get_drvdata(&pdev->dev);
	if (!data)
		return 0;

	for (i = 0; i < PMC_CLK_NUM; i++)
		plt_clk_unregister(data->clks[i]);
	plt_clk_unregister_parents(data);
	return 0;
}

static struct platform_driver plt_clk_driver = {
	.driver = {
		.name = PLT_CLK_DRIVER_NAME,
	},
	.probe = plt_clk_probe,
	.remove = plt_clk_remove,
};
module_platform_driver(plt_clk_driver);

MODULE_DESCRIPTION("Intel Atom platform clocks driver");
MODULE_AUTHOR("Irina Tirdea <irina.tirdea@intel.com>");
MODULE_LICENSE("GPL v2");

