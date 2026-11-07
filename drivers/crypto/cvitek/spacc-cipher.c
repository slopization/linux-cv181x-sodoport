// SPDX-License-Identifier: GPL-2.0-only
/*
 * CVITEK SPACC symmetric cipher support
 *
 * The engine works on physically contiguous buffers and always transforms
 * in place.  Requests are serialized by crypto_engine, so a single
 * descriptor in coherent memory is enough.
 */

#include <crypto/internal/skcipher.h>
#include <linux/bottom_half.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>

#include "spacc.h"

/* Key length accepted by an algorithm, and the matching cipher word bits. */
struct spacc_key_size {
	unsigned int len;
	u32 cipher;
};

struct spacc_cipher_alg {
	struct skcipher_engine_alg engine_alg;
	u32 ctrl;
	u32 mode;
	unsigned int blocksize;		/* block size of the engine */
	const struct spacc_key_size *keysizes;
	unsigned int nkeysizes;
	struct spacc_dev *spacc;
};

struct spacc_cipher_ctx {
	struct spacc_dev *spacc;
	const struct spacc_cipher_alg *alg;
	u8 key[SPACC_MAX_KEYLEN];
	unsigned int keylen;
	u32 keybits;
};

struct spacc_cipher_reqctx {
	bool encrypt;
};

static const struct spacc_key_size aes_keysizes[] = {
	{ 16, SPACC_CIPHER_KEY128 },
	{ 24, SPACC_CIPHER_KEY192 },
	{ 32, SPACC_CIPHER_KEY256 },
};

static const struct spacc_key_size sm4_keysizes[] = {
	{ 16, SPACC_CIPHER_KEY128 },
};

static const struct spacc_key_size des_keysizes[] = {
	{ 8, 0 },
};

static const struct spacc_key_size des3_keysizes[] = {
	{ 24, SPACC_CIPHER_KEY256 },
};

static int spacc_cipher_init(struct crypto_skcipher *skcipher)
{
	struct spacc_cipher_ctx *ctx = crypto_skcipher_ctx(skcipher);
	struct skcipher_alg *base = crypto_skcipher_alg(skcipher);
	struct spacc_cipher_alg *alg;

	alg = container_of(base, struct spacc_cipher_alg, engine_alg.base);
	ctx->spacc = alg->spacc;
	ctx->alg = alg;

	crypto_skcipher_set_reqsize(skcipher, sizeof(struct spacc_cipher_reqctx));

	return 0;
}

static void spacc_cipher_exit(struct crypto_skcipher *skcipher)
{
	struct spacc_cipher_ctx *ctx = crypto_skcipher_ctx(skcipher);

	memzero_explicit(ctx->key, sizeof(ctx->key));
}

static int spacc_setkey(struct crypto_skcipher *tfm, const u8 *key,
			unsigned int keylen)
{
	struct spacc_cipher_ctx *ctx = crypto_skcipher_ctx(tfm);
	const struct spacc_cipher_alg *alg = ctx->alg;
	unsigned int i;

	for (i = 0; i < alg->nkeysizes; i++) {
		if (alg->keysizes[i].len != keylen)
			continue;

		memcpy(ctx->key, key, keylen);
		ctx->keylen = keylen;
		ctx->keybits = alg->keysizes[i].cipher;

		return 0;
	}

	return -EINVAL;
}

/*
 * Transform one request.  crypto_engine has made sure no other request is
 * in flight, so the shared descriptor cannot be overwritten here.
 */
