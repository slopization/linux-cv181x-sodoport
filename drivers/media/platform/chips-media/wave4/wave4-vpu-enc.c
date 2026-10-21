// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave4 series multi-standard codec IP - encoder interface
 *
 * Copyright (C) 2021-2023 CHIPS&MEDIA INC
 */

#include <linux/pm_runtime.h>
#include "wave4-helper.h"

#define VPU_ENC_DEV_NAME "C&M Wave4 VPU encoder"
#define VPU_ENC_DRV_NAME "wave4-enc"

static const struct v4l2_frmsize_stepwise enc_frmsize[FMT_TYPES] = {
	[VPU_FMT_TYPE_CODEC] = {
		.min_width = W4_MIN_ENC_PIC_WIDTH,
		.max_width = W4_MAX_ENC_PIC_WIDTH,
		.step_width = W4_ENC_CODEC_STEP_WIDTH,
		.min_height = W4_MIN_ENC_PIC_HEIGHT,
		.max_height = W4_MAX_ENC_PIC_HEIGHT,
		.step_height = W4_ENC_CODEC_STEP_HEIGHT,
	},
	[VPU_FMT_TYPE_RAW] = {
		.min_width = W4_MIN_ENC_PIC_WIDTH,
		.max_width = W4_MAX_ENC_PIC_WIDTH,
		.step_width = W4_ENC_RAW_STEP_WIDTH,
		.min_height = W4_MIN_ENC_PIC_HEIGHT,
		.max_height = W4_MAX_ENC_PIC_HEIGHT,
		.step_height = W4_ENC_RAW_STEP_HEIGHT,
	},
};

static const struct v4l2_frmsize_stepwise enc_packed_frmsize = {
	.min_width = W4_MIN_ENC_PIC_WIDTH,
	.max_width = W4_MAX_ENC_PIC_WIDTH,
	.step_width = W4_ENC_CODEC_STEP_WIDTH,
	.min_height = W4_MIN_ENC_PIC_HEIGHT,
	.max_height = W4_MAX_ENC_PIC_HEIGHT,
	.step_height = W4_ENC_CODEC_STEP_HEIGHT,
};

static const struct vpu_format enc_fmt_list[FMT_TYPES][MAX_FMTS] = {
	[VPU_FMT_TYPE_CODEC] = {
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_HEVC,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_CODEC],
		},
	},
	[VPU_FMT_TYPE_RAW] = {
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUV420,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV12,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV21,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUV420M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV12M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV21M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUV422P,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV16,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV61,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUV422M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV16M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV61M,
			.v4l2_frmsize = &enc_frmsize[VPU_FMT_TYPE_RAW],
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUYV,
			.v4l2_frmsize = &enc_packed_frmsize,
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_UYVY,
			.v4l2_frmsize = &enc_packed_frmsize,
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YVYU,
			.v4l2_frmsize = &enc_packed_frmsize,
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_VYUY,
			.v4l2_frmsize = &enc_packed_frmsize,
		},
	}
};

static bool wave4_vpu_enc_is_packed_422(u32 pixelformat)
{
	return pixelformat == V4L2_PIX_FMT_YUYV ||
	       pixelformat == V4L2_PIX_FMT_UYVY ||
	       pixelformat == V4L2_PIX_FMT_YVYU ||
	       pixelformat == V4L2_PIX_FMT_VYUY;
}

static void wave4_vpu_enc_update_raw_fmt(struct v4l2_pix_format_mplane *pix_mp,
					 unsigned int width, unsigned int height,
					 const struct v4l2_frmsize_stepwise *frmsize)
{
	wave4_update_pix_fmt(pix_mp, VPU_FMT_TYPE_RAW, width, height, frmsize);

	if (!wave4_vpu_enc_is_packed_422(pix_mp->pixelformat))
		return;

	pix_mp->plane_fmt[0].bytesperline = ALIGN(pix_mp->width, 32) * 2;
	pix_mp->plane_fmt[0].sizeimage =
		pix_mp->plane_fmt[0].bytesperline * pix_mp->height;
}

static int switch_state(struct vpu_instance *inst, enum vpu_instance_state state)
{
	switch (state) {
	case VPU_INST_STATE_NONE:
		goto invalid_state_switch;
	case VPU_INST_STATE_OPEN:
		if (inst->state != VPU_INST_STATE_NONE)
			goto invalid_state_switch;
		break;
	case VPU_INST_STATE_INIT_SEQ:
		if (inst->state != VPU_INST_STATE_OPEN && inst->state != VPU_INST_STATE_STOP)
			goto invalid_state_switch;
		break;
	case VPU_INST_STATE_PIC_RUN:
		if (inst->state != VPU_INST_STATE_INIT_SEQ)
			goto invalid_state_switch;
		break;
	case VPU_INST_STATE_STOP:
		break;
	}

	dev_dbg(inst->dev->dev, "Switch state from %s to %s.\n",
		state_to_str(inst->state), state_to_str(state));
	inst->state = state;
	return 0;

invalid_state_switch:
	WARN(1, "Invalid state switch from %s to %s.\n",
	     state_to_str(inst->state), state_to_str(state));
	return -EINVAL;
}

static void wave4_vpu_enc_src_meta_reset(struct enc_info *p_enc_info)
{
	p_enc_info->src_meta_head = 0;
	p_enc_info->src_meta_count = 0;
}

static int wave4_vpu_enc_src_meta_push(struct vpu_instance *inst,
				       const struct vb2_v4l2_buffer *src_buf,
				       const struct vb2_v4l2_buffer *dst_buf)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	struct wave4_enc_src_meta *meta;
	u32 tail;

	if (WARN_ON(p_enc_info->src_meta_count >= ARRAY_SIZE(p_enc_info->src_meta_fifo)))
		return -ENOSPC;

	tail = (p_enc_info->src_meta_head + p_enc_info->src_meta_count) %
		ARRAY_SIZE(p_enc_info->src_meta_fifo);
	meta = &p_enc_info->src_meta_fifo[tail];
	meta->idx = src_buf->vb2_buf.index;
	meta->dst_idx = dst_buf->vb2_buf.index;
	meta->timestamp = src_buf->vb2_buf.timestamp;
	meta->flags = src_buf->flags &
		(V4L2_BUF_FLAG_TIMECODE | V4L2_BUF_FLAG_TSTAMP_SRC_MASK);
	if (meta->flags & V4L2_BUF_FLAG_TIMECODE)
		meta->timecode = src_buf->timecode;

	p_enc_info->src_meta_count++;

	return 0;
}

static bool wave4_vpu_enc_src_meta_pop(struct enc_info *p_enc_info,
				       struct wave4_enc_src_meta *meta)
{
	if (!p_enc_info->src_meta_count)
		return false;

	*meta = p_enc_info->src_meta_fifo[p_enc_info->src_meta_head];
	p_enc_info->src_meta_head = (p_enc_info->src_meta_head + 1) %
		ARRAY_SIZE(p_enc_info->src_meta_fifo);
	p_enc_info->src_meta_count--;

	return true;
}

static bool wave4_vpu_enc_src_meta_drop_last(struct enc_info *p_enc_info,
					     struct wave4_enc_src_meta *meta)
{
	u32 tail;

	if (!p_enc_info->src_meta_count)
		return false;

	tail = (p_enc_info->src_meta_head + p_enc_info->src_meta_count - 1) %
		ARRAY_SIZE(p_enc_info->src_meta_fifo);
	if (meta)
		*meta = p_enc_info->src_meta_fifo[tail];
	p_enc_info->src_meta_count--;

	return true;
}

static void wave4_vpu_enc_complete_src_meta(struct vpu_instance *inst,
					    const struct wave4_enc_src_meta *meta,
					    enum vb2_buffer_state state)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct vb2_buffer *vb;
	struct vb2_v4l2_buffer *src_buf;

	vb = vb2_get_buffer(v4l2_m2m_get_src_vq(m2m_ctx), meta->idx);
	if (!vb)
		return;

	if (vb->state != VB2_BUF_STATE_ACTIVE && vb->state != VB2_BUF_STATE_QUEUED)
		return;

	src_buf = to_vb2_v4l2_buffer(vb);
	if (state == VB2_BUF_STATE_DONE)
		inst->timestamp = meta->timestamp;
	v4l2_m2m_buf_done(src_buf, state);
}

static struct vb2_v4l2_buffer *
wave4_vpu_enc_get_dst_buf_by_idx(struct vpu_instance *inst, s32 idx)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct vb2_buffer *vb;

	vb = vb2_get_buffer(v4l2_m2m_get_dst_vq(m2m_ctx), idx);
	if (!vb)
		return NULL;

	if (vb->state != VB2_BUF_STATE_ACTIVE && vb->state != VB2_BUF_STATE_QUEUED)
		return NULL;

	return to_vb2_v4l2_buffer(vb);
}

static void wave4_vpu_enc_complete_dst_meta(struct vpu_instance *inst,
					    const struct wave4_enc_src_meta *meta,
					    enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *dst_buf;

	dst_buf = wave4_vpu_enc_get_dst_buf_by_idx(inst, meta->dst_idx);
	if (!dst_buf)
		return;

	if (state != VB2_BUF_STATE_DONE)
		vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
	dst_buf->field = V4L2_FIELD_NONE;
	v4l2_m2m_buf_done(dst_buf, state);
}

static void wave4_vpu_enc_flush_src_meta(struct vpu_instance *inst,
					 enum vb2_buffer_state state)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	struct wave4_enc_src_meta meta;

	while (wave4_vpu_enc_src_meta_pop(p_enc_info, &meta)) {
		wave4_vpu_enc_complete_src_meta(inst, &meta, state);
		wave4_vpu_enc_complete_dst_meta(inst, &meta, state);
	}
}

