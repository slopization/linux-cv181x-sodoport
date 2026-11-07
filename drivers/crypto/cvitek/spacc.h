/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CVITEK SPACC crypto engine
 *
 * The block is a descriptor driven DMA front end for the SoC's symmetric
 * crypto core.  A transfer is described by a 22 word descriptor that the
 * engine reads from memory; completion is signalled through a single
 * interrupt.
 */

#ifndef _SPACC_H
#define _SPACC_H

#include <crypto/engine.h>
#include <crypto/skcipher.h>
#include <linux/completion.h>
#include <linux/io.h>

#define SPACC_MAX_KEYLEN	32
#define SPACC_MAX_IVLEN		16

/* Number of 32 bit words in a DMA descriptor. */
#define SPACC_DESC_WORDS	22

/* Register offsets. */
#define SPACC_REG_DMA_CTRL	0x000
#define SPACC_REG_IRQ_MASK	0x004
#define SPACC_REG_DESC_LO	0x008
#define SPACC_REG_DESC_HI	0x00c
#define SPACC_REG_IRQ_STATUS	0x010
#define SPACC_REG_SHA_STATE	0x1c0

/* Descriptor words. */
#define SPACC_DESC_CTRL		0
#define SPACC_DESC_CIPHER	1
#define SPACC_DESC_SRC_LO	4
#define SPACC_DESC_SRC_HI	5
#define SPACC_DESC_DST_LO	6
#define SPACC_DESC_DST_HI	7
#define SPACC_DESC_LEN_LO	8
#define SPACC_DESC_LEN_HI	9
#define SPACC_DESC_KEY		10	/* 8 words */
#define SPACC_DESC_IV		18	/* 4 words */

/* Descriptor control word: algorithm selection. */
#define SPACC_CTRL_AES		BIT(9)
#define SPACC_CTRL_DES		BIT(10)
#define SPACC_CTRL_SM4		BIT(11)
#define SPACC_CTRL_SHA		BIT(12)
#define SPACC_CTRL_DESC_KEY	BIT(19)
#define SPACC_CTRL_DESC_IV	BIT(23)
/*
 * Both the vendor kernel driver and the FSBL set the low nibble of the
 * control word.  No documentation for the field exists; keep it named
 * rather than magic.
 */
#define SPACC_CTRL_TRAILER	0xf

/* Descriptor cipher word. */
#define SPACC_CIPHER_ENCRYPT	BIT(0)
#define SPACC_CIPHER_CBC	BIT(1)
#define SPACC_CIPHER_CTR	BIT(2)
#define SPACC_CIPHER_KEY256	BIT(3)	/* also 3DES on the DES engine */
#define SPACC_CIPHER_KEY192	BIT(4)
#define SPACC_CIPHER_KEY128	BIT(5)
#define SPACC_CIPHER_SHA256	BIT(1)

/* DMA control. */
#define SPACC_DMA_READ_BURST	16
#define SPACC_DMA_WRITE_BURST	6
#define SPACC_DMA_DESC_MODE	BIT(1)
#define SPACC_DMA_ENABLE	BIT(0)

#define SPACC_IRQ_BITS		0x3
#define SPACC_TIMEOUT_MS	1000

struct spacc_dev {
	struct device *dev;
	void __iomem *base;
	struct clk *clk_efuse;
	struct clk *clk_apb_efuse;
	void __iomem *sec_top;
	struct reset_control *rst;
	struct crypto_engine *engine;

	u32 *desc;
	dma_addr_t desc_dma;

	struct completion done;
	bool wedged;
};

/* One transfer, as laid out in the descriptor. */
struct spacc_op {
	u32 ctrl;
	u32 cipher;
	dma_addr_t src;
	dma_addr_t dst;
	size_t len;
	const u8 *key;
	unsigned int keylen;
	const u8 *iv;
};

int spacc_execute(struct spacc_dev *spacc, const struct spacc_op *op);

int spacc_cipher_register(struct spacc_dev *spacc);
void spacc_cipher_unregister(void);

#endif /* _SPACC_H */