static int spacc_cipher_crypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_cipher_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_cipher_reqctx *rctx = skcipher_request_ctx(req);
	const struct spacc_cipher_alg *alg = ctx->alg;
	struct spacc_dev *spacc = ctx->spacc;
	struct skcipher_walk walk;
	struct spacc_op op = {
		.ctrl = alg->ctrl | SPACC_CTRL_DESC_KEY,
		.cipher = alg->mode | ctx->keybits,
		.key = ctx->key,
		.keylen = ctx->keylen,
	};
	u8 iv[SPACC_MAX_IVLEN];
	unsigned int nbytes;
	int err;

	if (rctx->encrypt)
		op.cipher |= SPACC_CIPHER_ENCRYPT;

	if (alg->mode) {
		memset(iv, 0, sizeof(iv));
		memcpy(iv, req->iv, crypto_skcipher_ivsize(tfm));
		op.iv = iv;
		op.ctrl |= SPACC_CTRL_DESC_IV;
	}

	err = skcipher_walk_virt(&walk, req, false);
	while ((nbytes = walk.nbytes)) {
		void *src = (void *)walk.src.virt.addr;
		void *dst = walk.dst.virt.addr;
		dma_addr_t src_dma, dst_dma;

		/*
		 * The engine moves a whole number of blocks and takes a single
		 * address, so neither a partial block nor a buffer outside the
		 * linear map can be handed to it.  CTR is a stream mode for the
		 * crypto API (block size 1), so a tail is possible there.
		 */
		if ((nbytes & (alg->blocksize - 1)) || is_vmalloc_addr(src) ||
		    is_vmalloc_addr(dst)) {
			err = -EINVAL;
			break;
		}

		/*
		 * Chaining across walked segments needs the hardware to update
		 * the IV.  Whether it writes the descriptor back is not
		 * established, so CTR is limited to a single segment.
		 */
		if ((alg->mode == SPACC_CIPHER_CTR) && nbytes != req->cryptlen) {
			err = -EINVAL;
			break;
		}

		src_dma = dma_map_single(spacc->dev, src, nbytes,
					 DMA_BIDIRECTIONAL);
		if (dma_mapping_error(spacc->dev, src_dma)) {
			err = -ENOMEM;
			break;
		}

		if (dst == src) {
			dst_dma = src_dma;
		} else {
			dst_dma = dma_map_single(spacc->dev, dst, nbytes,
						 DMA_BIDIRECTIONAL);
			if (dma_mapping_error(spacc->dev, dst_dma)) {
				dma_unmap_single(spacc->dev, src_dma, nbytes,
						 DMA_BIDIRECTIONAL);
				err = -ENOMEM;
				break;
			}
		}

		op.src = src_dma;
		op.dst = dst_dma;
		op.len = nbytes;

		err = spacc_execute(spacc, &op);

		if (dst_dma != src_dma)
			dma_unmap_single(spacc->dev, dst_dma, nbytes,
					 DMA_BIDIRECTIONAL);
		dma_unmap_single(spacc->dev, src_dma, nbytes,
				 DMA_BIDIRECTIONAL);

		if (err)
			break;

		if (alg->mode == SPACC_CIPHER_CBC) {
			if (rctx->encrypt)
				memcpy(iv, dst + nbytes - alg->blocksize,
				       alg->blocksize);
			else
				memcpy(iv, src + nbytes - alg->blocksize,
				       alg->blocksize);
		}

		err = skcipher_walk_done(&walk, 0);
		if (err)
			break;
	}

	return err;
}

static int spacc_cipher_one(struct crypto_engine *engine, void *areq)
{
	struct skcipher_request *req = skcipher_request_cast(areq);
	int err = spacc_cipher_crypt(req);

	local_bh_disable();
	crypto_finalize_skcipher_request(engine, req, err);
	local_bh_enable();

	return 0;
}

static int spacc_cipher_crypt_req(struct skcipher_request *req, bool encrypt)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_cipher_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_cipher_reqctx *rctx = skcipher_request_ctx(req);

	rctx->encrypt = encrypt;

	return crypto_transfer_skcipher_request_to_engine(ctx->spacc->engine,
							  req);
}

static int spacc_encrypt(struct skcipher_request *req)
{
	return spacc_cipher_crypt_req(req, true);
}

static int spacc_decrypt(struct skcipher_request *req)
{
	return spacc_cipher_crypt_req(req, false);
}