static int start_encode(struct vpu_instance *inst, u32 *fail_res)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	int ret;
	struct vb2_v4l2_buffer *src_buf;
	struct vb2_v4l2_buffer *dst_buf;
	struct wave4_enc_src_meta dropped_meta;
	struct frame_buffer frame_buf;
	struct enc_param pic_param;
	const struct v4l2_format_info *info;
	u32 stride = inst->src_fmt.plane_fmt[0].bytesperline;
	u32 luma_size = 0;
	u32 chroma_size = 0;
	bool src_meta_armed = false;
	bool force_key_frame = false;

	memset(&pic_param, 0, sizeof(struct enc_param));
	memset(&frame_buf, 0, sizeof(struct frame_buffer));

	info = v4l2_format_info(inst->src_fmt.pixelformat);
	if (!info)
		return -EINVAL;

	if (wave4_vpu_enc_is_packed_422(inst->src_fmt.pixelformat)) {
		luma_size = inst->src_fmt.plane_fmt[0].sizeimage;
	} else if (info->mem_planes == 1) {
		luma_size = stride * inst->dst_fmt.height;
		chroma_size = luma_size / (info->hdiv * info->vdiv);
	} else {
		luma_size = inst->src_fmt.plane_fmt[0].sizeimage;
		chroma_size = inst->src_fmt.plane_fmt[1].sizeimage;
	}

	dst_buf = v4l2_m2m_next_dst_buf(m2m_ctx);
	if (!dst_buf) {
		dev_dbg(inst->dev->dev, "%s: No destination buffer found\n", __func__);
		return -EAGAIN;
	}

	pic_param.pic_stream_buffer_addr =
		vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	pic_param.pic_stream_buffer_size =
		vb2_plane_size(&dst_buf->vb2_buf, 0);

	src_buf = v4l2_m2m_next_src_buf(m2m_ctx);
	if (!src_buf) {
		dev_dbg(inst->dev->dev, "%s: No source buffer found\n", __func__);
		if (m2m_ctx->is_draining) {
			pic_param.src_end_flag = 1;
		} else {
			return -EAGAIN;
		}
	} else {
		if (inst->src_fmt.num_planes == 1) {
			frame_buf.buf_y =
				vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
			if (wave4_vpu_enc_is_packed_422(inst->src_fmt.pixelformat)) {
				frame_buf.buf_cb = 0;
				frame_buf.buf_cr = 0;
			} else {
				frame_buf.buf_cb = frame_buf.buf_y + luma_size;
				frame_buf.buf_cr = frame_buf.buf_cb + chroma_size;
			}
		} else if (inst->src_fmt.num_planes == 2) {
			frame_buf.buf_y =
				vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
			frame_buf.buf_cb =
				vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 1);
			frame_buf.buf_cr = frame_buf.buf_cb + chroma_size;
		} else if (inst->src_fmt.num_planes == 3) {
			frame_buf.buf_y =
				vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
			frame_buf.buf_cb =
				vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 1);
			frame_buf.buf_cr =
				vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 2);
		}

		frame_buf.stride = stride;
		pic_param.src_idx = src_buf->vb2_buf.index;
		ret = wave4_vpu_enc_src_meta_push(inst, src_buf, dst_buf);
		if (ret) {
			dev_err(inst->dev->dev,
				"%s: source metadata FIFO overflow, stopping instance\n",
				__func__);
			src_buf = v4l2_m2m_src_buf_remove_by_idx(m2m_ctx, src_buf->vb2_buf.index);
			dst_buf = v4l2_m2m_dst_buf_remove_by_idx(m2m_ctx, dst_buf->vb2_buf.index);
			if (src_buf)
				v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
			if (dst_buf) {
				vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
				dst_buf->field = V4L2_FIELD_NONE;
				v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
			}
			switch_state(inst, VPU_INST_STATE_STOP);
			return ret;
		}

		src_meta_armed = true;
		src_buf = v4l2_m2m_src_buf_remove_by_idx(m2m_ctx, pic_param.src_idx);
		dst_buf = v4l2_m2m_dst_buf_remove_by_idx(m2m_ctx, dst_buf->vb2_buf.index);
		if (!src_buf || !dst_buf) {
			wave4_vpu_enc_src_meta_drop_last(p_enc_info, NULL);
			if (src_buf)
				v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
			if (dst_buf) {
				vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
				dst_buf->field = V4L2_FIELD_NONE;
				v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
			}
			dev_warn_ratelimited(inst->dev->dev,
					     "%s: failed to remove prepared source/destination buffers src=%p dst=%p idx=%u\n",
					     __func__, src_buf, dst_buf, pic_param.src_idx);
			switch_state(inst, VPU_INST_STATE_STOP);
			return -EINVAL;
		}
	}

	pic_param.source_frame = &frame_buf;
	pic_param.code_option.implicit_header_encode = 1;
	pic_param.code_option.encode_aud = inst->encode_aud;
	if (!pic_param.src_end_flag)
		force_key_frame = atomic_xchg(&inst->force_key_frame, 0);
	if (force_key_frame) {
		pic_param.force_pic_type_enable = 1;
		pic_param.force_pic_type = 3;
	}
	ret = wave4_vpu_enc_start_one_frame(inst, &pic_param, fail_res);
	if (ret) {
		if (force_key_frame)
			atomic_set(&inst->force_key_frame, 1);
		if (src_meta_armed &&
		    wave4_vpu_enc_src_meta_drop_last(p_enc_info, &dropped_meta) &&
		    dropped_meta.idx != src_buf->vb2_buf.index) {
			dev_warn_ratelimited(inst->dev->dev,
					     "%s: source metadata rollback mismatch drop=%d expected=%d\n",
					     __func__, dropped_meta.idx, src_buf->vb2_buf.index);
			wave4_vpu_enc_src_meta_reset(p_enc_info);
		}

		dev_dbg(inst->dev->dev, "%s: wave4_vpu_enc_start_one_frame fail: %d\n",
			__func__, ret);

		if (*fail_res == W4_SYSERR_QUEUEING_FAIL)
			ret = -EINVAL;

		switch_state(inst, VPU_INST_STATE_STOP);
		if (src_meta_armed) {
			dst_buf->vb2_buf.timestamp = src_buf->vb2_buf.timestamp;
			v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
			vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
			dst_buf->field = V4L2_FIELD_NONE;
			v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
		} else {
			dst_buf = v4l2_m2m_dst_buf_remove(m2m_ctx);
			if (dst_buf) {
				dst_buf->vb2_buf.timestamp = 0;
				vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
				dst_buf->field = V4L2_FIELD_NONE;
				v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
			}
		}
	} else {
		dev_dbg(inst->dev->dev, "%s: wave4_vpu_enc_start_one_frame success\n",
			__func__);
	}

	return ret;
}

static void wave4_vpu_enc_put_async_pm(struct vpu_instance *inst)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;

	/*
	 * Async device_run keeps one runtime-PM ref until completion callback.
	 * Use xchg so stop_streaming and IRQ completion can safely race.
	 */
	if (xchg(&p_enc_info->async_pm_ref_held, 0))
		pm_runtime_put_autosuspend(inst->dev->dev);
}

static void wave4_vpu_enc_finish_encode(struct vpu_instance *inst)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	struct v4l2_timecode copied_timecode = { 0 };
	struct wave4_enc_src_meta src_meta;
	int ret;
	u32 copied_flags = 0;
	struct enc_output_info enc_output_info;
	struct vb2_v4l2_buffer *dst_buf = NULL;
	bool output_eos;
	bool have_src_meta = false;

	/*
	 * Crash-guard: streamoff can race with delayed ENC_PIC completions.
	 * Once stopping starts, never touch ready queues from this callback.
	 * Retire tracked in-flight source buffers and exit quietly.
	 */
	if (p_enc_info->stop_pending || inst->state != VPU_INST_STATE_PIC_RUN ||
	    !wave4_vpu_both_queues_are_streaming(inst)) {
		wave4_vpu_enc_flush_src_meta(inst, VB2_BUF_STATE_ERROR);
		wave4_vpu_enc_put_async_pm(inst);
		return;
	}

	ret = wave4_vpu_enc_get_output_info(inst, &enc_output_info);
	if (ret) {
		/*
		 * Crash-guard: never return here without finishing the mem2mem job.
		 * Returning early leaves a queued job active and userspace can block
		 * forever in poll/streamoff (observed as unkillable D-state v4l2-ctl).
		 */
		dev_warn_ratelimited(inst->dev->dev,
				     "%s: vpu_enc_get_output_info fail: %d reason:%u warn:%u; stopping instance\n",
				     __func__, ret, enc_output_info.error_reason,
				     enc_output_info.warn_info);
		/*
		 * Crash-guard: don't touch source/destination buffer lists here.
		 * On this error path we can get repeated callbacks without a valid
		 * report queue entry. Complete tracked in-flight source buffers and
		 * one pending destination buffer.
		 */
		wave4_vpu_enc_flush_src_meta(inst, VB2_BUF_STATE_ERROR);
		dst_buf = v4l2_m2m_dst_buf_remove(m2m_ctx);
		if (dst_buf) {
			vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
			dst_buf->field = V4L2_FIELD_NONE;
			v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
		}
		switch_state(inst, VPU_INST_STATE_STOP);
		wave4_vpu_enc_put_async_pm(inst);
		v4l2_m2m_job_finish(inst->v4l2_m2m_dev, m2m_ctx);
		return;
	}

	dev_dbg(inst->dev->dev,
		"%s: pic_type %i recon_idx %i src_idx %i pic_byte %u pts %llu\n",
		__func__,  enc_output_info.pic_type, enc_output_info.recon_frame_index,
		enc_output_info.enc_src_idx, enc_output_info.enc_pic_byte, enc_output_info.pts);

	output_eos = enc_output_info.recon_frame_index == RECON_IDX_FLAG_ENC_END;
	if (!output_eos) {
		if (!wave4_vpu_enc_src_meta_pop(p_enc_info, &src_meta)) {
			dev_warn_ratelimited(inst->dev->dev,
					     "%s: source metadata FIFO empty for src_idx=%d recon_idx=%d; stopping instance\n",
					     __func__, enc_output_info.enc_src_idx,
					     enc_output_info.recon_frame_index);
			dst_buf = v4l2_m2m_dst_buf_remove(m2m_ctx);
			if (dst_buf) {
				vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
				dst_buf->field = V4L2_FIELD_NONE;
				v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
			}
			switch_state(inst, VPU_INST_STATE_STOP);
			wave4_vpu_enc_put_async_pm(inst);
			v4l2_m2m_job_finish(inst->v4l2_m2m_dev, m2m_ctx);
			return;
		}

		have_src_meta = true;
		if (enc_output_info.enc_src_idx != src_meta.idx) {
			dev_warn_ratelimited(inst->dev->dev,
					     "%s: source metadata FIFO mismatch fw_src_idx=%d expected_src_idx=%d recon_idx=%d; stopping instance\n",
					     __func__, enc_output_info.enc_src_idx,
					     src_meta.idx, enc_output_info.recon_frame_index);
			wave4_vpu_enc_complete_src_meta(inst, &src_meta, VB2_BUF_STATE_ERROR);
			wave4_vpu_enc_complete_dst_meta(inst, &src_meta, VB2_BUF_STATE_ERROR);
			wave4_vpu_enc_flush_src_meta(inst, VB2_BUF_STATE_ERROR);
			switch_state(inst, VPU_INST_STATE_STOP);
			wave4_vpu_enc_put_async_pm(inst);
			v4l2_m2m_job_finish(inst->v4l2_m2m_dev, m2m_ctx);
			return;
		}

		copied_flags = src_meta.flags;
		if (copied_flags & V4L2_BUF_FLAG_TIMECODE)
			copied_timecode = src_meta.timecode;
		wave4_vpu_enc_complete_src_meta(inst, &src_meta, VB2_BUF_STATE_DONE);

		dst_buf = wave4_vpu_enc_get_dst_buf_by_idx(inst, src_meta.dst_idx);
		if (!dst_buf) {
			dev_warn_ratelimited(inst->dev->dev,
					     "%s: destination buffer lookup failed for dst_idx=%d src_idx=%d; stopping instance\n",
					     __func__, src_meta.dst_idx, src_meta.idx);
			wave4_vpu_enc_flush_src_meta(inst, VB2_BUF_STATE_ERROR);
			switch_state(inst, VPU_INST_STATE_STOP);
			wave4_vpu_enc_put_async_pm(inst);
			v4l2_m2m_job_finish(inst->v4l2_m2m_dev, m2m_ctx);
			return;
		}
	}

	if (output_eos) {
		static const struct v4l2_event vpu_event_eos = {
			.type = V4L2_EVENT_EOS
		};

		dst_buf = v4l2_m2m_dst_buf_remove(m2m_ctx);
		if (!WARN_ON(!dst_buf)) {
			vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
			dst_buf->field = V4L2_FIELD_NONE;
			v4l2_m2m_last_buffer_done(m2m_ctx, dst_buf);
		}

		v4l2_event_queue_fh(&inst->v4l2_fh, &vpu_event_eos);

		/*
		 * Drop async ownership before job_finish() wakes the next m2m job.
		 * Otherwise the next device_run can re-arm ownership while this
		 * completion path still clears the previous one, losing one ENC_PIC
		 * completion and stalling poll/select users.
		 */
		wave4_vpu_enc_put_async_pm(inst);
		v4l2_m2m_job_finish(inst->v4l2_m2m_dev, m2m_ctx);
	} else {
		if (!dst_buf) {
			dev_dbg(inst->dev->dev, "No bitstream buffer.");
			wave4_vpu_enc_put_async_pm(inst);
			v4l2_m2m_job_finish(inst->v4l2_m2m_dev, m2m_ctx);
			return;
		}

		if (!have_src_meta) {
			copied_flags = 0;
			copied_timecode = (struct v4l2_timecode){ 0 };
		}
		vb2_set_plane_payload(&dst_buf->vb2_buf, 0, enc_output_info.bitstream_size);
		dst_buf->vb2_buf.timestamp = inst->timestamp;
		dst_buf->field = V4L2_FIELD_NONE;
		dst_buf->flags &= ~(V4L2_BUF_FLAG_TIMECODE | V4L2_BUF_FLAG_TSTAMP_SRC_MASK |
				    V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
				    V4L2_BUF_FLAG_BFRAME);
		dst_buf->flags |= copied_flags;
		if (copied_flags & V4L2_BUF_FLAG_TIMECODE)
			dst_buf->timecode = copied_timecode;
		if (enc_output_info.pic_type == PIC_TYPE_I) {
			dst_buf->flags |= V4L2_BUF_FLAG_KEYFRAME;
		} else if (enc_output_info.pic_type == PIC_TYPE_P) {
			dst_buf->flags |= V4L2_BUF_FLAG_PFRAME;
		} else if (enc_output_info.pic_type == PIC_TYPE_B) {
			dst_buf->flags |= V4L2_BUF_FLAG_BFRAME;
		}

		v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);

		dev_dbg(inst->dev->dev, "%s: frame_cycle %8u\n",
			__func__, enc_output_info.frame_cycle);

		/*
		 * Keep async ownership transitions ordered before scheduling the
		 * next mem2mem job to avoid races with the IRQ stale-filter.
		 */
		wave4_vpu_enc_put_async_pm(inst);
		v4l2_m2m_job_finish(inst->v4l2_m2m_dev, m2m_ctx);
	}

	return;
}

