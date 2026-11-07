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
#include <linux/mm.h>
#include <linux/scatterlist.h>

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
 * The engine takes one contiguous physical range, so the request's own
 * scatterlist is used directly when dma_map_sg() collapses it into a single
 * segment; anything else is gathered through the coherent bounce buffer.
 * crypto_engine guarantees that only one request is in flight, so the shared
 * descriptor and bounce buffer cannot be overwritten here.
 */
static void spacc_ctr_advance(u8 *iv, unsigned int bytes)
{
	unsigned int blocks = bytes / SPACC_BLOCK_16;
	unsigned int i = SPACC_MAX_IVLEN;
	unsigned int carry = blocks;

	/* CTR is a big-endian counter over the whole block. */
	while (carry && i > 0) {
		unsigned int v = iv[--i] + (carry & 0xff);

		iv[i] = v;
		carry = (carry >> 8) + (v >> 8);
	}
}

static int spacc_cipher_crypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct spacc_cipher_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct spacc_cipher_reqctx *rctx = skcipher_request_ctx(req);
	const struct spacc_cipher_alg *alg = ctx->alg;
	struct spacc_dev *spacc = ctx->spacc;
	struct scatterlist *src = req->src;
	struct scatterlist *dst = req->dst;

	if (!dst)
		dst = src;
	unsigned int nents_src = sg_nents(src);
	unsigned int nents_dst = sg_nents(dst);
	unsigned int len = req->cryptlen;
	struct spacc_op op = {
		.ctrl = alg->ctrl | SPACC_CTRL_DESC_KEY,
		.cipher = alg->mode | ctx->keybits,
		.key = ctx->key,
		.keylen = ctx->keylen,
	};
	u8 iv[SPACC_MAX_IVLEN];
	int mapped_src, mapped_dst;
	unsigned int offset, chunk;
	int err = 0;

	if (!len)
		return 0;

	/* The engine moves whole blocks only. */
	if (len & (alg->blocksize - 1))
		return -EINVAL;

	if (rctx->encrypt)
		op.cipher |= SPACC_CIPHER_ENCRYPT;

	if (alg->mode) {
		memset(iv, 0, sizeof(iv));
		memcpy(iv, req->iv, crypto_skcipher_ivsize(tfm));
		op.iv = iv;
		op.ctrl |= SPACC_CTRL_DESC_IV;
	}

	mapped_src = dma_map_sg(spacc->dev, src, nents_src, DMA_BIDIRECTIONAL);
	if (!mapped_src)
		return -ENOMEM;

	if (dst == src) {
		mapped_dst = mapped_src;
	} else {
		mapped_dst = dma_map_sg(spacc->dev, dst, nents_dst,
					DMA_BIDIRECTIONAL);
		if (!mapped_dst) {
			err = -ENOMEM;
			goto unmap_src;
		}
	}

	/* Fast path: one physically contiguous segment for source and destination. */
	if (mapped_src == 1 && mapped_dst == 1 &&
	    sg_dma_len(src) >= len && sg_dma_len(dst) >= len) {
		op.src = sg_dma_address(src);
		op.dst = sg_dma_address(dst);
		op.len = len;

		err = spacc_execute(spacc, &op);
		goto unmap;
	}

	/* Gather path.  The bounce buffer is a multiple of the block size. */
	for (offset = 0; offset < len; offset += chunk) {
		chunk = min(len - offset, (unsigned int)SPACC_BOUNCE_SIZE);

		sg_pcopy_to_buffer(src, nents_src, spacc->bounce, chunk, offset);

		op.src = spacc->bounce_dma;
		op.dst = spacc->bounce_dma;
		op.len = chunk;

		err = spacc_execute(spacc, &op);
		if (err)
			break;

		sg_pcopy_from_buffer(dst, nents_dst, spacc->bounce, chunk,
				     offset);

		if (alg->mode == SPACC_CIPHER_CBC) {
			/*
			 * The next chunk starts from the last ciphertext block,
			 * which is in the bounce buffer after encryption and in
			 * the source list after decryption.
			 */
			if (rctx->encrypt)
				memcpy(iv, spacc->bounce + chunk - alg->blocksize,
				       alg->blocksize);
			else
				sg_pcopy_to_buffer(src, nents_src, iv,
						   alg->blocksize,
						   offset + chunk - alg->blocksize);
		} else if (alg->mode == SPACC_CIPHER_CTR) {
			spacc_ctr_advance(iv, chunk);
		}
	}

unmap:
	if (mapped_dst != mapped_src)
		dma_unmap_sg(spacc->dev, dst, nents_dst, DMA_BIDIRECTIONAL);
unmap_src:
	dma_unmap_sg(spacc->dev, src, nents_src, DMA_BIDIRECTIONAL);

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
