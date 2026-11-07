// SPDX-License-Identifier: GPL-2.0-only
/*
 * CVITEK SPACC crypto engine
 *
 * The engine processes one transfer at a time and signals completion with
 * a single interrupt.  Requests are serialized by crypto_engine; the
 * interrupt only has to end the wait.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "spacc.h"

static irqreturn_t spacc_irq(int irq, void *data)
{
	struct spacc_dev *spacc = data;
	u32 status = readl(spacc->base + SPACC_REG_IRQ_STATUS);

	if (!(status & SPACC_IRQ_BITS))
		return IRQ_NONE;

	writel(status & SPACC_IRQ_BITS, spacc->base + SPACC_REG_IRQ_STATUS);
	complete(&spacc->done);

	return IRQ_HANDLED;
}

/*
 * Build the descriptor in coherent memory and let the engine run.  The
 * request is complete when the engine raises the interrupt.
 */
int spacc_execute(struct spacc_dev *spacc, const struct spacc_op *op)
{
	u32 *desc = spacc->desc;
	u32 dma_ctrl;
	int ret;

	if (spacc->wedged)
		return -EIO;

	memset(desc, 0, SPACC_DESC_WORDS * sizeof(*desc));
	desc[SPACC_DESC_CTRL] = op->ctrl | SPACC_CTRL_TRAILER;
	desc[SPACC_DESC_CIPHER] = op->cipher;
	desc[SPACC_DESC_SRC_LO] = lower_32_bits(op->src);
	desc[SPACC_DESC_SRC_HI] = upper_32_bits(op->src);
	desc[SPACC_DESC_DST_LO] = lower_32_bits(op->dst);
	desc[SPACC_DESC_DST_HI] = upper_32_bits(op->dst);
	desc[SPACC_DESC_LEN_LO] = lower_32_bits(op->len);
	desc[SPACC_DESC_LEN_HI] = upper_32_bits(op->len);
	if (op->key)
		memcpy(&desc[SPACC_DESC_KEY], op->key, op->keylen);
	if (op->iv)
		memcpy(&desc[SPACC_DESC_IV], op->iv, SPACC_MAX_IVLEN);

	/* Descriptor stores must be visible before the engine is started. */
	wmb();

	writel(lower_32_bits(spacc->desc_dma), spacc->base + SPACC_REG_DESC_LO);
	writel(upper_32_bits(spacc->desc_dma), spacc->base + SPACC_REG_DESC_HI);
	writel(SPACC_IRQ_BITS, spacc->base + SPACC_REG_IRQ_MASK);
	writel(SPACC_IRQ_BITS, spacc->base + SPACC_REG_IRQ_STATUS);

	reinit_completion(&spacc->done);

	dma_ctrl = (SPACC_DMA_WRITE_BURST << 24) | (SPACC_DMA_READ_BURST << 16) |
		   SPACC_DMA_DESC_MODE | SPACC_DMA_ENABLE;
	writel(dma_ctrl, spacc->base + SPACC_REG_DMA_CTRL);

	ret = wait_for_completion_timeout(&spacc->done,
					  msecs_to_jiffies(SPACC_TIMEOUT_MS));
	if (ret)
		return 0;

	/*
	 * The engine did not answer.  Mask the interrupt so a late completion
	 * cannot satisfy the next transfer, and stop using the device: there is
	 * no known way to reset the engine from here.
	 */
	dev_err(spacc->dev, "transfer timed out, engine disabled\n");
	writel(0, spacc->base + SPACC_REG_IRQ_MASK);
	writel(SPACC_IRQ_BITS, spacc->base + SPACC_REG_IRQ_STATUS);
	spacc->wedged = true;

	return -ETIMEDOUT;
}