static int wave4_vpu_enc_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, VPU_ENC_DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, VPU_ENC_DRV_NAME, sizeof(cap->card));

	return 0;
}

static int wave4_vpu_enc_enum_framesizes(struct file *f, void *fh, struct v4l2_frmsizeenum *fsize)
{
	const struct vpu_format *vpu_fmt;

	if (fsize->index)
		return -EINVAL;

	vpu_fmt = wave4_find_vpu_fmt(fsize->pixel_format, enc_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt) {
		vpu_fmt = wave4_find_vpu_fmt(fsize->pixel_format, enc_fmt_list[VPU_FMT_TYPE_RAW]);
		if (!vpu_fmt)
			return -EINVAL;
	}

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = enc_frmsize[VPU_FMT_TYPE_CODEC];

	return 0;
}

static int wave4_vpu_enc_enum_fmt_cap(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	const struct vpu_format *vpu_fmt;

	dev_dbg(inst->dev->dev, "%s: index: %u\n", __func__, f->index);

	vpu_fmt = wave4_find_vpu_fmt_by_idx(f->index, enc_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt)
		return -EINVAL;

	f->pixelformat = vpu_fmt->v4l2_pix_fmt;
	f->flags = 0;

	return 0;
}

static int wave4_vpu_enc_try_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	const struct v4l2_frmsize_stepwise *frmsize;
	const struct vpu_format *vpu_fmt;
	int width, height;

	dev_dbg(inst->dev->dev, "%s: fourcc: %u width: %u height: %u num_planes: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.field);

	vpu_fmt = wave4_find_vpu_fmt(f->fmt.pix_mp.pixelformat, enc_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt) {
		width = inst->dst_fmt.width;
		height = inst->dst_fmt.height;
		f->fmt.pix_mp.pixelformat = inst->dst_fmt.pixelformat;
		frmsize = &enc_frmsize[VPU_FMT_TYPE_CODEC];
	} else {
		width = f->fmt.pix_mp.width;
		height = f->fmt.pix_mp.height;
		f->fmt.pix_mp.pixelformat = vpu_fmt->v4l2_pix_fmt;
		frmsize = vpu_fmt->v4l2_frmsize;
	}

	wave4_update_pix_fmt(&f->fmt.pix_mp, VPU_FMT_TYPE_CODEC,
			     width, height, frmsize);
	f->fmt.pix_mp.colorspace = inst->colorspace;
	f->fmt.pix_mp.ycbcr_enc = inst->ycbcr_enc;
	f->fmt.pix_mp.quantization = inst->quantization;
	f->fmt.pix_mp.xfer_func = inst->xfer_func;

	return 0;
}

static int wave4_vpu_enc_s_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	int i, ret;

	dev_dbg(inst->dev->dev, "%s: fourcc: %u width: %u height: %u num_planes: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.field);

	ret = wave4_vpu_enc_try_fmt_cap(file, fh, f);
	if (ret)
		return ret;

	inst->std = wave4_to_vpu_std(f->fmt.pix_mp.pixelformat, inst->type);
	if (inst->std == STD_UNKNOWN) {
		dev_warn(inst->dev->dev, "unsupported pixelformat: %.4s\n",
			 (char *)&f->fmt.pix_mp.pixelformat);
		return -EINVAL;
	}

	inst->dst_fmt.width = f->fmt.pix_mp.width;
	inst->dst_fmt.height = f->fmt.pix_mp.height;
	inst->dst_fmt.pixelformat = f->fmt.pix_mp.pixelformat;
	inst->dst_fmt.field = f->fmt.pix_mp.field;
	inst->dst_fmt.flags = f->fmt.pix_mp.flags;
	inst->dst_fmt.num_planes = f->fmt.pix_mp.num_planes;
	for (i = 0; i < inst->dst_fmt.num_planes; i++) {
		inst->dst_fmt.plane_fmt[i].bytesperline = f->fmt.pix_mp.plane_fmt[i].bytesperline;
		inst->dst_fmt.plane_fmt[i].sizeimage = f->fmt.pix_mp.plane_fmt[i].sizeimage;
	}

	return 0;
}

static int wave4_vpu_enc_g_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	int i;

	f->fmt.pix_mp.width = inst->dst_fmt.width;
	f->fmt.pix_mp.height = inst->dst_fmt.height;
	f->fmt.pix_mp.pixelformat = inst->dst_fmt.pixelformat;
	f->fmt.pix_mp.field = inst->dst_fmt.field;
	f->fmt.pix_mp.flags = inst->dst_fmt.flags;
	f->fmt.pix_mp.num_planes = inst->dst_fmt.num_planes;
	for (i = 0; i < f->fmt.pix_mp.num_planes; i++) {
		f->fmt.pix_mp.plane_fmt[i].bytesperline = inst->dst_fmt.plane_fmt[i].bytesperline;
		f->fmt.pix_mp.plane_fmt[i].sizeimage = inst->dst_fmt.plane_fmt[i].sizeimage;
	}

	f->fmt.pix_mp.colorspace = inst->colorspace;
	f->fmt.pix_mp.ycbcr_enc = inst->ycbcr_enc;
	f->fmt.pix_mp.quantization = inst->quantization;
	f->fmt.pix_mp.xfer_func = inst->xfer_func;

	return 0;
}

static int wave4_vpu_enc_enum_fmt_out(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	const struct vpu_format *vpu_fmt;

	dev_dbg(inst->dev->dev, "%s: index: %u\n", __func__, f->index);

	vpu_fmt = wave4_find_vpu_fmt_by_idx(f->index, enc_fmt_list[VPU_FMT_TYPE_RAW]);
	if (!vpu_fmt)
		return -EINVAL;

	f->pixelformat = vpu_fmt->v4l2_pix_fmt;
	f->flags = 0;

	return 0;
}