#define SPACC_ALG(_name, _drv, _apiblk, _hwblk, _ivsize, _min, _max, _ctrl, \
		  _mode, _keys)						\
{									\
	.engine_alg = {							\
		.base = {						\
			.base = {					\
				.cra_name = _name,			\
				.cra_driver_name = _drv,		\
				.cra_priority = 300,			\
				.cra_blocksize = _apiblk,		\
				.cra_ctxsize = sizeof(struct spacc_cipher_ctx), \
				.cra_flags = CRYPTO_ALG_ASYNC,		\
				.cra_module = THIS_MODULE,		\
			},						\
			.min_keysize = _min,				\
			.max_keysize = _max,				\
			.ivsize = _ivsize,				\
			.setkey = spacc_setkey,				\
			.encrypt = spacc_encrypt,			\
			.decrypt = spacc_decrypt,			\
			.init = spacc_cipher_init,			\
			.exit = spacc_cipher_exit,			\
		},							\
		.op = {							\
			.do_one_request = spacc_cipher_one,		\
		},							\
	},								\
	.ctrl = _ctrl,							\
	.mode = _mode,							\
	.blocksize = _hwblk,						\
	.keysizes = _keys,						\
	.nkeysizes = ARRAY_SIZE(_keys),					\
}

static struct spacc_cipher_alg spacc_cipher_algs[] = {
	SPACC_ALG("ecb(aes)", "spacc-ecb-aes", 16, 16, 0, 16, 32,
		  SPACC_CTRL_AES, 0, aes_keysizes),
	SPACC_ALG("cbc(aes)", "spacc-cbc-aes", 16, 16, 16, 16, 32,
		  SPACC_CTRL_AES, SPACC_CIPHER_CBC, aes_keysizes),
	SPACC_ALG("ctr(aes)", "spacc-ctr-aes", 1, 16, 16, 16, 32,
		  SPACC_CTRL_AES, SPACC_CIPHER_CTR, aes_keysizes),
	SPACC_ALG("ecb(sm4)", "spacc-ecb-sm4", 16, 16, 0, 16, 16,
		  SPACC_CTRL_SM4, 0, sm4_keysizes),
	SPACC_ALG("cbc(sm4)", "spacc-cbc-sm4", 16, 16, 16, 16, 16,
		  SPACC_CTRL_SM4, SPACC_CIPHER_CBC, sm4_keysizes),
	SPACC_ALG("ctr(sm4)", "spacc-ctr-sm4", 1, 16, 16, 16, 16,
		  SPACC_CTRL_SM4, SPACC_CIPHER_CTR, sm4_keysizes),
	SPACC_ALG("ecb(des)", "spacc-ecb-des", 8, 8, 0, 8, 8,
		  SPACC_CTRL_DES, 0, des_keysizes),
	SPACC_ALG("cbc(des)", "spacc-cbc-des", 8, 8, 8, 8, 8,
		  SPACC_CTRL_DES, SPACC_CIPHER_CBC, des_keysizes),
	SPACC_ALG("ecb(des3_ede)", "spacc-ecb-des3", 8, 8, 0, 24, 24,
		  SPACC_CTRL_DES, 0, des3_keysizes),
	SPACC_ALG("cbc(des3_ede)", "spacc-cbc-des3", 8, 8, 8, 24, 24,
		  SPACC_CTRL_DES, SPACC_CIPHER_CBC, des3_keysizes),
};

int spacc_cipher_register(struct spacc_dev *spacc)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(spacc_cipher_algs); i++) {
		spacc_cipher_algs[i].spacc = spacc;

		ret = crypto_engine_register_skcipher(&spacc_cipher_algs[i].engine_alg);
		if (ret)
			goto err;
	}

	return 0;

err:
	while (i--)
		crypto_engine_unregister_skcipher(&spacc_cipher_algs[i].engine_alg);

	return ret;
}

void spacc_cipher_unregister(void)
{
	unsigned int i;

	for (i = ARRAY_SIZE(spacc_cipher_algs); i-- > 0;)
		crypto_engine_unregister_skcipher(&spacc_cipher_algs[i].engine_alg);
}