static int spacc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spacc_dev *spacc;
	int irq, ret;

	spacc = devm_kzalloc(dev, sizeof(*spacc), GFP_KERNEL);
	if (!spacc)
		return -ENOMEM;

	spacc->dev = dev;
	platform_set_drvdata(pdev, spacc);

	/*
	 * The engine addresses memory through 32 bit descriptor fields, so the
	 * DMA mask has to be set before anything is mapped or allocated.
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	spacc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spacc->base))
		return PTR_ERR(spacc->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	spacc->clk_efuse = devm_clk_get_optional(dev, "efuse");
	if (IS_ERR(spacc->clk_efuse))
		return PTR_ERR(spacc->clk_efuse);

	spacc->clk_apb_efuse = devm_clk_get_optional(dev, "apb");
	if (IS_ERR(spacc->clk_apb_efuse))
		return PTR_ERR(spacc->clk_apb_efuse);

	ret = clk_prepare_enable(spacc->clk_efuse);
	if (ret)
		return ret;

	ret = clk_prepare_enable(spacc->clk_apb_efuse);
	if (ret)
		goto err_efuse_clk;

	/*
	 * The crypto subsystem is gated in sec_top.  The gate itself is only
	 * writable with the efuse clocks running, so this has to happen after
	 * they are enabled.  A zero write leaves the block running.
	 */
	spacc->sec_top = devm_platform_ioremap_resource(pdev, 1);
	if (!IS_ERR(spacc->sec_top))
		writel(0, spacc->sec_top);
	else
		spacc->sec_top = NULL;

	spacc->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(spacc->rst)) {
		ret = PTR_ERR(spacc->rst);
		goto err_clk;
	}

	ret = reset_control_deassert(spacc->rst);
	if (ret)
		goto err_clk;

	spacc->desc = dma_alloc_coherent(dev, SPACC_DESC_WORDS * sizeof(u32),
					 &spacc->desc_dma, GFP_KERNEL);
	if (!spacc->desc) {
		ret = -ENOMEM;
		goto err_reset;
	}

	init_completion(&spacc->done);

	ret = devm_request_irq(dev, irq, spacc_irq, 0, dev_name(dev), spacc);
	if (ret)
		goto err_desc;

	spacc->engine = crypto_engine_alloc_init(dev, true);
	if (!spacc->engine) {
		ret = -ENOMEM;
		goto err_desc;
	}

	ret = crypto_engine_start(spacc->engine);
	if (ret)
		goto err_engine;

	ret = spacc_cipher_register(spacc);
	if (ret)
		goto err_engine_start;

	return 0;

err_engine_start:
	crypto_engine_stop(spacc->engine);
err_engine:
	crypto_engine_exit(spacc->engine);
err_desc:
	dma_free_coherent(dev, SPACC_DESC_WORDS * sizeof(u32), spacc->desc,
			  spacc->desc_dma);
err_reset:
	reset_control_assert(spacc->rst);
err_clk:
	clk_disable_unprepare(spacc->clk_apb_efuse);
err_efuse_clk:
	clk_disable_unprepare(spacc->clk_efuse);

	return ret;
}

static void spacc_remove(struct platform_device *pdev)
{
	struct spacc_dev *spacc = platform_get_drvdata(pdev);

	spacc_cipher_unregister();
	crypto_engine_stop(spacc->engine);
	crypto_engine_exit(spacc->engine);
	dma_free_coherent(spacc->dev, SPACC_DESC_WORDS * sizeof(u32),
			  spacc->desc, spacc->desc_dma);
	reset_control_assert(spacc->rst);
	clk_disable_unprepare(spacc->clk_apb_efuse);
	clk_disable_unprepare(spacc->clk_efuse);
}

static const struct of_device_id spacc_of_match[] = {
	{ .compatible = "cvitek,spacc" },
	{ }
};
MODULE_DEVICE_TABLE(of, spacc_of_match);

static struct platform_driver spacc_driver = {
	.probe = spacc_probe,
	.remove = spacc_remove,
	.driver = {
		.name = "spacc",
		.of_match_table = spacc_of_match,
	},
};
module_platform_driver(spacc_driver);

MODULE_DESCRIPTION("CVITEK SPACC crypto engine");
MODULE_LICENSE("GPL");