static int wave4_vpu_enc_try_fmt_out(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	const struct v4l2_frmsize_stepwise *frmsize;
	const struct vpu_format *vpu_fmt;
	int width, height;

	dev_dbg(inst->dev->dev, "%s: fourcc: %u width: %u height: %u num_planes: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.field);

	vpu_fmt = wave4_find_vpu_fmt(f->fmt.pix_mp.pixelformat, enc_fmt_list[VPU_FMT_TYPE_RAW]);
	if (!vpu_fmt) {
		width = inst->src_fmt.width;
		height = inst->src_fmt.height;
		f->fmt.pix_mp.pixelformat = inst->src_fmt.pixelformat;
		vpu_fmt = wave4_find_vpu_fmt(f->fmt.pix_mp.pixelformat,
					     enc_fmt_list[VPU_FMT_TYPE_RAW]);
		frmsize = vpu_fmt ? vpu_fmt->v4l2_frmsize :
				     &enc_frmsize[VPU_FMT_TYPE_RAW];
	} else {
		width = f->fmt.pix_mp.width;
		height = f->fmt.pix_mp.height;
		f->fmt.pix_mp.pixelformat = vpu_fmt->v4l2_pix_fmt;
		frmsize = vpu_fmt->v4l2_frmsize;
	}

	wave4_vpu_enc_update_raw_fmt(&f->fmt.pix_mp, width, height, frmsize);
	return 0;
}

static int wave4_vpu_enc_s_fmt_out(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	const struct vpu_format *vpu_fmt;
	const struct v4l2_format_info *info;
	int i, ret;

	dev_dbg(inst->dev->dev, "%s: fourcc: %u width: %u height: %u num_planes: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.field);

	ret = wave4_vpu_enc_try_fmt_out(file, fh, f);
	if (ret)
		return ret;

	inst->src_fmt.width = f->fmt.pix_mp.width;
	inst->src_fmt.height = f->fmt.pix_mp.height;
	inst->src_fmt.pixelformat = f->fmt.pix_mp.pixelformat;
	inst->src_fmt.field = f->fmt.pix_mp.field;
	inst->src_fmt.flags = f->fmt.pix_mp.flags;
	inst->src_fmt.num_planes = f->fmt.pix_mp.num_planes;
	for (i = 0; i < inst->src_fmt.num_planes; i++) {
		inst->src_fmt.plane_fmt[i].bytesperline = f->fmt.pix_mp.plane_fmt[i].bytesperline;
		inst->src_fmt.plane_fmt[i].sizeimage = f->fmt.pix_mp.plane_fmt[i].sizeimage;
	}

	info = v4l2_format_info(inst->src_fmt.pixelformat);
	if (!info)
		return -EINVAL;

	inst->cbcr_interleave = info->comp_planes == 2;

	switch (inst->src_fmt.pixelformat) {
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV21M:
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_NV61M:
		inst->nv21 = true;
		break;
	default:
		inst->nv21 = false;
	}

	inst->colorspace = f->fmt.pix_mp.colorspace;
	inst->ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
	inst->quantization = f->fmt.pix_mp.quantization;
	inst->xfer_func = f->fmt.pix_mp.xfer_func;

	vpu_fmt = wave4_find_vpu_fmt(inst->dst_fmt.pixelformat, enc_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt)
		return -EINVAL;

	wave4_update_pix_fmt(&inst->dst_fmt, VPU_FMT_TYPE_CODEC,
			     f->fmt.pix_mp.width, f->fmt.pix_mp.height,
			     vpu_fmt->v4l2_frmsize);
	inst->conf_win.width = inst->dst_fmt.width;
	inst->conf_win.height = inst->dst_fmt.height;

	return 0;
}

static int wave4_vpu_enc_g_selection(struct file *file, void *fh, struct v4l2_selection *s)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);

	dev_dbg(inst->dev->dev, "%s: type: %u | target: %u\n", __func__, s->type, s->target);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;
	switch (s->target) {
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = inst->dst_fmt.width;
		s->r.height = inst->dst_fmt.height;
		break;
	case V4L2_SEL_TGT_CROP:
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = inst->conf_win.width;
		s->r.height = inst->conf_win.height;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int wave4_vpu_enc_s_selection(struct file *file, void *fh, struct v4l2_selection *s)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	if (s->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	dev_dbg(inst->dev->dev, "%s: V4L2_SEL_TGT_CROP width: %u | height: %u\n",
		__func__, s->r.width, s->r.height);

	s->r.left = 0;
	s->r.top = 0;
	s->r.width = min(s->r.width, inst->dst_fmt.width);
	s->r.height = min(s->r.height, inst->dst_fmt.height);

	inst->conf_win = s->r;

	return 0;
}

static int wave4_vpu_enc_encoder_cmd(struct file *file, void *fh, struct v4l2_encoder_cmd *ec)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, fh, ec);
	if (ret)
		return ret;

	if (!wave4_vpu_both_queues_are_streaming(inst))
		return 0;

	switch (ec->cmd) {
	case V4L2_ENC_CMD_STOP:
		if (m2m_ctx->is_draining)
			return -EBUSY;

		if (m2m_ctx->has_stopped)
			return 0;

		m2m_ctx->last_src_buf = v4l2_m2m_last_src_buf(m2m_ctx);
		m2m_ctx->is_draining = true;

		v4l2_m2m_try_schedule(m2m_ctx);
		break;
	case V4L2_ENC_CMD_START:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int wave4_vpu_enc_g_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);

	dev_dbg(inst->dev->dev, "%s: type: %u\n", __func__, a->type);

	if (a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT && a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.output.timeperframe.numerator = 1;
	a->parm.output.timeperframe.denominator = inst->frame_rate;

	dev_dbg(inst->dev->dev, "%s: numerator: %u | denominator: %u\n",
		__func__, a->parm.output.timeperframe.numerator,
		a->parm.output.timeperframe.denominator);

	return 0;
}

static int wave4_vpu_enc_s_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct vpu_instance *inst = file_to_vpu_inst(file);

	dev_dbg(inst->dev->dev, "%s: type: %u\n", __func__, a->type);

	if (a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT && a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	if (a->parm.output.timeperframe.denominator && a->parm.output.timeperframe.numerator) {
		inst->frame_rate = a->parm.output.timeperframe.denominator /
				   a->parm.output.timeperframe.numerator;
	} else {
		a->parm.output.timeperframe.numerator = 1;
		a->parm.output.timeperframe.denominator = inst->frame_rate;
	}

	dev_dbg(inst->dev->dev, "%s: numerator: %u | denominator: %u\n",
		__func__, a->parm.output.timeperframe.numerator,
		a->parm.output.timeperframe.denominator);

	return 0;
}

/*
 * Keep native operation in M2M mplane mode, but accept legacy VIDEO_OUTPUT
 * single-plane buffer type in setup/export ioctls used by user-space DMABUF
 * helper paths (e.g. v4l2-compliance expbuf helper queue).
 */
static bool wave4_vpu_enc_is_compat_splane_out(u32 type)
{
	return type == V4L2_BUF_TYPE_VIDEO_OUTPUT;
}

static int wave4_vpu_enc_ioctl_reqbufs(struct file *file, void *fh,
				       struct v4l2_requestbuffers *rb)
{
	struct v4l2_requestbuffers req = *rb;
	bool compat_splane_out = wave4_vpu_enc_is_compat_splane_out(req.type);
	int ret;

	if (compat_splane_out)
		req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	ret = v4l2_m2m_ioctl_reqbufs(file, fh, &req);
	if (ret)
		return ret;

	*rb = req;
	if (compat_splane_out)
		rb->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	return 0;
}

static int wave4_vpu_enc_ioctl_create_bufs(struct file *file, void *fh,
					   struct v4l2_create_buffers *cb)
{
	struct v4l2_create_buffers create = *cb;
	bool compat_splane_out = wave4_vpu_enc_is_compat_splane_out(create.format.type);
	int ret;

	if (compat_splane_out)
		create.format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	ret = v4l2_m2m_ioctl_create_bufs(file, fh, &create);
	if (ret)
		return ret;

	*cb = create;
	if (compat_splane_out)
		cb->format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	return 0;
}

static int wave4_vpu_enc_ioctl_querybuf(struct file *file, void *fh,
					struct v4l2_buffer *b)
{
	struct v4l2_buffer mb = *b;
	struct v4l2_plane plane = { 0 };
	int ret;

	if (b->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return v4l2_m2m_ioctl_querybuf(file, fh, b);

	mb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	mb.length = 1;
	mb.m.planes = &plane;

	ret = v4l2_m2m_ioctl_querybuf(file, fh, &mb);
	if (ret)
		return ret;

	b->index = mb.index;
	b->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	b->bytesused = plane.bytesused;
	b->flags = mb.flags;
	b->field = mb.field;
	b->timestamp = mb.timestamp;
	b->timecode = mb.timecode;
	b->sequence = mb.sequence;
	b->memory = mb.memory;
	b->length = plane.length;
	b->reserved2 = mb.reserved2;
	b->request_fd = mb.request_fd;
	if (mb.memory == V4L2_MEMORY_MMAP)
		b->m.offset = plane.m.mem_offset;

	return 0;
}

static int wave4_vpu_enc_ioctl_expbuf(struct file *file, void *fh,
				      struct v4l2_exportbuffer *eb)
{
	struct v4l2_exportbuffer exp = *eb;
	bool compat_splane_out = wave4_vpu_enc_is_compat_splane_out(exp.type);
	int ret;

	if (compat_splane_out)
		exp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	ret = v4l2_m2m_ioctl_expbuf(file, fh, &exp);
	if (ret)
		return ret;

	*eb = exp;
	if (compat_splane_out)
		eb->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	return 0;
}

static const struct v4l2_ioctl_ops wave4_vpu_enc_ioctl_ops = {
	.vidioc_querycap = wave4_vpu_enc_querycap,
	.vidioc_enum_framesizes = wave4_vpu_enc_enum_framesizes,

	.vidioc_enum_fmt_vid_cap	= wave4_vpu_enc_enum_fmt_cap,
	.vidioc_s_fmt_vid_cap_mplane = wave4_vpu_enc_s_fmt_cap,
	.vidioc_g_fmt_vid_cap_mplane = wave4_vpu_enc_g_fmt_cap,
	.vidioc_try_fmt_vid_cap_mplane = wave4_vpu_enc_try_fmt_cap,

	.vidioc_enum_fmt_vid_out	= wave4_vpu_enc_enum_fmt_out,
	.vidioc_s_fmt_vid_out_mplane = wave4_vpu_enc_s_fmt_out,
	.vidioc_g_fmt_vid_out_mplane = wave4_vpu_g_fmt_out,
	.vidioc_try_fmt_vid_out_mplane = wave4_vpu_enc_try_fmt_out,

	.vidioc_g_selection = wave4_vpu_enc_g_selection,
	.vidioc_s_selection = wave4_vpu_enc_s_selection,

	.vidioc_g_parm = wave4_vpu_enc_g_parm,
	.vidioc_s_parm = wave4_vpu_enc_s_parm,

	.vidioc_reqbufs = wave4_vpu_enc_ioctl_reqbufs,
	.vidioc_querybuf = wave4_vpu_enc_ioctl_querybuf,
	.vidioc_create_bufs = wave4_vpu_enc_ioctl_create_bufs,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_expbuf = wave4_vpu_enc_ioctl_expbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = wave4_vpu_enc_encoder_cmd,

	.vidioc_subscribe_event = wave4_vpu_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int wave4_vpu_enc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vpu_instance *inst = wave4_ctrl_to_vpu_inst(ctrl);

	dev_dbg(inst->dev->dev, "%s: name: %s | value: %d\n", __func__, ctrl->name, ctrl->val);

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_AU_DELIMITER:
		inst->encode_aud = ctrl->val;
		break;
	case V4L2_CID_HFLIP:
		inst->mirror_direction &= ~BIT(1);
		inst->mirror_direction |= ctrl->val << 1;
		break;
	case V4L2_CID_VFLIP:
		inst->mirror_direction &= ~BIT(0);
		inst->mirror_direction |= ctrl->val;
		break;
	case V4L2_CID_ROTATE:
		inst->rot_angle = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_VBV_SIZE:
		inst->vbv_buf_size = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_BITRATE_MODE_VBR:
			inst->rc_mode = 0;
			break;
		case V4L2_MPEG_VIDEO_BITRATE_MODE_CBR:
			inst->rc_mode = 1;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		inst->bit_rate = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		inst->enc_param.intra_period = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEADER_MODE:
		break;
	case V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE:
		inst->enc_param.independ_slice_mode = ctrl->val;
		inst->enc_param.avc_slice_mode = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_MB:
		inst->enc_param.independ_slice_mode_arg = ctrl->val;
		inst->enc_param.avc_slice_arg = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE:
		inst->rc_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE:
		inst->enc_param.mb_level_rc_enable = ctrl->val;
		inst->enc_param.cu_level_rc_enable = ctrl->val;
		inst->enc_param.hvs_qp_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		atomic_set(&inst->force_key_frame, 1);
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_PROFILE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN:
			inst->enc_param.profile = HEVC_PROFILE_MAIN;
			inst->bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE:
			inst->enc_param.profile = HEVC_PROFILE_STILLPICTURE;
			inst->enc_param.en_still_picture = 1;
			inst->bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10:
			inst->enc_param.profile = HEVC_PROFILE_MAIN10;
			inst->bit_depth = 10;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LEVEL:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_1:
			inst->enc_param.level = 10;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_2:
			inst->enc_param.level = 20;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_2_1:
			inst->enc_param.level = 21;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_3:
			inst->enc_param.level = 30;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1:
			inst->enc_param.level = 31;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_4:
			inst->enc_param.level = 40;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1:
			inst->enc_param.level = 41;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_5:
			inst->enc_param.level = 50;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1:
			inst->enc_param.level = 51;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2:
			inst->enc_param.level = 52;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP:
		inst->enc_param.min_qp_i = ctrl->val;
		inst->enc_param.min_qp_p = ctrl->val;
		inst->enc_param.min_qp_b = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP:
		inst->enc_param.max_qp_i = ctrl->val;
		inst->enc_param.max_qp_p = ctrl->val;
		inst->enc_param.max_qp_b = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP:
		inst->enc_param.intra_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED:
			inst->enc_param.disable_deblk = 1;
			inst->enc_param.sao_enable = 0;
			inst->enc_param.lf_cross_slice_boundary_enable = 0;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_ENABLED:
			inst->enc_param.disable_deblk = 0;
			inst->enc_param.sao_enable = 1;
			inst->enc_param.lf_cross_slice_boundary_enable = 1;
			break;
		case V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY:
			inst->enc_param.disable_deblk = 0;
			inst->enc_param.sao_enable = 1;
			inst->enc_param.lf_cross_slice_boundary_enable = 0;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2:
		inst->enc_param.beta_offset_div2 = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2:
		inst->enc_param.tc_offset_div2 = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_TYPE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_HEVC_REFRESH_NONE:
			inst->enc_param.decoding_refresh_type = 0;
			break;
		case V4L2_MPEG_VIDEO_HEVC_REFRESH_CRA:
			inst->enc_param.decoding_refresh_type = 1;
			break;
		case V4L2_MPEG_VIDEO_HEVC_REFRESH_IDR:
			inst->enc_param.decoding_refresh_type = 2;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_PERIOD:
		inst->enc_param.intra_period = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LOSSLESS_CU:
		inst->enc_param.lossless_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_CONST_INTRA_PRED:
		inst->enc_param.const_intra_pred_flag = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_WAVEFRONT:
		inst->enc_param.wpp_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_STRONG_SMOOTHING:
		inst->enc_param.strong_intra_smooth_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_MAX_NUM_MERGE_MV_MINUS1:
		inst->enc_param.max_num_merge = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_TMV_PREDICTION:
		inst->enc_param.tmvp_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
		case V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE:
			inst->enc_param.profile = H264_PROFILE_BP;
			inst->bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
			inst->enc_param.profile = H264_PROFILE_MP;
			inst->bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED:
			inst->enc_param.profile = H264_PROFILE_EXTENDED;
			inst->bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
			inst->enc_param.profile = H264_PROFILE_HP;
			inst->bit_depth = 8;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10:
			inst->enc_param.profile = H264_PROFILE_HIGH10;
			inst->bit_depth = 10;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422:
			inst->enc_param.profile = H264_PROFILE_HIGH422;
			inst->bit_depth = 10;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE:
			inst->enc_param.profile = H264_PROFILE_HIGH444;
			inst->bit_depth = 10;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_0:
			inst->enc_param.level = 10;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1B:
			inst->enc_param.level = 9;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_1:
			inst->enc_param.level = 11;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_2:
			inst->enc_param.level = 12;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_3:
			inst->enc_param.level = 13;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_0:
			inst->enc_param.level = 20;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_1:
			inst->enc_param.level = 21;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_2:
			inst->enc_param.level = 22;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:
			inst->enc_param.level = 30;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
			inst->enc_param.level = 31;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:
			inst->enc_param.level = 32;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
			inst->enc_param.level = 40;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_1:
			inst->enc_param.level = 41;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:
			inst->enc_param.level = 42;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_5_0:
			inst->enc_param.level = 50;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_5_1:
			inst->enc_param.level = 51;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_H264_MIN_QP:
		inst->enc_param.min_qp_i = ctrl->val;
		inst->enc_param.min_qp_p = ctrl->val;
		inst->enc_param.min_qp_b = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_MAX_QP:
		inst->enc_param.max_qp_i = ctrl->val;
		inst->enc_param.max_qp_p = ctrl->val;
		inst->enc_param.max_qp_b = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP:
		inst->enc_param.intra_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_DISABLED:
			inst->enc_param.disable_deblk = 1;
			inst->enc_param.lf_cross_slice_boundary_enable = 1;
			break;
		case V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_ENABLED:
			inst->enc_param.disable_deblk = 0;
			inst->enc_param.lf_cross_slice_boundary_enable = 1;
			break;
		case V4L2_MPEG_VIDEO_H264_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY:
			inst->enc_param.disable_deblk = 0;
			inst->enc_param.lf_cross_slice_boundary_enable = 0;
			break;
		default:
			return -EINVAL;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA:
		inst->enc_param.beta_offset_div2 = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA:
		inst->enc_param.tc_offset_div2 = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM:
		inst->enc_param.transform8x8_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_CONSTRAINED_INTRA_PREDICTION:
		inst->enc_param.const_intra_pred_flag = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_CHROMA_QP_INDEX_OFFSET:
		inst->enc_param.chroma_cb_qp_offset = ctrl->val;
		inst->enc_param.chroma_cr_qp_offset = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_I_PERIOD:
		inst->enc_param.intra_period = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE:
		inst->enc_param.entropy_coding_mode = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR:
		inst->enc_param.forced_idr_header_enable = ctrl->val;
		break;
	case V4L2_CID_MIN_BUFFERS_FOR_OUTPUT:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops wave4_vpu_enc_ctrl_ops = {
	.s_ctrl = wave4_vpu_enc_s_ctrl,
};

static int wave4_vpu_enc_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
				     unsigned int *num_planes, unsigned int sizes[],
				     struct device *alloc_devs[])
{
	struct vpu_instance *inst = vb2_get_drv_priv(q);
	struct v4l2_pix_format_mplane inst_format =
		(q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ? inst->src_fmt : inst->dst_fmt;
	unsigned int i;

	dev_dbg(inst->dev->dev, "%s: num_buffers: %u | num_planes: %u | type: %u\n", __func__,
		*num_buffers, *num_planes, q->type);

	if (*num_planes) {
		if (inst_format.num_planes != *num_planes)
			return -EINVAL;

		for (i = 0; i < *num_planes; i++) {
			if (sizes[i] < inst_format.plane_fmt[i].sizeimage)
				return -EINVAL;
		}
	} else {
		*num_planes = inst_format.num_planes;
		for (i = 0; i < *num_planes; i++) {
			sizes[i] = inst_format.plane_fmt[i].sizeimage;
			dev_dbg(inst->dev->dev, "%s: size[%u]: %u\n", __func__, i, sizes[i]);
		}
	}

	dev_dbg(inst->dev->dev, "%s: size: %u\n", __func__, sizes[0]);

	return 0;
}

static int wave4_vpu_enc_buf_prepare(struct vb2_buffer *vb)
{
	struct vpu_instance *inst = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct v4l2_pix_format_mplane *fmt;
	unsigned int i;

	fmt = vb->vb2_queue->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ?
		&inst->src_fmt : &inst->dst_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		/*
		 * v4l2-compliance queues OUTPUT buffers with FIELD_ANY and expects
		 * drivers to normalize it for progressive-only encoder paths.
		 */
		if (vbuf->field == V4L2_FIELD_ANY)
			vbuf->field = V4L2_FIELD_NONE;
		if (vbuf->field != V4L2_FIELD_NONE)
			return -EINVAL;
	}

	if (vb->num_planes != fmt->num_planes)
		return -EINVAL;

	for (i = 0; i < fmt->num_planes; i++) {
		if (vb2_plane_size(vb, i) < fmt->plane_fmt[i].sizeimage)
			return -EINVAL;
	}

	return 0;
}

static void wave4_vpu_enc_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vpu_instance *inst = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;

	dev_dbg(inst->dev->dev, "%s: type: %4u index: %4u size: ([0]=%4lu, [1]=%4lu, [2]=%4lu)\n",
		__func__, vb->type, vb->index, vb2_plane_size(&vbuf->vb2_buf, 0),
		vb2_plane_size(&vbuf->vb2_buf, 1), vb2_plane_size(&vbuf->vb2_buf, 2));

	if (vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		vbuf->sequence = inst->queued_src_buf_num++;
	else
		vbuf->sequence = inst->queued_dst_buf_num++;

	v4l2_m2m_buf_queue(m2m_ctx, vbuf);
}

static int wave4_set_enc_openparam(struct enc_open_param *open_param,
				   struct vpu_instance *inst)
{
	struct enc_wave_param input = inst->enc_param;
	const struct v4l2_format_info *info;

	info = v4l2_format_info(inst->src_fmt.pixelformat);
	if (!info)
		return -EINVAL;

	open_param->packed_format = NOT_PACKED;
	if (inst->src_fmt.pixelformat == V4L2_PIX_FMT_YUYV) {
		open_param->src_format = FORMAT_422;
		open_param->packed_format = PACKED_YUYV;
	} else if (inst->src_fmt.pixelformat == V4L2_PIX_FMT_UYVY) {
		open_param->src_format = FORMAT_422;
		open_param->packed_format = PACKED_UYVY;
	} else if (inst->src_fmt.pixelformat == V4L2_PIX_FMT_YVYU) {
		open_param->src_format = FORMAT_422;
		open_param->packed_format = PACKED_YVYU;
	} else if (inst->src_fmt.pixelformat == V4L2_PIX_FMT_VYUY) {
		open_param->src_format = FORMAT_422;
		open_param->packed_format = PACKED_VYUY;
	} else if (info->hdiv == 2 && info->vdiv == 1) {
		open_param->src_format = FORMAT_422;
	} else {
		open_param->src_format = FORMAT_420;
	}

	/*
	 * Wave420L firmware on SG2002 is stable with legacy IPP preset.
	 * PRESET_IDX_IPP_SINGLE can trip SET_PARAM watchdog on this path.
	 */
	open_param->wave_param.gop_preset_idx = PRESET_IDX_IPP;
	open_param->wave_param.hvs_qp_scale = 2;
	open_param->wave_param.hvs_max_delta_qp = 10;
	open_param->wave_param.skip_intra_trans = 1;
	open_param->wave_param.intra_nx_n_enable = 1;
	open_param->wave_param.nr_intra_weight_y = 7;
	open_param->wave_param.nr_intra_weight_cb = 7;
	open_param->wave_param.nr_intra_weight_cr = 7;
	open_param->wave_param.nr_inter_weight_y = 4;
	open_param->wave_param.nr_inter_weight_cb = 4;
	open_param->wave_param.nr_inter_weight_cr = 4;
	open_param->wave_param.rdo_skip = 1;
	open_param->wave_param.lambda_scaling_enable = 1;

	open_param->line_buf_int_en = true;
	open_param->pic_width = inst->conf_win.width;
	open_param->pic_height = inst->conf_win.height;
	open_param->frame_rate_info = inst->frame_rate;
	open_param->rc_enable = inst->rc_enable;
	open_param->bit_rate = inst->bit_rate;
	open_param->vbv_buffer_size = inst->vbv_buf_size;
	if (inst->rc_enable) {
		/* Match the Wave420L firmware RC buffer model used by the BSP. */
		open_param->initial_delay = inst->vbv_buf_size;
		open_param->wave_param.initial_rc_qp = 63;
		open_param->wave_param.init_buf_level_x8 = 1;
		open_param->wave_param.bit_alloc_mode = 1;
		if (inst->rc_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR)
			open_param->wave_param.trans_rate = inst->bit_rate;
		open_param->wave_param.rc_weight_param = 16;
		open_param->wave_param.rc_weight_buf = 128;
	}
	open_param->wave_param.mb_level_rc_enable = input.mb_level_rc_enable;
	open_param->wave_param.cu_level_rc_enable = input.cu_level_rc_enable;
	open_param->wave_param.hvs_qp_enable = input.hvs_qp_enable;
	if (inst->rc_mode == 0)
		open_param->vbv_buffer_size = 3000;
	open_param->wave_param.profile = input.profile;
	open_param->wave_param.en_still_picture = input.en_still_picture;
	open_param->wave_param.level = input.level;
	open_param->wave_param.internal_bit_depth = inst->bit_depth;
	open_param->wave_param.intra_qp = input.intra_qp;
	open_param->wave_param.min_qp_i = input.min_qp_i;
	open_param->wave_param.max_qp_i = input.max_qp_i;
	open_param->wave_param.min_qp_p = input.min_qp_p;
	open_param->wave_param.max_qp_p = input.max_qp_p;
	open_param->wave_param.min_qp_b = input.min_qp_b;
	open_param->wave_param.max_qp_b = input.max_qp_b;
	open_param->wave_param.disable_deblk = input.disable_deblk;
	open_param->wave_param.sao_enable = input.sao_enable;
	open_param->wave_param.lf_cross_slice_boundary_enable =
		input.lf_cross_slice_boundary_enable;
	open_param->wave_param.tc_offset_div2 = input.tc_offset_div2;
	open_param->wave_param.beta_offset_div2 = input.beta_offset_div2;
	open_param->wave_param.decoding_refresh_type = input.decoding_refresh_type;
	open_param->wave_param.intra_period = input.intra_period;
	open_param->wave_param.entropy_coding_mode = input.entropy_coding_mode;
	open_param->wave_param.lossless_enable = input.lossless_enable;
	open_param->wave_param.const_intra_pred_flag = input.const_intra_pred_flag;
	open_param->wave_param.wpp_enable = input.wpp_enable;
	open_param->wave_param.strong_intra_smooth_enable = input.strong_intra_smooth_enable;
	open_param->wave_param.max_num_merge = input.max_num_merge;
	open_param->wave_param.tmvp_enable = input.tmvp_enable;
	open_param->wave_param.transform8x8_enable = input.transform8x8_enable;
	open_param->wave_param.chroma_cb_qp_offset = input.chroma_cb_qp_offset;
	open_param->wave_param.chroma_cr_qp_offset = input.chroma_cr_qp_offset;
	open_param->wave_param.independ_slice_mode = input.independ_slice_mode;
	open_param->wave_param.independ_slice_mode_arg = input.independ_slice_mode_arg;
	open_param->wave_param.avc_slice_mode = input.avc_slice_mode;
	open_param->wave_param.avc_slice_arg = input.avc_slice_arg;
	open_param->wave_param.intra_refresh_mode = input.intra_refresh_mode;
	open_param->wave_param.intra_refresh_arg = input.intra_refresh_arg;
	open_param->wave_param.forced_idr_header_enable = input.forced_idr_header_enable;

	if (!open_param->rc_enable)
		open_param->wave_param.hvs_qp_scale = 0;

	return 0;
}

static int initialize_sequence(struct vpu_instance *inst)
{
	struct enc_initial_info initial_info;
	struct v4l2_ctrl *ctrl;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	int ret;

	/*
	 * Wave4 firmware expects a valid BS buffer at SET_PARAM time.
	 * Allocate a dedicated internal bitstream ring for sequence setup.
	 */
	if (!inst->bitstream_vbuf.size) {
		inst->bitstream_vbuf.size = W4_ENC_SETUP_BS_SIZE;
		ret = wave4_vdi_allocate_dma_memory(inst->dev, &inst->bitstream_vbuf);
		if (ret) {
			memset(&inst->bitstream_vbuf, 0, sizeof(inst->bitstream_vbuf));
			return ret;
		}
		wave4_vdi_clear_memory(inst->dev, &inst->bitstream_vbuf);
	}

	p_enc_info->stream_buf_start_addr = inst->bitstream_vbuf.daddr;
	p_enc_info->stream_buf_size = inst->bitstream_vbuf.size;
	p_enc_info->stream_buf_end_addr = p_enc_info->stream_buf_start_addr +
					  p_enc_info->stream_buf_size;
	p_enc_info->stream_rd_ptr = p_enc_info->stream_buf_start_addr;
	p_enc_info->stream_wr_ptr = p_enc_info->stream_buf_start_addr;

	ret = wave4_vpu_enc_issue_seq_init(inst);
	if (ret) {
		dev_err(inst->dev->dev, "%s: wave4_vpu_enc_issue_seq_init, fail: %d\n",
			__func__, ret);
		return ret;
	}

	ret = wave4_vpu_enc_complete_seq_init(inst, &initial_info);
	if (ret)
		return ret;

	/*
	 * Some Wave420L runs report zero here even after successful SET_PARAM.
	 * Keep a conservative floor so framebuffer setup can proceed and
	 * surface the next failure point.
	 */
	if (!initial_info.min_frame_buffer_count) {
		initial_info.min_frame_buffer_count = 2;
	}
	if (!initial_info.min_src_frame_count) {
		initial_info.min_src_frame_count = 1;
	}

	inst->min_src_buf_count = initial_info.min_src_frame_count +
				  W4_COMMAND_QUEUE_DEPTH;

	ctrl = v4l2_ctrl_find(&inst->v4l2_ctrl_hdl,
			      V4L2_CID_MIN_BUFFERS_FOR_OUTPUT);
	if (ctrl)
		v4l2_ctrl_s_ctrl(ctrl, inst->min_src_buf_count);

	inst->fbc_buf_count = initial_info.min_frame_buffer_count;

	return 0;
}

static int prepare_fb(struct vpu_instance *inst)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	u32 fb_stride = ALIGN(inst->dst_fmt.width, 32);
	u32 fb_height = ALIGN(inst->dst_fmt.height, 32);
	int i, ret = 0;

	/*
	 * STREAMOFF/STREAMON can reuse one open instance. Re-registering frame
	 * buffers requires clearing the previous registration state and freeing
	 * associated per-registration DMA buffers first.
	 */
	if (p_enc_info->stride) {
		for (i = 0; i < MAX_REG_FRAME; i++) {
			if (!inst->frame_vbuf[i].size)
				continue;
			wave4_vpu_dec_reset_framebuffer(inst, i);
		}

		if (p_enc_info->vb_sub_sam_buf.size)
			wave4_vdi_free_dma_memory(inst->dev, &p_enc_info->vb_sub_sam_buf);
		if (p_enc_info->vb_mv.size)
			wave4_vdi_free_dma_memory(inst->dev, &p_enc_info->vb_mv);
		if (p_enc_info->vb_fbc_y_tbl.size)
			wave4_vdi_free_dma_memory(inst->dev, &p_enc_info->vb_fbc_y_tbl);
		if (p_enc_info->vb_fbc_c_tbl.size)
			wave4_vdi_free_dma_memory(inst->dev, &p_enc_info->vb_fbc_c_tbl);

		p_enc_info->stride = 0;
		p_enc_info->num_frame_buffers = 0;
	}

	for (i = 0; i < inst->fbc_buf_count; i++) {
		u32 luma_size = fb_stride * fb_height;
		u32 chroma_size = ALIGN(fb_stride / 2, 16) * fb_height;

		inst->frame_vbuf[i].size = luma_size + chroma_size;
		ret = wave4_vdi_allocate_dma_memory(inst->dev, &inst->frame_vbuf[i]);
		if (ret < 0) {
			dev_err(inst->dev->dev, "%s: failed to allocate FBC buffer %zu\n",
				__func__, inst->frame_vbuf[i].size);
			goto free_buffers;
		}

		inst->frame_buf[i].buf_y = inst->frame_vbuf[i].daddr;
		inst->frame_buf[i].buf_cb = (dma_addr_t)-1;
		inst->frame_buf[i].buf_cr = (dma_addr_t)-1;
		inst->frame_buf[i].update_fb_info = true;
		inst->frame_buf[i].size = inst->frame_vbuf[i].size;
	}

	ret = wave4_vpu_enc_register_frame_buffer(inst, inst->fbc_buf_count, fb_stride,
						  fb_height, COMPRESSED_FRAME_MAP);
	if (ret) {
		dev_err(inst->dev->dev,
			"%s: wave4_vpu_enc_register_frame_buffer, fail: %d\n",
			__func__, ret);
		goto free_buffers;
	}

	return 0;
free_buffers:
	for (i = 0; i < inst->fbc_buf_count; i++)
		wave4_vpu_dec_reset_framebuffer(inst, i);
	return ret;
}

static int wave4_vpu_enc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vpu_instance *inst = vb2_get_drv_priv(q);
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	bool other_q_streaming;
	int ret;

	p_enc_info->stop_pending = false;
	wave4_vpu_enc_put_async_pm(inst);
	ret = pm_runtime_resume_and_get(inst->dev->dev);
	if (ret < 0)
		return ret;
	v4l2_m2m_update_start_streaming_state(m2m_ctx, q);
	other_q_streaming = q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ?
			    m2m_ctx->cap_q_ctx.q.streaming :
			    m2m_ctx->out_q_ctx.q.streaming;
	dev_dbg(inst->dev->dev,
		"%s: type=%u state=%s out_stream=%d cap_stream=%d other=%d\n",
		__func__, q->type, state_to_str(inst->state),
		m2m_ctx->out_q_ctx.q.streaming, m2m_ctx->cap_q_ctx.q.streaming,
		other_q_streaming);

	if (inst->state == VPU_INST_STATE_NONE && q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		struct enc_open_param open_param;

		memset(&open_param, 0, sizeof(struct enc_open_param));

		ret = wave4_set_enc_openparam(&open_param, inst);
		if (ret) {
			dev_dbg(inst->dev->dev, "%s: wave4_set_enc_openparam, fail: %d\n",
				__func__, ret);
			goto return_buffers;
		}

		ret = wave4_vpu_enc_open(inst, &open_param);
		if (ret) {
			dev_dbg(inst->dev->dev, "%s: wave4_vpu_enc_open, fail: %d\n",
				__func__, ret);
			goto return_buffers;
		}

		if (inst->mirror_direction) {
			wave4_vpu_enc_give_command(inst, ENABLE_MIRRORING, NULL);
			wave4_vpu_enc_give_command(inst, SET_MIRROR_DIRECTION,
						   &inst->mirror_direction);
		}
		if (inst->rot_angle) {
			wave4_vpu_enc_give_command(inst, ENABLE_ROTATION, NULL);
			wave4_vpu_enc_give_command(inst, SET_ROTATION_ANGLE, &inst->rot_angle);
		}

		ret = switch_state(inst, VPU_INST_STATE_OPEN);
		if (ret)
			goto return_buffers;
	}
	/*
	 * STREAMOFF drives the instance into STOP while file handle stays open.
	 * Allow START_STREAMING to re-run sequence setup from STOP so repeated
	 * streamon/off cycles (e.g. v4l2-compliance) can continue on one fd.
	 */
	if ((inst->state == VPU_INST_STATE_OPEN ||
	     inst->state == VPU_INST_STATE_STOP) && other_q_streaming) {
		ret = initialize_sequence(inst);
		if (ret) {
			dev_warn(inst->dev->dev, "Sequence not found: %d\n", ret);
			goto return_buffers;
		}
		ret = switch_state(inst, VPU_INST_STATE_INIT_SEQ);
		if (ret)
			goto return_buffers;
		/*
		 * The sequence must be analyzed first to calculate the proper
		 * size of the auxiliary buffers.
		 */
		ret = prepare_fb(inst);
		if (ret) {
			dev_warn(inst->dev->dev, "Framebuffer preparation, fail: %d\n", ret);
			goto return_buffers;
		}

		ret = switch_state(inst, VPU_INST_STATE_PIC_RUN);
	}
	if (ret)
		goto return_buffers;

	if (inst->state == VPU_INST_STATE_PIC_RUN && other_q_streaming) {
		/*
		 * Userspace may queue buffers before the second STREAMON call.
		 * Kick m2m scheduling once both queues are streaming so those
		 * pre-queued buffers are processed immediately.
		 */
		dev_dbg(inst->dev->dev,
			"%s: kicking schedule src_ready=%u dst_ready=%u draining=%d\n",
			__func__, v4l2_m2m_num_src_bufs_ready(m2m_ctx),
			v4l2_m2m_num_dst_bufs_ready(m2m_ctx),
			m2m_ctx->is_draining);
		v4l2_m2m_try_schedule(m2m_ctx);
	}

	pm_runtime_put_autosuspend(inst->dev->dev);
	return 0;
return_buffers:
	wave4_return_bufs(q, VB2_BUF_STATE_QUEUED);
	pm_runtime_put_autosuspend(inst->dev->dev);
	return ret;
}

static void streamoff_output(struct vpu_instance *inst, struct vb2_queue *q)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct vb2_v4l2_buffer *buf;

	while ((buf = v4l2_m2m_src_buf_remove(m2m_ctx))) {
		dev_dbg(inst->dev->dev, "%s: buf type %4u | index %4u\n",
			__func__, buf->vb2_buf.type, buf->vb2_buf.index);
		if (buf->vb2_buf.state == VB2_BUF_STATE_ACTIVE ||
		    buf->vb2_buf.state == VB2_BUF_STATE_QUEUED)
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
	}
}

static void streamoff_capture(struct vpu_instance *inst, struct vb2_queue *q)
{
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct vb2_v4l2_buffer *buf;

	while ((buf = v4l2_m2m_dst_buf_remove(m2m_ctx))) {
		dev_dbg(inst->dev->dev, "%s: buf type %4u | index %4u\n",
			__func__, buf->vb2_buf.type, buf->vb2_buf.index);
		if (buf->vb2_buf.state == VB2_BUF_STATE_ACTIVE ||
		    buf->vb2_buf.state == VB2_BUF_STATE_QUEUED) {
			vb2_set_plane_payload(&buf->vb2_buf, 0, 0);
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
		}
	}

	v4l2_m2m_clear_state(m2m_ctx);
}

static void wave4_vpu_enc_stop_streaming(struct vb2_queue *q)
{
	struct vpu_instance *inst = vb2_get_drv_priv(q);
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	int ret;
	bool check_cmd = true;

	/*
	 * Note that we don't need m2m_ctx->next_buf_last for this driver, so we
	 * don't call v4l2_m2m_update_stop_streaming_state().
	 */

	dev_dbg(inst->dev->dev, "%s: type: %u\n", __func__, q->type);
	ret = pm_runtime_resume_and_get(inst->dev->dev);
	if (ret < 0) {
		wave4_vpu_enc_put_async_pm(inst);
		if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
			streamoff_output(inst, q);
		else
			streamoff_capture(inst, q);
		return;
	}
	p_enc_info->stop_pending = true;

	if (wave4_vpu_both_queues_are_streaming(inst))
		switch_state(inst, VPU_INST_STATE_STOP);

	while (check_cmd) {
		struct queue_status_info q_status;
		struct enc_output_info enc_output_info;

		wave4_vpu_enc_give_command(inst, ENC_GET_QUEUE_STATUS, &q_status);

		if (q_status.report_queue_count == 0)
			break;

		if (wave4_vpu_wait_interrupt(inst, VPU_ENC_TIMEOUT) < 0)
			break;

		if (wave4_vpu_enc_get_output_info(inst, &enc_output_info))
			dev_dbg(inst->dev->dev, "Getting encoding results from fw, fail\n");
	}

	/*
	 * Crash-guard: start_encode() removes one source buffer from m2m ready
	 * queue and leaves it ACTIVE until finish_encode() reports completion.
	 * On streamoff races, that callback may never safely consume queues.
	 * Force-complete all tracked in-flight source buffers here to avoid
	 * VB2 warning:
	 * "stop_streaming operation is leaving buffer ... in active state".
	 */
	wave4_vpu_enc_flush_src_meta(inst, VB2_BUF_STATE_ERROR);

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		streamoff_output(inst, q);
	else
		streamoff_capture(inst, q);

	wave4_vpu_enc_put_async_pm(inst);
	pm_runtime_put_autosuspend(inst->dev->dev);
}

static const struct vb2_ops wave4_vpu_enc_vb2_ops = {
	.queue_setup = wave4_vpu_enc_queue_setup,
	.buf_prepare = wave4_vpu_enc_buf_prepare,
	.buf_queue = wave4_vpu_enc_buf_queue,
	.start_streaming = wave4_vpu_enc_start_streaming,
	.stop_streaming = wave4_vpu_enc_stop_streaming,
};

static void wave4_set_default_format(struct v4l2_pix_format_mplane *src_fmt,
				     struct v4l2_pix_format_mplane *dst_fmt)
{
	src_fmt->pixelformat = enc_fmt_list[VPU_FMT_TYPE_RAW][0].v4l2_pix_fmt;
	wave4_update_pix_fmt(src_fmt, VPU_FMT_TYPE_RAW,
			     W4_DEF_ENC_PIC_WIDTH, W4_DEF_ENC_PIC_HEIGHT,
			     &enc_frmsize[VPU_FMT_TYPE_RAW]);

	dst_fmt->pixelformat = enc_fmt_list[VPU_FMT_TYPE_CODEC][0].v4l2_pix_fmt;
	wave4_update_pix_fmt(dst_fmt, VPU_FMT_TYPE_CODEC,
			     W4_DEF_ENC_PIC_WIDTH, W4_DEF_ENC_PIC_HEIGHT,
			     &enc_frmsize[VPU_FMT_TYPE_CODEC]);
}

static int wave4_vpu_enc_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	return wave4_vpu_queue_init(priv, src_vq, dst_vq, &wave4_vpu_enc_vb2_ops);
}

static const struct vpu_instance_ops wave4_vpu_enc_inst_ops = {
	.finish_process = wave4_vpu_enc_finish_encode,
};

static void wave4_vpu_enc_device_run(void *priv)
{
	struct vpu_instance *inst = priv;
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	u32 fail_res = 0;
	int ret;

	ret = pm_runtime_resume_and_get(inst->dev->dev);
	if (ret < 0) {
		v4l2_m2m_job_finish(inst->v4l2_m2m_dev, m2m_ctx);
		return;
	}
	switch (inst->state) {
	case VPU_INST_STATE_PIC_RUN:
		/*
		 * Arm completion ownership before issuing ENC_PIC.
		 * Otherwise a very fast completion can race in and get discarded
		 * before device_run returns, leaking runtime-PM accounting.
		 */
		WRITE_ONCE(p_enc_info->async_pm_ref_held, 1);

		ret = start_encode(inst, &fail_res);
		if (ret) {
			if (ret == -EINVAL)
				dev_err(inst->dev->dev,
					"Frame encoding on m2m context (%p), fail: %d (res: %d)\n",
					m2m_ctx, ret, fail_res);
			else if (ret == -EAGAIN)
				dev_dbg(inst->dev->dev, "Missing buffers for encode, try again\n");
			break;
		}
		dev_dbg(inst->dev->dev, "%s: leave with active job", __func__);
		return;
	default:
		WARN(1, "Execution of a job in state %s is invalid.\n",
		     state_to_str(inst->state));
		break;
	}
	dev_dbg(inst->dev->dev, "%s: leave and finish job", __func__);
	if (READ_ONCE(p_enc_info->async_pm_ref_held))
		wave4_vpu_enc_put_async_pm(inst);
	else
		pm_runtime_put_autosuspend(inst->dev->dev);
	v4l2_m2m_job_finish(inst->v4l2_m2m_dev, m2m_ctx);
}

static int wave4_vpu_enc_job_ready(void *priv)
{
	struct vpu_instance *inst = priv;
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;

	switch (inst->state) {
	case VPU_INST_STATE_NONE:
		dev_dbg(inst->dev->dev, "Encoder must be open to start queueing M2M jobs!\n");
		return false;
	case VPU_INST_STATE_PIC_RUN:
		if (p_enc_info->stop_pending)
			return false;
		if (m2m_ctx->is_draining || v4l2_m2m_num_src_bufs_ready(m2m_ctx)) {
			dev_dbg(inst->dev->dev, "Encoder ready for a job, state: %s\n",
				state_to_str(inst->state));
			return true;
		}
		fallthrough;
	default:
		dev_dbg(inst->dev->dev,
			"Encoder not ready for a job, state: %s, %s draining, %d src bufs ready\n",
			state_to_str(inst->state), m2m_ctx->is_draining ? "is" : "is not",
			v4l2_m2m_num_src_bufs_ready(m2m_ctx));
		break;
	}
	return false;
}

static void wave4_vpu_enc_job_abort(void *priv)
{
	struct vpu_instance *inst = priv;
	struct v4l2_m2m_ctx *m2m_ctx = inst->v4l2_fh.m2m_ctx;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;

	/*
	 * Crash-guard: if a running ENC_PIC job never reports completion,
	 * v4l2_m2m_cancel_job() waits forever in release/streamoff.
	 * Force-finish the running m2m job so teardown can proceed.
	 */
	p_enc_info->stop_pending = true;
	if (inst->state == VPU_INST_STATE_PIC_RUN)
		switch_state(inst, VPU_INST_STATE_STOP);
	wave4_vpu_enc_put_async_pm(inst);
	v4l2_m2m_job_finish(inst->v4l2_m2m_dev, m2m_ctx);
}

static const struct v4l2_m2m_ops wave4_vpu_enc_m2m_ops = {
	.device_run = wave4_vpu_enc_device_run,
	.job_ready = wave4_vpu_enc_job_ready,
	.job_abort = wave4_vpu_enc_job_abort,
};

static int wave4_vpu_open_enc(struct file *filp)
{
	struct video_device *vdev = video_devdata(filp);
	struct vpu_device *dev = video_drvdata(filp);
	struct vpu_instance *inst = NULL;
	struct v4l2_ctrl_handler *v4l2_ctrl_hdl;
	int ret = 0;

	inst = kzalloc_obj(*inst);
	if (!inst)
		return -ENOMEM;
	v4l2_ctrl_hdl = &inst->v4l2_ctrl_hdl;

	inst->dev = dev;
	inst->type = VPU_INST_TYPE_ENC;
	inst->ops = &wave4_vpu_enc_inst_ops;

	inst->codec_info = kzalloc_obj(*inst->codec_info);
	if (!inst->codec_info) {
		kfree(inst);
		return -ENOMEM;
	}
	wave4_vpu_enc_src_meta_reset(&inst->codec_info->enc_info);

	v4l2_fh_init(&inst->v4l2_fh, vdev);
	v4l2_fh_add(&inst->v4l2_fh, filp);

	INIT_LIST_HEAD(&inst->list);

	inst->v4l2_m2m_dev = inst->dev->v4l2_m2m_enc_dev;
	inst->v4l2_fh.m2m_ctx =
		v4l2_m2m_ctx_init(inst->v4l2_m2m_dev, inst, wave4_vpu_enc_queue_init);
	if (IS_ERR(inst->v4l2_fh.m2m_ctx)) {
		ret = PTR_ERR(inst->v4l2_fh.m2m_ctx);
		goto cleanup_inst;
	}
	v4l2_m2m_set_src_buffered(inst->v4l2_fh.m2m_ctx, true);

	v4l2_ctrl_handler_init(v4l2_ctrl_hdl, 50);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN, 0,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1, 0,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_1);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
			  0, 51, 1, 8);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,
			  0, 51, 1, 51);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP,
			  0, 51, 1, 30);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE,
			       V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY, 0,
			       V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_ENABLED);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2,
			  -6, 6, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2,
			  -6, 6, 1, 0);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_TYPE,
			       V4L2_MPEG_VIDEO_HEVC_REFRESH_IDR,
			       BIT(V4L2_MPEG_VIDEO_HEVC_REFRESH_NONE) |
			       BIT(V4L2_MPEG_VIDEO_HEVC_REFRESH_CRA),
			       V4L2_MPEG_VIDEO_HEVC_REFRESH_IDR);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_REFRESH_PERIOD,
			  0, 2047, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_LOSSLESS_CU,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_CONST_INTRA_PRED,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_TMV_PREDICTION,
			  0, 1, 1, 1);

	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_AU_DELIMITER,
			  0, 1, 1, 1);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_HFLIP,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_VFLIP,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_VBV_SIZE,
			  10, 3000, 1, 1000);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CBR,
			       BIT(V4L2_MPEG_VIDEO_BITRATE_MODE_VBR),
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_BITRATE,
			  0, 700000000, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_GOP_SIZE,
			  0, 2047, 1, 0);
	v4l2_ctrl_new_std_menu(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEADER_MODE,
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
			       BIT(V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE),
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
			  0, 0, 0, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE,
			  0, 1, 1, 0);
	v4l2_ctrl_new_std(v4l2_ctrl_hdl, &wave4_vpu_enc_ctrl_ops,
			  V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 1, 32, 1, 1);

	if (v4l2_ctrl_hdl->error) {
		ret = -ENODEV;
		goto cleanup_inst;
	}

	inst->v4l2_fh.ctrl_handler = v4l2_ctrl_hdl;
	atomic_set(&inst->force_key_frame, 0);
	v4l2_ctrl_handler_setup(v4l2_ctrl_hdl);

	wave4_set_default_format(&inst->src_fmt, &inst->dst_fmt);
	inst->std = wave4_to_vpu_std(inst->dst_fmt.pixelformat, inst->type);
	inst->conf_win.width = inst->dst_fmt.width;
	inst->conf_win.height = inst->dst_fmt.height;
	inst->colorspace = V4L2_COLORSPACE_REC709;
	inst->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	inst->quantization = V4L2_QUANTIZATION_DEFAULT;
	inst->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	inst->frame_rate = 30;

	init_completion(&inst->irq_done);
	init_completion(&inst->command_done);
	ret = wave4_kfifo_alloc(inst);
	if (ret) {
		dev_err(inst->dev->dev, "failed to allocate fifo\n");
		goto cleanup_inst;
	}

	inst->id = ida_alloc(&inst->dev->inst_ida, GFP_KERNEL);
	if (inst->id < 0) {
		dev_warn(inst->dev->dev, "Allocating instance ID, fail: %d\n", inst->id);
		ret = inst->id;
		goto cleanup_inst;
	}

	wave4_vdi_allocate_sram(inst->dev);

	ret = mutex_lock_interruptible(&dev->irq_lock);
	if (ret)
		goto cleanup_inst;

	spin_lock_irq(&dev->irq_spinlock);
	list_add_tail(&inst->list, &dev->instances);
	spin_unlock_irq(&dev->irq_spinlock);

	mutex_unlock(&dev->irq_lock);

	return 0;

cleanup_inst:
	wave4_cleanup_instance(inst, filp);
	return ret;
}

static int wave4_vpu_enc_release(struct file *filp)
{
	return wave4_vpu_release_device(filp, wave4_vpu_enc_close, "encoder");
}

static const struct v4l2_file_operations wave4_vpu_enc_fops = {
	.owner = THIS_MODULE,
	.open = wave4_vpu_open_enc,
	.release = wave4_vpu_enc_release,
	.unlocked_ioctl = video_ioctl2,
	.poll = v4l2_m2m_fop_poll,
	.mmap = v4l2_m2m_fop_mmap,
};

int wave4_vpu_enc_register_device(struct vpu_device *dev)
{
	struct video_device *vdev_enc;
	int ret;

	vdev_enc = devm_kzalloc(dev->v4l2_dev.dev, sizeof(*vdev_enc), GFP_KERNEL);
	if (!vdev_enc)
		return -ENOMEM;

	dev->v4l2_m2m_enc_dev = v4l2_m2m_init(&wave4_vpu_enc_m2m_ops);
	if (IS_ERR(dev->v4l2_m2m_enc_dev)) {
		ret = PTR_ERR(dev->v4l2_m2m_enc_dev);
		dev_err(dev->dev, "v4l2_m2m_init, fail: %d\n", ret);
		return -EINVAL;
	}

	dev->video_dev_enc = vdev_enc;

	strscpy(vdev_enc->name, VPU_ENC_DEV_NAME, sizeof(vdev_enc->name));
	vdev_enc->fops = &wave4_vpu_enc_fops;
	vdev_enc->ioctl_ops = &wave4_vpu_enc_ioctl_ops;
	vdev_enc->release = video_device_release_empty;
	vdev_enc->v4l2_dev = &dev->v4l2_dev;
	vdev_enc->vfl_dir = VFL_DIR_M2M;
	vdev_enc->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	vdev_enc->lock = &dev->dev_lock;

	ret = video_register_device(vdev_enc, VFL_TYPE_VIDEO, -1);
	if (ret)
		return ret;

	video_set_drvdata(vdev_enc, dev);

	return 0;
}

void wave4_vpu_enc_unregister_device(struct vpu_device *dev)
{
	video_unregister_device(dev->video_dev_enc);
	if (dev->v4l2_m2m_enc_dev)
		v4l2_m2m_release(dev->v4l2_m2m_enc_dev);
}
