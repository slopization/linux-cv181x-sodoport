// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave4 series multi-standard codec IP - wave4 backend logic
 *
 * Copyright (C) 2021-2023 CHIPS&MEDIA INC
 */

#include <linux/iopoll.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/reset.h>
#include "wave4-vpu.h"
#include "wave4.h"
#include "wave4-regdefine.h"

#define FIO_TIMEOUT			10000000
#define FIO_CTRL_READY			BIT(31)
#define FIO_CTRL_WRITE			BIT(16)
#define VPU_BUSY_CHECK_TIMEOUT		10000000
#define QUEUE_REPORT_MASK		0xffff

#define ENC_AVC_INTRA_IDR_PARAM_MASK	0x7ff
#define ENC_AVC_INTRA_PERIOD_SHIFT		6
#define ENC_AVC_IDR_PERIOD_SHIFT		17
#define ENC_AVC_FORCED_IDR_HEADER_SHIFT		28

#define ENC_HEVC_INTRA_QP_SHIFT			3
#define ENC_HEVC_FORCED_IDR_HEADER_SHIFT	9
#define ENC_HEVC_INTRA_PERIOD_SHIFT		16

#define REMAP_CTRL_MAX_SIZE_BITS	((W4_REMAP_MAX_SIZE >> 12) & 0x1ff)
#define REMAP_CTRL_REGISTER_VALUE(index)	(			\
	(BIT(31) | ((index) << 12) | BIT(11) | REMAP_CTRL_MAX_SIZE_BITS)\
)

#define FASTIO_ADDRESS_MASK		GENMASK(15, 0)
#define SEQ_PARAM_PROFILE_MASK		GENMASK(30, 24)
/* Wave420L register definitions are centralized in wave4-regdefine.h. */
static void _wave4_print_reg_err(struct vpu_device *vpu_dev, u32 reg_fail_reason,
				 const char *func);
#define PRINT_REG_ERR(dev, reason)	_wave4_print_reg_err((dev), (reason), __func__)

static int wave4_allow_resident_fw_fallback = 1;
module_param_named(w4_allow_resident_fw_fallback, wave4_allow_resident_fw_fallback, int, 0644);
MODULE_PARM_DESC(w4_allow_resident_fw_fallback,
		 "Allow using already-running resident firmware when INIT_VPU times out (1=default, 0=force failure)");

static inline const char *cmd_to_str(int cmd, bool is_dec)
{
	switch (cmd) {
	case W4_INIT_VPU:
		return "W4_INIT_VPU";
	case W4_WAKEUP_VPU:
		return "W4_WAKEUP_VPU";
	case W4_SLEEP_VPU:
		return "W4_SLEEP_VPU";
	case W4_CREATE_INSTANCE:
		return "W4_CREATE_INSTANCE";
	case W4_FLUSH_INSTANCE:
		return "W4_FLUSH_INSTANCE";
	case W4_DESTROY_INSTANCE:
		return "W4_DESTROY_INSTANCE";
	case W4_INIT_SEQ:
		return "W4_INIT_SEQ";
	case W4_SET_FB:
		return "W4_SET_FB";
	case W4_DEC_ENC_PIC:
		if (is_dec)
			return "W4_DEC_PIC";
		return "W4_ENC_PIC";
	case W4_ENC_SET_PARAM:
		return "W4_ENC_SET_PARAM";
	case W4_QUERY:
		return "W4_QUERY";
	case W4_UPDATE_BS:
		return "W4_UPDATE_BS";
	case W4_MAX_VPU_COMD:
		return "W4_MAX_VPU_COMD";
	default:
		return "UNKNOWN";
	}
}

static void _wave4_print_reg_err(struct vpu_device *vpu_dev, u32 reg_fail_reason,
				 const char *func)
{
	struct device *dev = vpu_dev->dev;
	u32 reg_val;

	switch (reg_fail_reason) {
	case W4_SYSERR_QUEUEING_FAIL:
		if (vpu_dev->product == PRODUCT_ID_W4) {
			dev_warn(dev, "%s: codec error class: 0x%x\n", func, reg_fail_reason);
			break;
		}
		reg_val = vpu_read_reg(vpu_dev, W4_RET_QUEUE_FAIL_REASON);
		dev_warn(dev, "%s: queueing failure detail: 0x%x\n", func, reg_val);
		break;
	case W4_SYSERR_RESULT_NOT_READY:
		dev_dbg(dev, "%s: result not ready: 0x%x\n", func, reg_fail_reason);
		break;
	case W4_SYSERR_ACCESS_VIOLATION_HW:
		dev_err(dev, "%s: access violation: 0x%x\n", func, reg_fail_reason);
		break;
	case W4_SYSERR_WATCHDOG_TIMEOUT:
		dev_err(dev, "%s: watchdog timeout: 0x%x\n", func, reg_fail_reason);
		break;
	case W4_SYSERR_BUS_ERROR:
		dev_err(dev, "%s: bus error: 0x%x\n", func, reg_fail_reason);
		break;
	case W4_SYSERR_DOUBLE_FAULT:
		dev_err(dev, "%s: double fault: 0x%x\n", func, reg_fail_reason);
		break;
	case W4_SYSERR_VPU_STILL_RUNNING:
		if (vpu_dev->product == PRODUCT_ID_W4) {
			dev_err(dev, "%s: access violation: 0x%x\n", func, reg_fail_reason);
			break;
		}
		dev_err(dev, "%s: still running: 0x%x\n", func, reg_fail_reason);
		break;
	case W4_SYSERR_VLC_BUF_FULL:
		dev_err(dev, "%s: vlc buf full: 0x%x\n", func, reg_fail_reason);
		break;
	default:
		dev_err(dev, "%s: failure:: 0x%x\n", func, reg_fail_reason);
		break;
	}
}

static int wave4_wait_fio_readl(struct vpu_device *vpu_dev, u32 addr, u32 val)
{
	u32 ctrl;
	ktime_t deadline;
	int ret;

	/*
	 * Keep sampling until the target register reaches the expected value.
	 * A single read is not sufficient for BUS_IDLE waits on Wave420L.
	 */
	deadline = ktime_add_us(ktime_get(), FIO_TIMEOUT);
	do {
		ctrl = addr & 0xffff;
		wave4_vdi_write_register(vpu_dev, W4_VPU_FIO_CTRL_ADDR, ctrl);
		ret = read_poll_timeout(wave4_vdi_read_register, ctrl, ctrl & FIO_CTRL_READY,
					0, FIO_TIMEOUT, false, vpu_dev,
					W4_VPU_FIO_CTRL_ADDR);
		if (ret)
			return ret;

		if (wave4_vdi_read_register(vpu_dev, W4_VPU_FIO_DATA) == val)
			return 0;
		cpu_relax();
	} while (ktime_before(ktime_get(), deadline));

	return -ETIMEDOUT;
}

static int wave4_fio_writel(struct vpu_device *vpu_dev, unsigned int addr, unsigned int data)
{
	int ret;
	unsigned int ctrl;

	wave4_vdi_write_register(vpu_dev, W4_VPU_FIO_DATA, data);
	ctrl = FIELD_GET(FASTIO_ADDRESS_MASK, addr);
	ctrl |= FIO_CTRL_WRITE;
	wave4_vdi_write_register(vpu_dev, W4_VPU_FIO_CTRL_ADDR, ctrl);
	ret = read_poll_timeout(wave4_vdi_read_register, ctrl, ctrl & FIO_CTRL_READY, 0,
				FIO_TIMEOUT, false, vpu_dev, W4_VPU_FIO_CTRL_ADDR);
	if (ret)
		dev_dbg_ratelimited(vpu_dev->dev, "FIO write timeout: addr=0x%x data=%x\n",
				    ctrl, data);
	return ret;
}

static int wave4_wait_bus_busy(struct vpu_device *vpu_dev, unsigned int addr)
{
	return wave4_wait_fio_readl(vpu_dev, addr, 0);
}

static int wave4_wait_vpu_busy(struct vpu_device *vpu_dev, unsigned int addr)
{
	u32 data;

	return read_poll_timeout(wave4_vdi_read_register, data, data == 0,
				 0, VPU_BUSY_CHECK_TIMEOUT, false, vpu_dev, addr);
}

static int wave4_wait_vcpu_bus_busy(struct vpu_device *vpu_dev, unsigned int addr)
{
	return wave4_wait_fio_readl(vpu_dev, addr, 0);
}

bool wave4_vpu_is_init(struct vpu_device *vpu_dev)
{
	/*
	 * On some Wave420L integrations the current PC register may read as 0
	 * despite firmware being resident/running. Keep a software latch set
	 * once init/re-init succeeds (or resident firmware is accepted).
	 */
	return vpu_read_reg(vpu_dev, W4_VCPU_CUR_PC) != 0 || vpu_dev->fw_running;
}

unsigned int wave4_vpu_get_product_id(struct vpu_device *vpu_dev)
{
	u32 val = vpu_read_reg(vpu_dev, W4_PRODUCT_NUMBER);

	if (val == W4_PRODUCT_CODE)
		return PRODUCT_ID_W4;

	dev_err(vpu_dev->dev, "Unexpected product id (%x), expected Wave4\n", val);

	return PRODUCT_ID_NONE;
}

static u32 wave4_resolve_bs_endian_nibble(void)
{
	/* Keep the proven Wave4 baseline fixed: 128-bit big-endian nibble. */
	return BITSTREAM_ENDIANNESS_BIG_ENDIAN;
}

static u32 wave4_apply_dec_sec_axi_mask(u32 sec_axi)
{
	return sec_axi;
}

static u32 wave4_apply_enc_sec_axi_mask(u32 sec_axi)
{
	return sec_axi;
}

static inline u32 wave4_cmd_addr(dma_addr_t addr)
{
	return (u32)addr;
}

static inline u32 wave4_bs_addr(dma_addr_t addr)
{
	return wave4_cmd_addr(addr);
}

static u32 wave4_bs_ptr_reg_value(dma_addr_t ptr)
{
	return wave4_bs_addr(ptr);
}

static u32 wave4_translate_command(u32 cmd)
{
	switch (cmd) {
	case W4_INIT_VPU:
		return W4_CMD_INIT_VPU;
	case W4_WAKEUP_VPU:
		/*
		 * Wave420L BSP wake restore re-enters firmware through INIT_VPU
		 * (after remap/reset programming) instead of WAKEUP_VPU.
		 */
		return W4_CMD_INIT_VPU;
	case W4_SLEEP_VPU:
		return W4_CMD_SLEEP_VPU;
	case W4_CREATE_INSTANCE:
		return W4_CMD_CREATE_INSTANCE;
	case W4_FLUSH_INSTANCE:
		return W4_CMD_FLUSH_DECODER;
	case W4_DESTROY_INSTANCE:
		return W4_CMD_FINI_SEQ;
	case W4_INIT_SEQ:
		return W4_CMD_DEC_PIC_HDR_SET_PARAM;
	case W4_SET_FB:
		return W4_CMD_SET_FB;
	case W4_DEC_ENC_PIC:
		return W4_CMD_DEC_ENC_PIC;
	case W4_ENC_SET_PARAM:
		return W4_CMD_DEC_PIC_HDR_SET_PARAM;
	case W4_QUERY:
		return W4_CMD_QUERY_DECODER;
	default:
		return cmd;
	}
}

static inline u32 wave4_hw_command(struct vpu_device *vpu_dev, u32 cmd)
{
	(void)vpu_dev;
	return wave4_translate_command(cmd);
}

static inline u32 wave4_dec_bs_rd_ptr_reg(struct vpu_device *vpu_dev)
{
	(void)vpu_dev;
	return W4_BS_RD_PTR;
}

static inline u32 wave4_dec_bs_wr_ptr_reg(struct vpu_device *vpu_dev)
{
	(void)vpu_dev;
	return W4_BS_WR_PTR;
}

static inline u32 wave4_dec_bs_option_reg(struct vpu_device *vpu_dev)
{
	(void)vpu_dev;
	return W4_BS_OPTION;
}

static inline u32 wave4_dec_cmd_option_reg(struct vpu_device *vpu_dev)
{
	(void)vpu_dev;
	return W4_COMMAND_OPTION;
}

static inline u32 wave4_dec_user_mask_reg(struct vpu_device *vpu_dev)
{
	(void)vpu_dev;
	return W4_CMD_DEC_USER_MASK;
}

static inline u32 wave4_dec_temp_id_reg(struct vpu_device *vpu_dev)
{
	(void)vpu_dev;
	return W4_CMD_DEC_TEMPORAL_ID_PLUS1;
}

static inline u32 wave4_dec_force_fb_latency_reg(struct vpu_device *vpu_dev)
{
	(void)vpu_dev;
	return W4_CMD_DEC_FORCE_FB_LATENCY_PLUS1;
}

static inline u32 wave4_dec_seq_change_reg(struct vpu_device *vpu_dev)
{
	(void)vpu_dev;
	return W4_CMD_SEQ_CHANGE_ENABLE_FLAG;
}

static u32 wave4_codec_mode(enum wave_std std)
{
	switch (std) {
	case W_HEVC_DEC:
		return W4_CODEC_MODE_HEVC_DEC;
	case W_HEVC_ENC:
		return W4_CODEC_MODE_HEVC_ENC;
	case W_AVC_DEC:
		return W4_CODEC_MODE_AVC_DEC;
	case W_AVC_ENC:
		return W4_CODEC_MODE_AVC_ENC;
	default:
		return std;
	}
}

static void wave4_bit_issue_command(struct vpu_device *vpu_dev, struct vpu_instance *inst, u32 cmd)
{
	u32 instance_index = 0;
	u32 codec_mode = 0;
	u32 hw_cmd = wave4_hw_command(vpu_dev, cmd);

	if (inst) {
		instance_index = inst->id;
		codec_mode = wave4_codec_mode(inst->std);
	}

	vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 1);
	vpu_write_reg(vpu_dev, W4_RET_SUCCESS, 0);
	vpu_write_reg(vpu_dev, W4_CORE_INDEX, 0);
	vpu_write_reg(vpu_dev, W4_INST_INDEX,
		      (instance_index & 0xffff) |
		      (codec_mode << 16));

	vpu_write_reg(vpu_dev, W4_COMMAND, hw_cmd);

	if (inst) {
		dev_dbg(vpu_dev->dev,
			"%s: cmd=0x%x hw=0x%x inst_idx=0x%x codec_mode=0x%x (%s)\n",
			__func__, cmd, hw_cmd, instance_index, codec_mode,
			cmd_to_str(cmd, inst->type == VPU_INST_TYPE_DEC));
	} else {
		dev_dbg(vpu_dev->dev, "%s: cmd=0x%x hw=0x%x\n", __func__, cmd, hw_cmd);
	}

	if (hw_cmd != W4_CMD_INIT_VPU) {
		vpu_write_reg(vpu_dev, W4_VPU_HOST_INT_REQ, 1);
	}
}

static int wave4_vpu_firmware_command_queue_error_check(struct vpu_device *dev, u32 *fail_res)
{
	u32 reason = 0;

	/* Check if we were able to add a command into the VCPU QUEUE */
	if (!vpu_read_reg(dev, W4_RET_SUCCESS)) {
		reason = vpu_read_reg(dev, W4_RET_FAIL_REASON);

		/*
		 * Wave4 can report SKIP_MODE_ENABLE on commands that are
		 * otherwise accepted in the legacy W4 flow (e.g. SET_FB,
		 * DESTROY_INSTANCE). Treat it as non-fatal here.
		 */
		if (reason == W4_CMDQ_ERR_SKIP_MODE_ENABLE) {
			if (fail_res)
				*fail_res = reason;
			dev_dbg(dev->dev,
				"%s: ignoring W4 queue reason=0x%x for cmd=0x%x\n",
				__func__, reason, vpu_read_reg(dev, W4_COMMAND));
			return 0;
		}

		PRINT_REG_ERR(dev, reason);

		if (fail_res)
			*fail_res = reason;

		if (reason == W4_SYSERR_VPU_STILL_RUNNING &&
		    dev->product != PRODUCT_ID_W4)
			return -EBUSY;

		return -EIO;
	}
	return 0;
}

static int wave4_wait_command_interrupt(struct vpu_instance *inst)
{
	unsigned long timeout;

	timeout = wait_for_completion_timeout(&inst->command_done,
					      usecs_to_jiffies(VPU_BUSY_CHECK_TIMEOUT));
	WRITE_ONCE(inst->command_irq_mask, 0);

	if (!timeout)
		return -ETIMEDOUT;

	return 0;
}

static int send_firmware_command(struct vpu_instance *inst, u32 cmd, bool check_success,
				 u32 *queue_status, u32 *fail_result)
{
	int ret;
	u32 command_irq_mask = 0;
	u32 int_sts = vpu_read_reg(inst->dev, W4_VPU_VPU_INT_STS);
	u32 reason = vpu_read_reg(inst->dev, W4_VPU_VINT_REASON);
	u32 reason_usr = vpu_read_reg(inst->dev, W4_VPU_VINT_REASON_USR);

	/*
	 * Wave4 can leave VINT_REASON_USR latched even when INT_STS is 0.
	 * Clear any stale reason before queueing a new command.
	 */
	if (int_sts || reason || reason_usr) {
		u32 clear_reason = reason | reason_usr;

		if (clear_reason)
			vpu_write_reg(inst->dev, W4_VPU_VINT_REASON_CLR, clear_reason);
		vpu_write_reg(inst->dev, W4_VPU_VINT_CLEAR, 0x1);
	}

	if (cmd == W4_INIT_SEQ || cmd == W4_ENC_SET_PARAM) {
		command_irq_mask = W4_VPU_INT_SET_PARAM;
		reinit_completion(&inst->command_done);
		WRITE_ONCE(inst->command_irq_mask, command_irq_mask);
	}

	wave4_bit_issue_command(inst->dev, inst, cmd);

	if (command_irq_mask) {
		ret = wave4_wait_command_interrupt(inst);
		if (ret) {
			dev_warn(inst->dev->dev, "%s: command: '%s', timed out\n", __func__,
				 cmd_to_str(cmd, inst->type == VPU_INST_TYPE_DEC));
			return ret;
		}
	} else {
		ret = wave4_wait_vpu_busy(inst->dev, W4_VPU_BUSY_STATUS);
		if (ret) {
			dev_warn(inst->dev->dev, "%s: command: '%s', timed out\n", __func__,
				 cmd_to_str(cmd, inst->type == VPU_INST_TYPE_DEC));
			return -ETIMEDOUT;
		}
	}

	if (queue_status)
		*queue_status = vpu_read_reg(inst->dev, W4_RET_QUEUE_STATUS);

	/* In some cases we want to send multiple commands before checking
	 * whether they are queued properly
	 */
	if (!check_success)
		return 0;

	return wave4_vpu_firmware_command_queue_error_check(inst->dev, fail_result);
}

static int wave4_send_query(struct vpu_device *vpu_dev, struct vpu_instance *inst,
			    enum query_opt query_opt)
{
	int ret;

	vpu_write_reg(vpu_dev, W4_COMMAND_OPTION, query_opt);
	vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 1);
	wave4_bit_issue_command(vpu_dev, inst, W4_QUERY);

	ret = wave4_wait_vpu_busy(vpu_dev, W4_VPU_BUSY_STATUS);
	if (ret) {
		dev_warn(vpu_dev->dev, "command: 'W4_QUERY', timed out opt=0x%x\n", query_opt);
		return ret;
	}

	return wave4_vpu_firmware_command_queue_error_check(vpu_dev, NULL);
}

static int wave4_get_fw_version(struct vpu_device *vpu_dev, u32 *revision)
{
	int ret;

	/*
	 * W4_CMD_GET_FW_VERSION (0x100) collides with W4_DEC_ENC_PIC in the
	 * Wave4 command enum, so issue the raw Wave4 command directly.
	 */
	vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 1);
	vpu_write_reg(vpu_dev, W4_RET_SUCCESS, 0);
	vpu_write_reg(vpu_dev, W4_CORE_INDEX, 0);
	vpu_write_reg(vpu_dev, W4_INST_INDEX, 0);
	vpu_write_reg(vpu_dev, W4_COMMAND, W4_CMD_GET_FW_VERSION);
	vpu_write_reg(vpu_dev, W4_VPU_HOST_INT_REQ, 1);

	ret = wave4_wait_vpu_busy(vpu_dev, W4_VPU_BUSY_STATUS);
	if (ret) {
		/*
		 * Resident-firmware recovery may proceed even when GET_FW_VERSION
		 * does not answer. Ensure BUSY is not left asserted, otherwise
		 * subsequent CREATE_INSTANCE commands cannot be queued.
		 */
		vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 0);
		return ret;
	}

	if (!vpu_read_reg(vpu_dev, W4_RET_SUCCESS)) {
		vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 0);
		return -EIO;
	}

	if (revision)
		*revision = vpu_read_reg(vpu_dev, W4_RET_FW_VERSION);

	return 0;
}

static void setup_wave4_interrupts(struct vpu_device *vpu_dev)
{
	u32 reg_val = 0;

	/*
	 * Keep upstream-style interrupt gating based on exposed capabilities.
	 * Wave420L uses shared reason bits for encode/decode PIC/SET_PARAM paths.
	 */
	if (vpu_dev->attr.support_encoders) {
		reg_val |= W4_VPU_INT_SET_PARAM;
		reg_val |= W4_VPU_INT_PIC_RUN;
		reg_val |= W4_VPU_INT_QUERY;
		reg_val |= W4_VPU_INT_SLEEP_WAKE;
		reg_val |= W4_VPU_INT_BSBUF;
	}

	if (vpu_dev->attr.support_decoders) {
		reg_val |= W4_VPU_INT_SET_PARAM;
		reg_val |= W4_VPU_INT_PIC_RUN;
		reg_val |= W4_VPU_INT_QUERY;
		reg_val |= W4_VPU_INT_SLEEP_WAKE;
		reg_val |= W4_VPU_INT_BSBUF;
	}

	return vpu_write_reg(vpu_dev, W4_VPU_VINT_ENABLE, reg_val);
}

static int setup_wave4_properties(struct device *dev)
{
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	struct vpu_attr *p_attr = &vpu_dev->attr;
	int ret;
	u32 fw_revision = 0;
	u32 std_def0;
	u32 std_def1;
	u32 conf_feature;
	bool has_enc;
	bool has_dec;

	memset(p_attr->product_name, 0, sizeof(p_attr->product_name));
	p_attr->product_name[0] = '4';
	p_attr->product_name[1] = '2';
	p_attr->product_name[2] = '0';
	p_attr->product_name[3] = 'L';
	p_attr->product_id = PRODUCT_ID_W4;
	p_attr->support_backbone = 0;
	p_attr->support_vcpu_backbone = 0;
	p_attr->support_vcore_backbone = 0;

	ret = wave4_get_fw_version(vpu_dev, &fw_revision);
	if (ret) {
		dev_err(dev, "w4 GET_FW_VERSION ping failed: %d\n", ret);
		return ret;
	}

	std_def0 = vpu_read_reg(vpu_dev, W4_RET_STD_DEF0);
	std_def1 = vpu_read_reg(vpu_dev, W4_RET_STD_DEF1);
	conf_feature = vpu_read_reg(vpu_dev, W4_RET_CONF_FEATURE);
	vpu_dev->hw_std_def0 = std_def0;
	vpu_dev->hw_std_def1 = std_def1;
	vpu_dev->hw_conf_feature = conf_feature;
	vpu_dev->hw_cap_queried = true;

	if (vpu_dev->hw_cap_from_std_def1) {
		has_enc = !!(std_def1 & vpu_dev->hw_std_def1_enc_mask);
		has_dec = !!(std_def1 & vpu_dev->hw_std_def1_dec_mask);
		vpu_dev->has_encoder = has_enc;
		vpu_dev->has_decoder = has_dec;
	}

	p_attr->support_decoders = vpu_dev->has_decoder ? BIT(STD_HEVC) : 0;
	p_attr->support_encoders = vpu_dev->has_encoder ? BIT(STD_HEVC) : 0;
	setup_wave4_interrupts(vpu_dev);

	dev_dbg(dev, "w4 firmware revision: %u\n", fw_revision);
	return 0;
}

int wave4_vpu_get_version(struct vpu_device *vpu_dev, u32 *revision)
{
	int ret;

	ret = wave4_get_fw_version(vpu_dev, revision);
	if (ret)
		return ret;
	return revision ? 0 : -EINVAL;
}

int wave4_vpu_init(struct device *dev, u8 *fw, size_t size)
{
	struct vpu_buf *common_vb;
	dma_addr_t code_base;
	u32 code_size;
	size_t code_bytes_required;
	u32 i, reg_val, reason_code;
	int ret;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);

	common_vb = &vpu_dev->common_mem;

	code_base = common_vb->daddr;
	dev_dbg(vpu_dev->dev, "w4 init: code_base=0x%llx fw_size=%zu\n",
		(unsigned long long)code_base, size);

	code_size = W4_MAX_CODE_BUF_SIZE;

	/* ALIGN TO 4KB */
	code_size &= ~0xfff;
	code_bytes_required = size;
	if (code_size < code_bytes_required)
		return -EINVAL;

	ret = wave4_vdi_write_memory(vpu_dev, common_vb, 0, fw, size);
	if (ret < 0) {
		dev_err(vpu_dev->dev, "VPU init, Writing firmware to common buffer, fail: %d\n",
			ret);
		return ret;
	}

	vpu_write_reg(vpu_dev, W4_PO_CONF, 0);

	vpu_write_reg(vpu_dev, W4_VPU_RESET_REQ, W4_RESET_ALL_BLOCKS);
	ret = wave4_wait_vpu_busy(vpu_dev, W4_VPU_RESET_STATUS);
	if (ret) {
		vpu_write_reg(vpu_dev, W4_VPU_RESET_REQ, 0);
		dev_err(vpu_dev->dev, "VPU init(W4 reset-all) timeout\n");
		return ret;
	}
	vpu_write_reg(vpu_dev, W4_VPU_RESET_REQ, 0);

	/* clear registers */

	for (i = W4_CMD_REG_BASE; i < W4_CMD_REG_END; i += 4)
		vpu_write_reg(vpu_dev, i, 0x00);

	{
		u32 remap_size = (code_size >> 12) & 0x1ff;

		reg_val = BIT(31) | (W4_REMAP_INDEX0 << 12) | BIT(11) | remap_size;
		vpu_write_reg(vpu_dev, W4_VPU_REMAP_CTRL, reg_val);
		vpu_write_reg(vpu_dev, W4_VPU_REMAP_VADDR, 0x00000000);
		vpu_write_reg(vpu_dev, W4_VPU_REMAP_PADDR, code_base);

		vpu_write_reg(vpu_dev, W4_ADDR_CODE_BASE, code_base);
		vpu_write_reg(vpu_dev, W4_CODE_SIZE, code_size);
		vpu_write_reg(vpu_dev, W4_CODE_PARAM, 0);
		vpu_write_reg(vpu_dev, W4_TIMEOUT_CNT, 0xffffffff);
		vpu_write_reg(vpu_dev, W4_HW_OPTION, 0);
	}

	setup_wave4_interrupts(vpu_dev);

	vpu_write_reg(vpu_dev, W4_RET_SUCCESS, 0);
	vpu_write_reg(vpu_dev, W4_CORE_INDEX, 0);
	vpu_write_reg(vpu_dev, W4_INST_INDEX, 0);

	vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 1);
	vpu_write_reg(vpu_dev, W4_COMMAND, wave4_hw_command(vpu_dev, W4_INIT_VPU));
	vpu_write_reg(vpu_dev, W4_VPU_REMAP_CORE_START, 1);
	ret = wave4_wait_vpu_busy(vpu_dev, W4_VPU_BUSY_STATUS);
	if (ret) {
		u32 vcpu_pc = vpu_read_reg(vpu_dev, W4_VCPU_CUR_PC);

		dev_err(vpu_dev->dev, "VPU init(W4_VPU_REMAP_CORE_START) timeout\n");
		if (vcpu_pc && READ_ONCE(wave4_allow_resident_fw_fallback)) {
			dev_warn(vpu_dev->dev,
				 "w4 init timeout with live VCPU (pc=0x%x), clearing BUSY and using resident firmware\n",
				 vcpu_pc);
			vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 0);
			ret = setup_wave4_properties(dev);
			if (!ret)
				vpu_dev->fw_running = true;
			return ret;
		} else if (vcpu_pc) {
			dev_warn(vpu_dev->dev,
				 "w4 init timeout with live VCPU (pc=0x%x), resident fallback disabled\n",
				 vcpu_pc);
		}
		return ret;
	}

	ret = wave4_vpu_firmware_command_queue_error_check(vpu_dev, &reason_code);
	if (ret)
		return ret;

	ret = setup_wave4_properties(dev);
	if (!ret)
		vpu_dev->fw_running = true;

	return ret;
}

static int wave4_alloc_dec_workbuf(struct vpu_instance *inst)
{
	struct vpu_buf *work = &inst->codec_info->dec_info.vb_work;
	int ret;

	work->size = W4_DEC_WORKBUF_SIZE;
	ret = wave4_vdi_allocate_dma_memory(inst->dev, work);
	if (ret) {
		memset(work, 0, sizeof(*work));
		return ret;
	}

	ret = wave4_vdi_clear_memory(inst->dev, work);
	if (ret < 0) {
		wave4_vdi_free_dma_memory(inst->dev, work);
		return ret;
	}

	return 0;
}

int wave4_vpu_build_up_dec_param(struct vpu_instance *inst,
				 struct dec_open_param *param)
{
	int ret;
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	struct vpu_device *vpu_dev = inst->dev;

	(void)param;

	p_dec_info->cycle_per_tick = 256;
	if (vpu_dev->sram_buf.size) {
		p_dec_info->sec_axi_info.use_bit_enable = 1;
		p_dec_info->sec_axi_info.use_ip_enable = 1;
		p_dec_info->sec_axi_info.use_lf_row_enable = 1;
	}
	switch (inst->std) {
	case W_HEVC_DEC:
		p_dec_info->seq_change_mask = SEQ_CHANGE_ENABLE_ALL_HEVC;
		break;
	case W_AVC_DEC:
		p_dec_info->seq_change_mask = SEQ_CHANGE_ENABLE_ALL_AVC;
		break;
	default:
		return -EINVAL;
	}

	ret = wave4_alloc_dec_workbuf(inst);
	if (ret)
		return ret;

	/*
	 * Wave4 keeps the CREATE_INSTANCE work-buffer programming in
	 * this command window.
	 */
	vpu_write_reg(inst->dev, W4_ADDR_WORK_BASE, wave4_cmd_addr(p_dec_info->vb_work.daddr));
	vpu_write_reg(inst->dev, W4_WORK_SIZE, p_dec_info->vb_work.size);
	vpu_write_reg(inst->dev, W4_WORK_PARAM, 0);

	/*
	 * Follow Wave4 flow and require RET_SUCCESS after CREATE_INSTANCE.
	 */
	ret = send_firmware_command(inst, W4_CREATE_INSTANCE, true, NULL, NULL);
	if (ret) {
		wave4_vdi_free_dma_memory(vpu_dev, &p_dec_info->vb_work);
		return ret;
	}

	p_dec_info->product_code = vpu_read_reg(inst->dev, W4_PRODUCT_NUMBER);

	return 0;
}

int wave4_vpu_hw_flush_instance(struct vpu_instance *inst)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	u32 instance_queue_count, report_queue_count;
	u32 reg_val = 0;
	u32 fail_res = 0;
	int ret;

	ret = send_firmware_command(inst, W4_FLUSH_INSTANCE, true, &reg_val, &fail_res);
	if (ret)
		return ret;

	instance_queue_count = (reg_val >> 16) & 0xff;
	report_queue_count = (reg_val & QUEUE_REPORT_MASK);
	if (instance_queue_count != 0 || report_queue_count != 0) {
		dev_warn(inst->dev->dev,
			 "FLUSH_INSTANCE cmd didn't reset the amount of queued commands & reports");
	}

	/* reset our local copy of the counts */
	p_dec_info->instance_queue_count = 0;
	p_dec_info->report_queue_count = 0;

	return 0;
}

static u32 get_bitstream_options(struct dec_info *info)
{
	u32 bs_option = 0;

	if (info->stream_endflag)
		bs_option |= BSOPTION_ENABLE_EXPLICIT_END |
			     BSOPTION_HIGHLIGHT_STREAM_END;
	return bs_option;
}

static u32 wave4_vpu_dec_validate_sec_axi(struct vpu_instance *inst);
static u32 wave4_vpu_enc_validate_sec_axi(struct vpu_instance *inst, u32 *sec_axi_size);

int wave4_vpu_dec_init_seq(struct vpu_instance *inst)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	u32 bs_param;
	u32 bs_option;
	u32 sec_axi;
	dma_addr_t temp_base = inst->dev->common_mem.daddr + W4_MAX_CODE_BUF_SIZE;
	u32 bs_rd_reg = wave4_dec_bs_rd_ptr_reg(inst->dev);
	u32 bs_wr_reg = wave4_dec_bs_wr_ptr_reg(inst->dev);
	u32 bs_opt_reg = wave4_dec_bs_option_reg(inst->dev);

	if (!inst->codec_info)
		return -EINVAL;

	dev_dbg(inst->dev->dev,
		"%s: bs start=0x%llx size=0x%x rd=0x%llx wr=0x%llx\n",
		__func__,
		(unsigned long long)p_dec_info->stream_buf_start_addr,
		p_dec_info->stream_buf_size,
		(unsigned long long)p_dec_info->stream_rd_ptr,
		(unsigned long long)p_dec_info->stream_wr_ptr);

	bs_param = W4_BS_PARAM_ENABLE_RINGBUFFER |
		   wave4_resolve_bs_endian_nibble();
	vpu_write_reg(inst->dev, W4_BS_PARAM, bs_param);
	vpu_write_reg(inst->dev, W4_BS_START_ADDR,
		      wave4_bs_addr(p_dec_info->stream_buf_start_addr));
	vpu_write_reg(inst->dev, W4_BS_SIZE, p_dec_info->stream_buf_size);
	vpu_write_reg(inst->dev, bs_rd_reg,
		      wave4_bs_ptr_reg_value(p_dec_info->stream_rd_ptr));
	vpu_write_reg(inst->dev, bs_wr_reg,
		      wave4_bs_ptr_reg_value(p_dec_info->stream_wr_ptr));

	bs_option = get_bitstream_options(p_dec_info);
	vpu_write_reg(inst->dev, bs_opt_reg, bs_option);

	dev_dbg(inst->dev->dev,
		"%s: pre-issue bs_param=0x%x bs_opt=0x%x bs_rd=0x%x bs_wr=0x%x cmd_opt=0x%x\n",
		__func__,
		vpu_read_reg(inst->dev, W4_BS_PARAM),
		vpu_read_reg(inst->dev, W4_BS_OPTION),
		vpu_read_reg(inst->dev, W4_BS_RD_PTR),
		vpu_read_reg(inst->dev, W4_BS_WR_PTR),
		INIT_SEQ_NORMAL);
	if (inst->bitstream_vbuf.vaddr) {
		u8 *bs_head = (u8 *)inst->bitstream_vbuf.vaddr;

		dev_dbg(inst->dev->dev,
			"%s: ring head bytes %*ph\n",
			__func__, 16, bs_head);
	}

	/* Secondary AXI setup follows BSP PrepareDecodingPicture() order. */
	sec_axi = (p_dec_info->sec_axi_info.use_bit_enable << 0) |
		  (p_dec_info->sec_axi_info.use_ip_enable << 9) |
		  (p_dec_info->sec_axi_info.use_lf_row_enable << 15);
	sec_axi = wave4_apply_dec_sec_axi_mask(sec_axi);
	vpu_write_reg(inst->dev, W4_ADDR_SEC_AXI, wave4_cmd_addr(inst->dev->sram_buf.daddr));
	vpu_write_reg(inst->dev, W4_SEC_AXI_SIZE,
		      sec_axi ? inst->dev->sram_buf.size : 0);
	vpu_write_reg(inst->dev, W4_USE_SEC_AXI, sec_axi);
	dev_dbg(inst->dev->dev,
		"%s: sec_axi=0x%x addr=0x%llx size=0x%zx\n",
		__func__, sec_axi,
		(unsigned long long)inst->dev->sram_buf.daddr,
		inst->dev->sram_buf.size);

	/* Work/temp buffers follow sec-axi, matching BSP ordering. */
	vpu_write_reg(inst->dev, W4_ADDR_WORK_BASE, wave4_cmd_addr(p_dec_info->vb_work.daddr));
	vpu_write_reg(inst->dev, W4_WORK_SIZE, p_dec_info->vb_work.size);
	vpu_write_reg(inst->dev, W4_WORK_PARAM, 0);
	vpu_write_reg(inst->dev, W4_ADDR_TEMP_BASE, wave4_cmd_addr(temp_base));
	vpu_write_reg(inst->dev, W4_TEMP_SIZE, W4_TEMPBUF_SIZE);
	vpu_write_reg(inst->dev, W4_TEMP_PARAM, 0);

	vpu_write_reg(inst->dev, W4_CMD_DEC_USER_MASK, p_dec_info->user_data_enable);
	vpu_write_reg(inst->dev, W4_CMD_DEC_ADDR_USER_BASE,
		      wave4_cmd_addr(p_dec_info->user_data_buf_addr));
	vpu_write_reg(inst->dev, W4_CMD_DEC_USER_SIZE, p_dec_info->user_data_buf_size);
	vpu_write_reg(inst->dev, W4_CMD_DEC_USER_PARAM, W4_USER_DATA_ENDIAN_LITTLE);
	vpu_write_reg(inst->dev, W4_CMD_DEC_SEVERITY_LEVEL, 0);
	vpu_write_reg(inst->dev, W4_CMD_DEC_DISP_FLAG, 0);

	vpu_write_reg(inst->dev, wave4_dec_cmd_option_reg(inst->dev), INIT_SEQ_NORMAL);
	vpu_write_reg(inst->dev, wave4_dec_force_fb_latency_reg(inst->dev), 0);

	/*
	 * Match BSP flow: DEC_PIC_HDR is issued here without a BUSY wait.
	 * Final success/failure is evaluated after interrupt in get_seq_info().
	 */
	/*
	 * Match BSP Wave4 flow exactly for DEC_PIC_HDR:
	 * issue command and return. Completion is handled by interrupt +
	 * get_seq_info() in the caller.
	 */
	wave4_bit_issue_command(inst->dev, inst, W4_INIT_SEQ);

	/* Wave420L does not provide reliable queue status fields for this path. */
	p_dec_info->instance_queue_count = 0;
	p_dec_info->report_queue_count = 0;

	dev_dbg(inst->dev->dev, "%s: init seq sent (queue %u : %u)\n", __func__,
		p_dec_info->instance_queue_count, p_dec_info->report_queue_count);

	return 0;
}

static void wave4_get_dec_seq_result(struct vpu_instance *inst, struct dec_initial_info *info)
{
	u32 reg_val;
	u32 profile_compatibility_flag;
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;

	p_dec_info->stream_rd_ptr = wave4_dec_get_rd_ptr(inst);
	info->rd_ptr = p_dec_info->stream_rd_ptr;

	p_dec_info->frame_display_flag = vpu_read_reg(inst->dev, W4_RET_DEC_DISP_FLAG);

	reg_val = vpu_read_reg(inst->dev, W4_RET_DEC_PIC_SIZE);
	info->pic_width = ((reg_val >> 16) & 0xffff);
	info->pic_height = (reg_val & 0xffff);
	info->min_frame_buffer_count = vpu_read_reg(inst->dev, W4_RET_DEC_FRAMEBUF_NEEDED);

	reg_val = vpu_read_reg(inst->dev, W4_RET_DEC_CROP_LEFT_RIGHT);
	info->pic_crop_rect.left = (reg_val >> 16) & 0xffff;
	info->pic_crop_rect.right = reg_val & 0xffff;
	reg_val = vpu_read_reg(inst->dev, W4_RET_DEC_CROP_TOP_BOTTOM);
	info->pic_crop_rect.top = (reg_val >> 16) & 0xffff;
	info->pic_crop_rect.bottom = reg_val & 0xffff;

	reg_val = vpu_read_reg(inst->dev, W4_RET_DEC_COLOR_SAMPLE_INFO);
	info->luma_bitdepth = reg_val & 0xf;
	info->chroma_bitdepth = (reg_val >> 4) & 0xf;

	reg_val = vpu_read_reg(inst->dev, W4_RET_DEC_SEQ_PARAM);
	profile_compatibility_flag = (reg_val >> 12) & 0xff;
	info->profile = (reg_val >> 24) & 0x1f;

	if (inst->std == W_HEVC_DEC) {
		/* guessing profile */
		if (!info->profile) {
			if ((profile_compatibility_flag & 0x06) == 0x06)
				info->profile = HEVC_PROFILE_MAIN; /* main profile */
			else if (profile_compatibility_flag & 0x04)
				info->profile = HEVC_PROFILE_MAIN10; /* main10 profile */
			else if (profile_compatibility_flag & 0x08)
				/* main still picture profile */
				info->profile = HEVC_PROFILE_STILLPICTURE;
			else
				info->profile = HEVC_PROFILE_MAIN; /* for old version HM */
		}
	} else if (inst->std == W_AVC_DEC) {
		info->profile = FIELD_GET(SEQ_PARAM_PROFILE_MASK, reg_val);
	}

	/* Wave4 flow does not use the Wave421 task-buffer size query fields. */
}

int wave4_vpu_dec_get_seq_info(struct vpu_instance *inst, struct dec_initial_info *info)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;

	p_dec_info->instance_queue_count = 0;
	p_dec_info->report_queue_count = 0;

	dev_dbg(inst->dev->dev, "%s: init seq complete (queue %u : %u)\n", __func__,
		p_dec_info->instance_queue_count, p_dec_info->report_queue_count);

	if (!vpu_read_reg(inst->dev, W4_RET_SUCCESS)) {
		u32 fail_reason;
		u32 dec_err;

		info->rd_ptr = wave4_dec_get_rd_ptr(inst);
		fail_reason = vpu_read_reg(inst->dev, W4_RET_FAIL_REASON);
		dec_err = vpu_read_reg(inst->dev, W4_RET_DEC_ERR_INFO);
		if (fail_reason == 1) {
			info->seq_init_err_reason = dec_err;
			return -EIO;
		} else {
			info->seq_init_err_reason = fail_reason;
			return -EIO;
		}
	}

	wave4_get_dec_seq_result(inst, info);

	return 0;
}

int wave4_vpu_dec_register_framebuffer(struct vpu_instance *inst, struct frame_buffer *fb_arr,
				       enum tiled_map_type map_type, unsigned int count)
{
	int ret;
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	struct dec_initial_info *init_info = &p_dec_info->initial_info;
	size_t remain, idx, j, i, cnt_8_chunk, size;
	u32 start_no, end_no;
	u32 reg_val, cbcr_interleave, nv21, pic_size;
	u32 addr_y, addr_cb, addr_cr;
	u32 mv_col_size, frame_width, frame_height, fbc_y_tbl_size, fbc_c_tbl_size;
	bool justified = WTL_RIGHT_JUSTIFIED;
	u32 format_no = WTL_PIXEL_8BIT;
	u32 color_format = 0;
	u32 pixel_order = 1;
	u32 bwb_flag = (map_type == LINEAR_FRAME_MAP) ? 1 : 0;

	cbcr_interleave = inst->cbcr_interleave;
	nv21 = inst->nv21;
	mv_col_size = 0;
	fbc_y_tbl_size = 0;
	fbc_c_tbl_size = 0;

	if (map_type >= COMPRESSED_FRAME_MAP) {
		cbcr_interleave = 0;
		nv21 = 0;

		switch (inst->std) {
		case W_HEVC_DEC:
			mv_col_size = W4_DEC_HEVC_BUF_SIZE(init_info->pic_width,
							      init_info->pic_height);
			break;
		case W_AVC_DEC:
			mv_col_size = W4_DEC_AVC_BUF_SIZE(init_info->pic_width,
							     init_info->pic_height);
			break;
		default:
			return -EINVAL;
		}

		if (inst->std == W_HEVC_DEC || inst->std == W_AVC_DEC) {
			size = ALIGN(ALIGN(mv_col_size, 16), BUFFER_MARGIN) + BUFFER_MARGIN;
			ret = wave4_vdi_allocate_array(inst->dev, p_dec_info->vb_mv, count, size);
			if (ret)
				goto free_mv_buffers;
		}

		frame_width = init_info->pic_width;
		frame_height = init_info->pic_height;
		fbc_y_tbl_size = ALIGN(W4_FBC_LUMA_TABLE_SIZE(frame_width, frame_height), 16);
		fbc_c_tbl_size = ALIGN(W4_FBC_CHROMA_TABLE_SIZE(frame_width, frame_height), 16);

		size = ALIGN(fbc_y_tbl_size, BUFFER_MARGIN) + BUFFER_MARGIN;
		ret = wave4_vdi_allocate_array(inst->dev, p_dec_info->vb_fbc_y_tbl, count, size);
		if (ret)
			goto free_fbc_y_tbl_buffers;

		size = ALIGN(fbc_c_tbl_size, BUFFER_MARGIN) + BUFFER_MARGIN;
		ret = wave4_vdi_allocate_array(inst->dev, p_dec_info->vb_fbc_c_tbl, count, size);
		if (ret)
			goto free_fbc_c_tbl_buffers;

		pic_size = (init_info->pic_width << 16) | (init_info->pic_height);
	} else {
		pic_size = (init_info->pic_width << 16) | (init_info->pic_height);

		if (inst->output_format == FORMAT_422)
			color_format = 1;
	}
	vpu_write_reg(inst->dev, W4_PIC_SIZE, pic_size);

	reg_val = (bwb_flag << 28) |
		  (pixel_order << 23) |
		  (justified << 22) |
		  (format_no << 20) |
		  (color_format << 19) |
		  (nv21 << 17) |
		  (cbcr_interleave << 16) |
		  (fb_arr[0].stride);
	vpu_write_reg(inst->dev, W4_COMMON_PIC_INFO, reg_val);

	remain = count;
	cnt_8_chunk = DIV_ROUND_UP(count, 8);
	idx = 0;
	for (j = 0; j < cnt_8_chunk; j++) {
		reg_val = (j == cnt_8_chunk - 1) << 4 | ((j == 0) << 3);
		vpu_write_reg(inst->dev, W4_SFB_OPTION, reg_val);
		start_no = j * 8;
		end_no = start_no + ((remain >= 8) ? 8 : remain) - 1;

		vpu_write_reg(inst->dev, W4_SET_FB_NUM, (start_no << 8) | end_no);

		for (i = 0; i < 8 && i < remain; i++) {
			addr_y = fb_arr[i + start_no].buf_y;
			addr_cb = fb_arr[i + start_no].buf_cb;
			addr_cr = fb_arr[i + start_no].buf_cr;
			vpu_write_reg(inst->dev, W4_ADDR_LUMA_BASE0 + (i << 4), addr_y);
			vpu_write_reg(inst->dev, W4_ADDR_CB_BASE0 + (i << 4), addr_cb);
			if (map_type >= COMPRESSED_FRAME_MAP) {
				/* luma FBC offset table */
				vpu_write_reg(inst->dev, W4_ADDR_FBC_Y_OFFSET0 + (i << 4),
					      p_dec_info->vb_fbc_y_tbl[idx].daddr);
				/* chroma FBC offset table */
				vpu_write_reg(inst->dev, W4_ADDR_FBC_C_OFFSET0 + (i << 4),
					      p_dec_info->vb_fbc_c_tbl[idx].daddr);
				vpu_write_reg(inst->dev, W4_ADDR_MV_COL0 + (i << 2),
					      p_dec_info->vb_mv[idx].daddr);
			} else {
				vpu_write_reg(inst->dev, W4_ADDR_CR_BASE0 + (i << 4), addr_cr);
				vpu_write_reg(inst->dev, W4_ADDR_FBC_C_OFFSET0 + (i << 4), 0);
				vpu_write_reg(inst->dev, W4_ADDR_MV_COL0 + (i << 2), 0);
			}
			idx++;
		}
		remain -= i;

		ret = send_firmware_command(inst, W4_SET_FB, false, NULL, NULL);
		if (ret)
			goto free_buffers;
	}

	reg_val = vpu_read_reg(inst->dev, W4_RET_SUCCESS);
	if (!reg_val) {
		ret = -EIO;
		goto free_buffers;
	}

	return 0;

free_buffers:
free_fbc_c_tbl_buffers:
	for (i = 0; i < count; i++)
		wave4_vdi_free_dma_memory(inst->dev, &p_dec_info->vb_fbc_c_tbl[i]);
free_fbc_y_tbl_buffers:
	for (i = 0; i < count; i++)
		wave4_vdi_free_dma_memory(inst->dev, &p_dec_info->vb_fbc_y_tbl[i]);
free_mv_buffers:
	for (i = 0; i < count; i++)
		wave4_vdi_free_dma_memory(inst->dev, &p_dec_info->vb_mv[i]);
	return ret;
}

static u32 wave4_vpu_dec_validate_sec_axi(struct vpu_instance *inst)
{
	u32 bitdepth = inst->codec_info->dec_info.initial_info.luma_bitdepth;
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	u32 bit_size = 0, ip_size = 0, lf_size = 0, ret = 0;
	u32 sram_size = inst->dev->sram_size;
	u32 width = inst->src_fmt.width;

	if (!sram_size)
		return 0;

	bit_size = DIV_ROUND_UP(width, 16) * 5 * 8;
	ip_size = ALIGN(width, 16) * 2 * bitdepth / 8;
	lf_size = ALIGN(width, 16) * 10 * bitdepth / 8;

	if (p_dec_info->sec_axi_info.use_bit_enable && sram_size >= bit_size) {
		ret |= BIT(0);
		sram_size -= bit_size;
	}

	if (p_dec_info->sec_axi_info.use_ip_enable && sram_size >= ip_size) {
		ret |= BIT(9);
		sram_size -= ip_size;
	}

	if (p_dec_info->sec_axi_info.use_lf_row_enable && sram_size >= lf_size)
		ret |= BIT(15);

	return ret;
}

int wave4_vpu_decode(struct vpu_instance *inst, u32 *fail_res)
{
	u32 reg_val;
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	u32 bs_rd_reg = wave4_dec_bs_rd_ptr_reg(inst->dev);
	u32 bs_wr_reg = wave4_dec_bs_wr_ptr_reg(inst->dev);
	u32 bs_opt_reg = wave4_dec_bs_option_reg(inst->dev);
	int ret;

	vpu_write_reg(inst->dev, W4_BS_PARAM,
		      W4_BS_PARAM_ENABLE_RINGBUFFER |
		      wave4_resolve_bs_endian_nibble());
	vpu_write_reg(inst->dev, W4_BS_START_ADDR,
		      wave4_bs_addr(p_dec_info->stream_buf_start_addr));
	vpu_write_reg(inst->dev, W4_BS_SIZE, p_dec_info->stream_buf_size);
	vpu_write_reg(inst->dev, bs_rd_reg,
		      wave4_bs_ptr_reg_value(p_dec_info->stream_rd_ptr));
	vpu_write_reg(inst->dev, bs_wr_reg,
		      wave4_bs_ptr_reg_value(p_dec_info->stream_wr_ptr));

	vpu_write_reg(inst->dev, bs_opt_reg, get_bitstream_options(p_dec_info));

	/* secondary AXI */
	reg_val = wave4_vpu_dec_validate_sec_axi(inst);
	reg_val = wave4_apply_dec_sec_axi_mask(reg_val);
	vpu_write_reg(inst->dev, W4_USE_SEC_AXI, reg_val);
	vpu_write_reg(inst->dev, W4_ADDR_SEC_AXI, wave4_cmd_addr(inst->dev->sram_buf.daddr));
	vpu_write_reg(inst->dev, W4_SEC_AXI_SIZE,
		      reg_val ? inst->dev->sram_buf.size : 0);

	/* set attributes of user buffer */
	vpu_write_reg(inst->dev, W4_CMD_DEC_ADDR_REPORT_BASE,
		      wave4_cmd_addr(p_dec_info->user_data_buf_addr));
	vpu_write_reg(inst->dev, W4_CMD_DEC_REPORT_SIZE, p_dec_info->user_data_buf_size);
	vpu_write_reg(inst->dev, W4_CMD_DEC_REPORT_PARAM,
		      W4_REPORT_ENDIAN_LE_WORD_BYTE_SWAP);
	vpu_write_reg(inst->dev, W4_CMD_DEC_ADDR_USER_BASE,
		      wave4_cmd_addr(p_dec_info->user_data_buf_addr));
	vpu_write_reg(inst->dev, W4_CMD_DEC_USER_SIZE, p_dec_info->user_data_buf_size);
	vpu_write_reg(inst->dev, W4_CMD_DEC_USER_PARAM, W4_USER_DATA_ENDIAN_LITTLE);
	vpu_write_reg(inst->dev, W4_CMD_DEC_SEVERITY_LEVEL, 0);
	vpu_write_reg(inst->dev, wave4_dec_user_mask_reg(inst->dev),
		      p_dec_info->user_data_enable);
	vpu_write_reg(inst->dev, W4_CMD_DEC_VCORE_LIMIT, 1);

	vpu_write_reg(inst->dev, wave4_dec_cmd_option_reg(inst->dev), DEC_PIC_NORMAL);
	vpu_write_reg(inst->dev, wave4_dec_temp_id_reg(inst->dev),
		      p_dec_info->target_temp_id + 1);
	vpu_write_reg(inst->dev, wave4_dec_seq_change_reg(inst->dev), p_dec_info->seq_change_mask);
	/* When reordering is disabled we force the latency of the framebuffers */
	vpu_write_reg(inst->dev, wave4_dec_force_fb_latency_reg(inst->dev),
		      !p_dec_info->reorder_enable);

	ret = send_firmware_command(inst, W4_DEC_ENC_PIC, true, &reg_val, fail_res);
	if (ret == -ETIMEDOUT)
		return ret;

	/* Wave420L does not provide reliable queue status fields for this path. */
	p_dec_info->instance_queue_count = 0;
	p_dec_info->report_queue_count = 0;

	dev_dbg(inst->dev->dev, "%s: dec pic sent (queue %u : %u)\n", __func__,
		p_dec_info->instance_queue_count, p_dec_info->report_queue_count);

	if (ret)
		return ret;

	return 0;
}

static inline s32 wave4_unpack_dec_index(u32 packed, bool linear_idx)
{
	if (linear_idx)
		return (s16)(packed >> 16);

	return (s16)(packed & 0xffff);
}

int wave4_vpu_dec_get_result(struct vpu_instance *inst, struct dec_output_info *result)
{
	u32 index, nal_unit_type, reg_val, sub_layer_info;
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	bool linear_output = p_dec_info->num_of_display_fbs > 0;
	u32 fail_reason;
	u32 dec_err;

	vpu_write_reg(inst->dev, W4_CMD_DEC_ADDR_REPORT_BASE, p_dec_info->user_data_buf_addr);
	vpu_write_reg(inst->dev, W4_CMD_DEC_REPORT_SIZE, p_dec_info->user_data_buf_size);
	vpu_write_reg(inst->dev, W4_CMD_DEC_REPORT_PARAM,
		      W4_REPORT_ENDIAN_LE_WORD_BYTE_SWAP);

	/*
	 * Wave420L returns DEC_PIC results directly on PIC_RUN completion.
	 * Issuing GET_RESULT here can re-trigger PIC_RUN handling and spin.
	 */
	p_dec_info->instance_queue_count = 0;
	p_dec_info->report_queue_count = 0;

	dev_dbg(inst->dev->dev, "%s: dec pic complete (queue %u : %u)\n", __func__,
		p_dec_info->instance_queue_count, p_dec_info->report_queue_count);

	if (!vpu_read_reg(inst->dev, W4_RET_SUCCESS)) {
		fail_reason = vpu_read_reg(inst->dev, W4_RET_FAIL_REASON);
		dec_err = vpu_read_reg(inst->dev, W4_RET_DEC_ERR_INFO);
		dev_warn(inst->dev->dev,
			 "%s: dec result failed: fail_reason=0x%x dec_err=0x%x\n",
			 __func__, fail_reason, dec_err);
		result->index_frame_display = DISPLAY_IDX_FLAG_NO_FB;
		result->index_frame_decoded = DECODED_IDX_FLAG_NO_FB;
		result->index_frame_decoded_for_tiled = DECODED_IDX_FLAG_NO_FB;
		result->frame_display_flag = vpu_read_reg(inst->dev, W4_RET_DEC_DISP_FLAG);
		result->sequence_changed = 0;
		return -EIO;
	}

	reg_val = vpu_read_reg(inst->dev, W4_RET_DEC_PIC_TYPE);

	nal_unit_type = (reg_val >> 4) & 0x3f;

	if (inst->std == W_HEVC_DEC) {
		if (reg_val & 0x04)
			result->pic_type = PIC_TYPE_B;
		else if (reg_val & 0x02)
			result->pic_type = PIC_TYPE_P;
		else if (reg_val & 0x01)
			result->pic_type = PIC_TYPE_I;
		else
			result->pic_type = PIC_TYPE_MAX;
		if ((nal_unit_type == 19 || nal_unit_type == 20) && result->pic_type == PIC_TYPE_I)
			/* IDR_W_RADL, IDR_N_LP */
			result->pic_type = PIC_TYPE_IDR;
	} else if (inst->std == W_AVC_DEC) {
		if (reg_val & 0x04)
			result->pic_type = PIC_TYPE_B;
		else if (reg_val & 0x02)
			result->pic_type = PIC_TYPE_P;
		else if (reg_val & 0x01)
			result->pic_type = PIC_TYPE_I;
		else
			result->pic_type = PIC_TYPE_MAX;
		if (nal_unit_type == 5 && result->pic_type == PIC_TYPE_I)
			result->pic_type = PIC_TYPE_IDR;
	}
	index = vpu_read_reg(inst->dev, W4_RET_DEC_DISPLAY_INDEX);
	result->index_frame_display = wave4_unpack_dec_index(index, linear_output);
	index = vpu_read_reg(inst->dev, W4_RET_DEC_DECODED_INDEX);
	result->index_frame_decoded = wave4_unpack_dec_index(index, linear_output);
	result->index_frame_decoded_for_tiled = wave4_unpack_dec_index(index, false);

	sub_layer_info = vpu_read_reg(inst->dev, W4_RET_TEMP_SUB_LAYER_INFO);
	result->temporal_id = sub_layer_info & 0xff;

	if (inst->std == W_HEVC_DEC || inst->std == W_AVC_DEC) {
		result->decoded_poc = -1;
		if (result->index_frame_decoded >= 0 ||
		    result->index_frame_decoded == DECODED_IDX_FLAG_SKIP)
			result->decoded_poc = vpu_read_reg(inst->dev, W4_RET_DEC_PIC_POC);
	}

	result->sequence_changed = vpu_read_reg(inst->dev, W4_RET_DEC_SEQ_CHANGE_FLAG) & 0x7fffffff;
	if (!result->sequence_changed) {
		reg_val = vpu_read_reg(inst->dev, W4_RET_DEC_PIC_SIZE);
		result->dec_pic_width = reg_val >> 16;
		result->dec_pic_height = reg_val & 0xffff;
	} else if (result->index_frame_decoded < 0) {
		result->dec_pic_width = 0;
		result->dec_pic_height = 0;
	} else {
		result->dec_pic_width = p_dec_info->initial_info.pic_width;
		result->dec_pic_height = p_dec_info->initial_info.pic_height;
	}
	result->frame_display_flag = vpu_read_reg(inst->dev, W4_RET_DEC_DISP_FLAG);

	if (result->sequence_changed) {
		memcpy((void *)&p_dec_info->new_seq_info, (void *)&p_dec_info->initial_info,
		       sizeof(struct dec_initial_info));
		wave4_get_dec_seq_result(inst, &p_dec_info->new_seq_info);
	}

	result->dec_host_cmd_tick = 0;
	result->dec_decode_end_tick = 0;
	result->frame_cycle = vpu_read_reg(inst->dev, W4_RET_FRAME_CYCLE);
	p_dec_info->first_cycle_check = false;

	return 0;
}

int wave4_vpu_re_init(struct device *dev, u8 *fw, size_t size)
{
	struct vpu_buf *common_vb;
	dma_addr_t code_base;
	dma_addr_t old_code_base, expected_code_base;
	u32 code_size, reason_code;
	size_t code_bytes_required;
	u32 reg_val;
	int ret;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);

	common_vb = &vpu_dev->common_mem;

	code_base = common_vb->daddr;

	code_size = W4_MAX_CODE_BUF_SIZE;

	/* ALIGN TO 4KB */
	code_size &= ~0xfff;
	code_bytes_required = size;
	if (code_size < code_bytes_required)
		return -EINVAL;

	old_code_base = vpu_read_reg(vpu_dev, W4_VPU_REMAP_PADDR);
	expected_code_base = code_base;
	dev_dbg(vpu_dev->dev,
		"w4 reinit: remap old=0x%llx expected=0x%llx code_base=0x%llx fw_size=%zu\n",
		(unsigned long long)old_code_base,
		(unsigned long long)expected_code_base,
		(unsigned long long)code_base, size);

	if (old_code_base != expected_code_base) {
		ret = wave4_vdi_write_memory(vpu_dev, common_vb, 0, fw, size);
		if (ret < 0) {
			dev_err(vpu_dev->dev,
				"VPU init, Writing firmware to common buffer, fail: %d\n", ret);
			return ret;
		}

		vpu_write_reg(vpu_dev, W4_PO_CONF, 0);

		/*
		 * Match Wave4 BSP reset ordering: gate GDI transactions before
		 * asserting reset, then release GDI bus control afterwards.
		 */
		ret = wave4_fio_writel(vpu_dev, W4_GDI_BUS_CTRL, 0x100);
		if (ret)
			return ret;
		ret = wave4_wait_bus_busy(vpu_dev, W4_GDI_BUS_STATUS);
		if (ret) {
			(void)wave4_fio_writel(vpu_dev, W4_GDI_BUS_CTRL, 0x00);
			dev_err(vpu_dev->dev, "VPU reinit(W4 gdi bus idle) timeout\n");
			return ret;
		}

		/*
		 * Follow Wave4 BSP path: assert full block reset directly
		 * before remap/INIT_VPU programming.
		 */
		vpu_write_reg(vpu_dev, W4_VPU_RESET_REQ, W4_RESET_ALL_BLOCKS);
		ret = wave4_wait_vpu_busy(vpu_dev, W4_VPU_RESET_STATUS);
		if (ret) {
			vpu_write_reg(vpu_dev, W4_VPU_RESET_REQ, 0);
			(void)wave4_fio_writel(vpu_dev, W4_GDI_BUS_CTRL, 0x00);
			dev_err(vpu_dev->dev, "VPU reinit(W4 reset-all) timeout\n");
			return ret;
		}
		vpu_write_reg(vpu_dev, W4_VPU_RESET_REQ, 0);
		ret = wave4_fio_writel(vpu_dev, W4_GDI_BUS_CTRL, 0x00);
		if (ret)
			return ret;

		/* clear command window registers */
		for (reg_val = W4_CMD_REG_BASE; reg_val < W4_CMD_REG_END; reg_val += 4)
			vpu_write_reg(vpu_dev, reg_val, 0x00);

		{
			u32 remap_size = (code_size >> 12) & 0x1ff;

			reg_val = BIT(31) | (W4_REMAP_INDEX0 << 12) | BIT(11) | remap_size;
			vpu_write_reg(vpu_dev, W4_VPU_REMAP_CTRL, reg_val);
			vpu_write_reg(vpu_dev, W4_VPU_REMAP_VADDR, 0x00000000);
			vpu_write_reg(vpu_dev, W4_VPU_REMAP_PADDR, code_base);

			vpu_write_reg(vpu_dev, W4_ADDR_CODE_BASE, code_base);
			vpu_write_reg(vpu_dev, W4_CODE_SIZE, code_size);
			vpu_write_reg(vpu_dev, W4_CODE_PARAM, 0);
			vpu_write_reg(vpu_dev, W4_TIMEOUT_CNT, 0xffffffff);
			vpu_write_reg(vpu_dev, W4_HW_OPTION, 0);
		}

		setup_wave4_interrupts(vpu_dev);
		vpu_write_reg(vpu_dev, W4_RET_SUCCESS, 0);
		vpu_write_reg(vpu_dev, W4_CORE_INDEX, 0);
		vpu_write_reg(vpu_dev, W4_INST_INDEX, 0);

		vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 1);
		vpu_write_reg(vpu_dev, W4_COMMAND, wave4_hw_command(vpu_dev, W4_INIT_VPU));
		vpu_write_reg(vpu_dev, W4_VPU_REMAP_CORE_START, 1);

		ret = wave4_wait_vpu_busy(vpu_dev, W4_VPU_BUSY_STATUS);
		if (ret) {
			u32 vcpu_pc = vpu_read_reg(vpu_dev, W4_VCPU_CUR_PC);

			dev_err(vpu_dev->dev, "VPU reinit(W4_VPU_REMAP_CORE_START) timeout\n");
			if (vcpu_pc && READ_ONCE(wave4_allow_resident_fw_fallback)) {
				dev_warn(vpu_dev->dev,
					 "w4 reinit timeout with live VCPU (pc=0x%x), clearing BUSY and using resident firmware\n",
					 vcpu_pc);
				vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 0);
				ret = setup_wave4_properties(dev);
				if (!ret)
					vpu_dev->fw_running = true;
				return ret;
			} else if (vcpu_pc) {
				dev_warn(vpu_dev->dev,
					 "w4 reinit timeout with live VCPU (pc=0x%x), resident fallback disabled\n",
					 vcpu_pc);
			}
			return ret;
		}

		ret = wave4_vpu_firmware_command_queue_error_check(vpu_dev, &reason_code);
		if (ret)
			return ret;
	}

	ret = setup_wave4_properties(dev);
	if (!ret)
		vpu_dev->fw_running = true;

	return ret;
}

int wave4_vpu_sleep_wake(struct device *dev, bool i_sleep_wake, const uint16_t *code,
			 size_t size)
{
	u32 reg_val, remap_size;
	struct vpu_buf *common_vb;
	dma_addr_t code_base;
	u32 code_size, reason_code;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	int ret;

	(void)code;

	if (i_sleep_wake) {
		ret = wave4_wait_vpu_busy(vpu_dev, W4_VPU_BUSY_STATUS);
		if (ret)
			return ret;

		/*
		 * Declare who has ownership for the host interface access
		 * 1 = VPU
		 * 0 = Host processor
		 */
		vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 1);
		vpu_write_reg(vpu_dev, W4_COMMAND, wave4_hw_command(vpu_dev, W4_SLEEP_VPU));
		/* Send an interrupt named HOST to the VPU */
		vpu_write_reg(vpu_dev, W4_VPU_HOST_INT_REQ, 1);

		ret = wave4_wait_vpu_busy(vpu_dev, W4_VPU_BUSY_STATUS);
		if (ret)
			return ret;

		ret = wave4_vpu_firmware_command_queue_error_check(vpu_dev, &reason_code);
		if (ret)
			return ret;
	} else { /* restore */
		common_vb = &vpu_dev->common_mem;

		code_base = common_vb->daddr;
		code_size = W4_MAX_CODE_BUF_SIZE;

		/* ALIGN TO 4KB */
		code_size &= ~0xfff;
		if (code_size < size) {
			dev_err(dev, "size too small\n");
			return -EINVAL;
		}

		/* Power on without DEBUG mode */
		vpu_write_reg(vpu_dev, W4_PO_CONF, 0);

		remap_size = (code_size >> 12) & 0x1ff;
		reg_val = BIT(31) | (W4_REMAP_INDEX0 << 12) | BIT(11) | remap_size;
		vpu_write_reg(vpu_dev, W4_VPU_REMAP_CTRL, reg_val);
		vpu_write_reg(vpu_dev, W4_VPU_REMAP_VADDR, 0x00000000);
		vpu_write_reg(vpu_dev, W4_VPU_REMAP_PADDR, code_base);

		vpu_write_reg(vpu_dev, W4_ADDR_CODE_BASE, code_base);
		vpu_write_reg(vpu_dev, W4_CODE_SIZE, code_size);
		vpu_write_reg(vpu_dev, W4_CODE_PARAM, 0);
		vpu_write_reg(vpu_dev, W4_TIMEOUT_CNT, 0xffffffff);
		vpu_write_reg(vpu_dev, W4_HW_OPTION, 0);

		setup_wave4_interrupts(vpu_dev);
		vpu_write_reg(vpu_dev, W4_RET_SUCCESS, 0);
		vpu_write_reg(vpu_dev, W4_CORE_INDEX, 0);
		vpu_write_reg(vpu_dev, W4_INST_INDEX, 0);

		vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 1);
		vpu_write_reg(vpu_dev, W4_COMMAND, wave4_hw_command(vpu_dev, W4_WAKEUP_VPU));
		vpu_write_reg(vpu_dev, W4_VPU_HOST_INT_REQ, 1);
		/* Start VPU after settings */
		vpu_write_reg(vpu_dev, W4_VPU_REMAP_CORE_START, 1);

		ret = wave4_wait_vpu_busy(vpu_dev, W4_VPU_BUSY_STATUS);
		if (ret) {
			dev_err(vpu_dev->dev, "VPU wakeup(W4_VPU_REMAP_CORE_START) timeout\n");
			return ret;
		}

		return wave4_vpu_firmware_command_queue_error_check(vpu_dev, &reason_code);
	}

	return 0;
}

int wave4_vpu_reset(struct device *dev, enum sw_reset_mode reset_mode)
{
	u32 val = 0;
	int ret = 0;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	struct vpu_attr *p_attr = &vpu_dev->attr;
	/* VPU doesn't send response. force to set BUSY flag to 0. */
	vpu_write_reg(vpu_dev, W4_VPU_BUSY_STATUS, 0);

	if (reset_mode == SW_RESET_SAFETY) {
		ret = wave4_vpu_sleep_wake(dev, true, NULL, 0);
		if (ret)
			return ret;
	}

	val = vpu_read_reg(vpu_dev, W4_VPU_RET_VPU_CONFIG0);
	if ((val >> 16) & 0x1)
		p_attr->support_backbone = true;
	if ((val >> 22) & 0x1)
		p_attr->support_vcore_backbone = true;
	if ((val >> 28) & 0x1)
		p_attr->support_vcpu_backbone = true;

	/* waiting for completion of bus transaction */
	if (p_attr->support_backbone) {
		dev_dbg(dev, "%s: backbone supported\n", __func__);

		if (p_attr->support_vcore_backbone) {
			if (p_attr->support_vcpu_backbone) {
				/* step1 : disable request */
				ret = wave4_fio_writel(vpu_dev, W4_BACKBONE_BUS_CTRL_VCPU, 0xFF);
				if (ret)
					return ret;

				/* step2 : waiting for completion of bus transaction */
				ret = wave4_wait_vcpu_bus_busy(vpu_dev,
							       W4_BACKBONE_BUS_STATUS_VCPU);
				if (ret) {
					(void)wave4_fio_writel(vpu_dev,
							       W4_BACKBONE_BUS_CTRL_VCPU, 0x00);
					return ret;
				}
			}
			/* step1 : disable request */
			ret = wave4_fio_writel(vpu_dev, W4_BACKBONE_BUS_CTRL_VCORE0, 0x7);
			if (ret)
				return ret;

			/* step2 : waiting for completion of bus transaction */
			if (wave4_wait_bus_busy(vpu_dev, W4_BACKBONE_BUS_STATUS_VCORE0)) {
				(void)wave4_fio_writel(vpu_dev, W4_BACKBONE_BUS_CTRL_VCORE0, 0x00);
				return -EBUSY;
			}
		} else {
			/* step1 : disable request */
			ret = wave4_fio_writel(vpu_dev, W4_COMBINED_BACKBONE_BUS_CTRL, 0x7);
			if (ret)
				return ret;

			/* step2 : waiting for completion of bus transaction */
			if (wave4_wait_bus_busy(vpu_dev, W4_COMBINED_BACKBONE_BUS_STATUS)) {
				(void)wave4_fio_writel(vpu_dev, W4_COMBINED_BACKBONE_BUS_CTRL, 0x00);
				return -EBUSY;
			}
		}
	} else {
		dev_dbg(dev, "%s: backbone NOT supported\n", __func__);
		/* step1 : disable request */
		ret = wave4_fio_writel(vpu_dev, W4_GDI_BUS_CTRL, 0x100);
		if (ret)
			return ret;

		/* step2 : waiting for completion of bus transaction */
		ret = wave4_wait_bus_busy(vpu_dev, W4_GDI_BUS_STATUS);
		if (ret) {
			(void)wave4_fio_writel(vpu_dev, W4_GDI_BUS_CTRL, 0x00);
			return ret;
		}
	}

	switch (reset_mode) {
	case SW_RESET_ON_BOOT:
	case SW_RESET_FORCE:
	case SW_RESET_SAFETY:
		val = W4_RST_BLOCK_ALL;
		break;
	default:
		return -EINVAL;
	}

	if (val) {
		vpu_write_reg(vpu_dev, W4_VPU_RESET_REQ, val);

		ret = wave4_wait_vpu_busy(vpu_dev, W4_VPU_RESET_STATUS);
		if (ret) {
			vpu_write_reg(vpu_dev, W4_VPU_RESET_REQ, 0);
			return ret;
		}
		vpu_write_reg(vpu_dev, W4_VPU_RESET_REQ, 0);
	}
	/* step3 : must clear GDI_BUS_CTRL after done SW_RESET */
	if (p_attr->support_backbone) {
		if (p_attr->support_vcore_backbone) {
			if (p_attr->support_vcpu_backbone) {
				ret = wave4_fio_writel(vpu_dev, W4_BACKBONE_BUS_CTRL_VCPU, 0x00);
				if (ret)
					return ret;
			}
			ret = wave4_fio_writel(vpu_dev, W4_BACKBONE_BUS_CTRL_VCORE0, 0x00);
		} else {
			ret = wave4_fio_writel(vpu_dev, W4_COMBINED_BACKBONE_BUS_CTRL, 0x00);
		}
	} else {
		ret = wave4_fio_writel(vpu_dev, W4_GDI_BUS_CTRL, 0x00);
	}
	if (ret)
		return ret;
	if (reset_mode == SW_RESET_SAFETY || reset_mode == SW_RESET_FORCE)
		ret = wave4_vpu_sleep_wake(dev, false, NULL, 0);

	return ret;
}

int wave4_vpu_dec_finish_seq(struct vpu_instance *inst, u32 *fail_res)
{
	return send_firmware_command(inst, W4_DESTROY_INSTANCE, true, NULL, fail_res);
}

int wave4_vpu_dec_set_bitstream_flag(struct vpu_instance *inst, bool eos)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	u32 bs_opt_reg = wave4_dec_bs_option_reg(inst->dev);
	u32 bs_wr_reg = wave4_dec_bs_wr_ptr_reg(inst->dev);
	bool running = (inst->state == VPU_INST_STATE_PIC_RUN);

	p_dec_info->stream_endflag = eos ? 1 : 0;
	/*
	 * Match BSP Wave4 semantics:
	 * update BS_OPTION only while decode is running (PIC_RUN path).
	 * WR_PTR is host-owned and advanced through INIT/PIC command setup.
	 */
	if (!running)
		return 0;

	vpu_write_reg(inst->dev, bs_opt_reg, get_bitstream_options(p_dec_info));
	vpu_write_reg(inst->dev, bs_wr_reg,
		      wave4_bs_ptr_reg_value(p_dec_info->stream_wr_ptr));

	/*
	 * Wave4 updates bitstream state via register writes only.
	 * W4_UPDATE_BS is not valid in this command space.
	 */
	return 0;
}

int wave4_dec_clr_disp_flag(struct vpu_instance *inst, unsigned int index)
{
	struct dec_info *p_dec_info = &inst->codec_info->dec_info;
	int ret;

	vpu_write_reg(inst->dev, W4_CMD_DEC_CLR_DISP_IDC, BIT(index));
	vpu_write_reg(inst->dev, W4_CMD_DEC_SET_DISP_IDC, 0);

	ret = wave4_send_query(inst->dev, inst, UPDATE_DISP_FLAG);
	if (ret)
		return ret;

	p_dec_info->frame_display_flag = vpu_read_reg(inst->dev, W4_RET_DEC_DISP_IDC);

	return 0;
}

int wave4_dec_set_disp_flag(struct vpu_instance *inst, unsigned int index)
{
	int ret;

	vpu_write_reg(inst->dev, W4_CMD_DEC_CLR_DISP_IDC, 0);
	vpu_write_reg(inst->dev, W4_CMD_DEC_SET_DISP_IDC, BIT(index));

	ret = wave4_send_query(inst->dev, inst, UPDATE_DISP_FLAG);
	if (ret)
		return ret;

	return 0;
}

int wave4_vpu_clear_interrupt(struct vpu_instance *inst, u32 flags)
{
	u32 interrupt_reason;

	interrupt_reason = vpu_read_reg(inst->dev, W4_VPU_VINT_REASON_USR);
	interrupt_reason &= ~flags;
	vpu_write_reg(inst->dev, W4_VPU_VINT_REASON_USR, interrupt_reason);

	return 0;
}

dma_addr_t wave4_dec_get_rd_ptr(struct vpu_instance *inst)
{
	struct dec_info *p_dec_info;
	dma_addr_t rd_ptr;
	dma_addr_t start;
	dma_addr_t end;

	rd_ptr = vpu_read_reg(inst->dev, W4_BS_RD_PTR);
	if (!inst->codec_info)
		return rd_ptr;

	p_dec_info = &inst->codec_info->dec_info;
	if (!p_dec_info->stream_buf_size)
		return rd_ptr;

	start = p_dec_info->stream_buf_start_addr;
	end = start + p_dec_info->stream_buf_size;

	if (rd_ptr >= start && rd_ptr < end)
		return rd_ptr;

	return rd_ptr;
}

int wave4_dec_set_rd_ptr(struct vpu_instance *inst, dma_addr_t addr)
{
	vpu_write_reg(inst->dev, W4_BS_RD_PTR,
		      wave4_bs_ptr_reg_value(addr));
	return 0;
}

/************************************************************************/
/* ENCODER functions */
/************************************************************************/

int wave4_vpu_build_up_enc_param(struct device *dev, struct vpu_instance *inst,
				 struct enc_open_param *open_param)
{
	int ret;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	dma_addr_t buffer_addr;
	size_t buffer_size;

	p_enc_info->cycle_per_tick = 256;
	if (vpu_dev->sram_buf.size) {
		/* Wave4 HEVC encode uses bit9(bit-imd), bit11(rdo), bit15(lf). */
		p_enc_info->sec_axi_info.use_ip_enable = 1;
		p_enc_info->sec_axi_info.use_enc_rdo_enable = 1;
		p_enc_info->sec_axi_info.use_enc_lf_enable = 1;
	}

	p_enc_info->vb_work.size = W4_ENC_WORKBUF_SIZE;
	ret = wave4_vdi_allocate_dma_memory(vpu_dev, &p_enc_info->vb_work);
	if (ret) {
		memset(&p_enc_info->vb_work, 0, sizeof(p_enc_info->vb_work));
		return ret;
	}

	wave4_vdi_clear_memory(vpu_dev, &p_enc_info->vb_work);

	vpu_write_reg(inst->dev, W4_ADDR_WORK_BASE, p_enc_info->vb_work.daddr);
	vpu_write_reg(inst->dev, W4_WORK_SIZE, p_enc_info->vb_work.size);
	vpu_write_reg(inst->dev, W4_WORK_PARAM, 0);
	ret = send_firmware_command(inst, W4_CREATE_INSTANCE, true, NULL, NULL);
	if (ret)
		goto free_vb_work;

	buffer_addr = open_param->bitstream_buffer;
	buffer_size = open_param->bitstream_buffer_size;
	p_enc_info->stream_rd_ptr = buffer_addr;
	p_enc_info->stream_wr_ptr = buffer_addr;
	p_enc_info->line_buf_int_en = open_param->line_buf_int_en;
	p_enc_info->stream_buf_start_addr = buffer_addr;
	p_enc_info->stream_buf_size = buffer_size;
	p_enc_info->stream_buf_end_addr = buffer_addr + buffer_size;
	p_enc_info->stride = 0;
	p_enc_info->initial_info_obtained = false;
	p_enc_info->product_code = vpu_read_reg(inst->dev, W4_PRODUCT_NUMBER);

	return 0;
free_vb_work:
	if (wave4_vdi_free_dma_memory(vpu_dev, &p_enc_info->vb_work))
		memset(&p_enc_info->vb_work, 0, sizeof(p_enc_info->vb_work));
	return ret;
}

static void wave4_set_enc_crop_info(u32 codec, struct enc_wave_param *param, int rot_mode,
				    int src_width, int src_height)
{
	int aligned_width = (codec == W_HEVC_ENC) ? ALIGN(src_width, 32) : ALIGN(src_width, 16);
	int aligned_height = (codec == W_HEVC_ENC) ? ALIGN(src_height, 32) : ALIGN(src_height, 16);
	int pad_right, pad_bot;
	int crop_right, crop_left, crop_top, crop_bot;
	int prp_mode = rot_mode >> 1; /* remove prp_enable bit */

	if (codec == W_HEVC_ENC &&
	    (!rot_mode || prp_mode == 14)) /* prp_mode 14 : hor_mir && ver_mir && rot_180 */
		return;

	pad_right = aligned_width - src_width;
	pad_bot = aligned_height - src_height;

	if (param->conf_win_right > 0)
		crop_right = param->conf_win_right + pad_right;
	else
		crop_right = pad_right;

	if (param->conf_win_bot > 0)
		crop_bot = param->conf_win_bot + pad_bot;
	else
		crop_bot = pad_bot;

	crop_top = param->conf_win_top;
	crop_left = param->conf_win_left;

	param->conf_win_top = crop_top;
	param->conf_win_left = crop_left;
	param->conf_win_bot = crop_bot;
	param->conf_win_right = crop_right;

	switch (prp_mode) {
	case 0:
		return;
	case 1:
	case 15:
		param->conf_win_top = crop_right;
		param->conf_win_left = crop_top;
		param->conf_win_bot = crop_left;
		param->conf_win_right = crop_bot;
		break;
	case 2:
	case 12:
		param->conf_win_top = crop_bot;
		param->conf_win_left = crop_right;
		param->conf_win_bot = crop_top;
		param->conf_win_right = crop_left;
		break;
	case 3:
	case 13:
		param->conf_win_top = crop_left;
		param->conf_win_left = crop_bot;
		param->conf_win_bot = crop_right;
		param->conf_win_right = crop_top;
		break;
	case 4:
	case 10:
		param->conf_win_top = crop_bot;
		param->conf_win_bot = crop_top;
		break;
	case 8:
	case 6:
		param->conf_win_left = crop_right;
		param->conf_win_right = crop_left;
		break;
	case 5:
	case 11:
		param->conf_win_top = crop_left;
		param->conf_win_left = crop_top;
		param->conf_win_bot = crop_right;
		param->conf_win_right = crop_bot;
		break;
	case 7:
	case 9:
		param->conf_win_top = crop_right;
		param->conf_win_left = crop_bot;
		param->conf_win_bot = crop_left;
		param->conf_win_right = crop_top;
		break;
	default:
		WARN(1, "Invalid prp_mode: %d, must be in range of 1 - 15\n", prp_mode);
	}
}

int wave4_vpu_enc_init_seq(struct vpu_instance *inst)
{
	u32 reg_val = 0, rot_mir_mode, fixed_cu_size_mode = 0x7;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	struct enc_open_param *p_open_param = &p_enc_info->open_param;
	struct enc_wave_param *p_param = &p_open_param->wave_param;

	/*
	 * OPT_COMMON:
	 *	the last SET_PARAM command should be called with OPT_COMMON
	 */
	rot_mir_mode = 0;
	if (p_enc_info->rotation_enable) {
		switch (p_enc_info->rotation_angle) {
		case 0:
			rot_mir_mode |= NONE_ROTATE;
			break;
		case 90:
			rot_mir_mode |= ROT_CLOCKWISE_90;
			break;
		case 180:
			rot_mir_mode |= ROT_CLOCKWISE_180;
			break;
		case 270:
			rot_mir_mode |= ROT_CLOCKWISE_270;
			break;
		}
	}

	if (p_enc_info->mirror_enable) {
		switch (p_enc_info->mirror_direction) {
		case MIRDIR_NONE:
			rot_mir_mode |= NONE_ROTATE;
			break;
		case MIRDIR_VER:
			rot_mir_mode |= MIR_VER_FLIP;
			break;
		case MIRDIR_HOR:
			rot_mir_mode |= MIR_HOR_FLIP;
			break;
		case MIRDIR_HOR_VER:
			rot_mir_mode |= MIR_HOR_VER_FLIP;
			break;
		}
	}

	wave4_set_enc_crop_info(inst->std, p_param, rot_mir_mode, p_open_param->pic_width,
				p_open_param->pic_height);

	{
		u32 src_width, src_height, frame_rate = 0, sec_axi, sec_axi_size = 0;
		u32 chroma_format_idc;
		dma_addr_t temp_base;
		bool rc_enabled = p_open_param->rc_enable;

		/*
		 * Wave420L uses the legacy Wave4 command window and expects
		 * BS/WORK/TEMP setup before SET_PARAM.
		 */
		src_width = ALIGN(p_open_param->pic_width, 8);
		src_height = ALIGN(p_open_param->pic_height, 8);
		chroma_format_idc = (p_open_param->src_format == FORMAT_422) ? 1 : 0;
		temp_base = inst->dev->common_mem.daddr + W4_MAX_CODE_BUF_SIZE;

		vpu_write_reg(inst->dev, W4_BS_START_ADDR, p_enc_info->stream_buf_start_addr);
		vpu_write_reg(inst->dev, W4_BS_SIZE, p_enc_info->stream_buf_size);
		vpu_write_reg(inst->dev, W4_BS_RD_PTR, p_enc_info->stream_rd_ptr);
		vpu_write_reg(inst->dev, W4_BS_WR_PTR, p_enc_info->stream_wr_ptr);
		vpu_write_reg(inst->dev, W4_BS_PARAM,
			      (p_enc_info->line_buf_int_en << 6) |
			      wave4_resolve_bs_endian_nibble());

		sec_axi = wave4_vpu_enc_validate_sec_axi(inst, &sec_axi_size);
		sec_axi = wave4_apply_enc_sec_axi_mask(sec_axi);
		vpu_write_reg(inst->dev, W4_ADDR_SEC_AXI,
			      inst->dev->sram_buf.daddr);
		vpu_write_reg(inst->dev, W4_SEC_AXI_SIZE,
			      sec_axi ? sec_axi_size : 0);
		vpu_write_reg(inst->dev, W4_USE_SEC_AXI, sec_axi);

		vpu_write_reg(inst->dev, W4_ADDR_WORK_BASE, p_enc_info->vb_work.daddr);
		vpu_write_reg(inst->dev, W4_WORK_SIZE, p_enc_info->vb_work.size);
		vpu_write_reg(inst->dev, W4_WORK_PARAM, 0);
		vpu_write_reg(inst->dev, W4_ADDR_TEMP_BASE, temp_base);
		vpu_write_reg(inst->dev, W4_TEMP_SIZE, W4_TEMPBUF_SIZE);
		vpu_write_reg(inst->dev, W4_TEMP_PARAM, 0);

		vpu_write_reg(inst->dev, W4_COMMAND_OPTION, OPT_COMMON);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SET_PARAM_ENABLE,
			      W4_ENC_SET_PARAM_ENABLE_ALL);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_SRC_SIZE, src_height << 16 |
			      src_width);

		reg_val = (p_param->profile << 0) |
			  (p_param->level << 3) |
			  (p_param->tier << 12) |
			  (p_param->internal_bit_depth << 14) |
			  (chroma_format_idc << 18) |
			  (p_param->lossless_enable << 20) |
			  (p_param->const_intra_pred_flag << 21) |
			  ((p_param->chroma_cb_qp_offset & 0x1f) << 22) |
			  ((p_param->chroma_cr_qp_offset & 0x1f) << 27);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_PIC_PARAM, reg_val);

		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_GOP_PARAM, p_param->gop_preset_idx);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_LAYER_PERIOD, 0);

		reg_val = (rc_enabled << 0) |
			  (p_param->cu_level_rc_enable << 1) |
			  (p_param->hvs_qp_enable << 2) |
			  (!!p_param->hvs_qp_enable && !!p_param->hvs_qp_scale) << 3 |
			  (p_param->hvs_qp_scale << 4) |
			  (p_param->bit_alloc_mode << 7) |
			  (p_param->init_buf_level_x8 << 9) |
			  ((p_param->initial_rc_qp & 0x3f) << 14) |
			  (p_open_param->initial_delay << 20);
		vpu_write_reg(inst->dev, W4_CMD_ENC_RC_PARAM, reg_val);

		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_INTRA_PARAM,
			      (p_param->decoding_refresh_type << 0) |
			      (p_param->intra_qp << ENC_HEVC_INTRA_QP_SHIFT) |
			      (p_param->forced_idr_header_enable <<
			       ENC_HEVC_FORCED_IDR_HEADER_SHIFT) |
			      (p_param->intra_period << ENC_HEVC_INTRA_PERIOD_SHIFT));
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CONF_WIN_TOP_BOT,
			      p_param->conf_win_bot << 16 | p_param->conf_win_top);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CONF_WIN_LEFT_RIGHT,
			      p_param->conf_win_right << 16 | p_param->conf_win_left);

		if ((p_open_param->frame_rate_info >> 16) + 1)
			frame_rate = (p_open_param->frame_rate_info & 0xffff) /
				     ((p_open_param->frame_rate_info >> 16) + 1);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_FRAME_RATE, frame_rate);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_INDEPENDENT_SLICE,
			      p_param->independ_slice_mode_arg << 16 |
			      p_param->independ_slice_mode);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_DEPENDENT_SLICE,
			      p_param->depend_slice_mode_arg << 16 | p_param->depend_slice_mode);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_INTRA_REFRESH,
			      p_param->intra_refresh_arg << 16 | p_param->intra_refresh_mode);

		reg_val = (fixed_cu_size_mode << 4) |
			  (p_param->max_num_merge << 9) |
			  (p_param->disable_deblk << 15) |
			  (p_param->lf_cross_slice_boundary_enable << 16) |
			  ((p_param->beta_offset_div2 & 0xf) << 17) |
			  ((p_param->tc_offset_div2 & 0xf) << 21) |
			  (p_param->skip_intra_trans << 25) |
			  (p_param->sao_enable << 26) |
			  (p_param->tmvp_enable << 7) |
			  (p_param->wpp_enable << 8) |
			  (p_param->intra_nx_n_enable << 28);
		vpu_write_reg(inst->dev, W4_CMD_ENC_PARAM, reg_val);

		reg_val = (p_param->min_qp_p << 0) |
			  (p_param->max_qp_p << 6) |
			  (p_param->hvs_max_delta_qp << 12);
		vpu_write_reg(inst->dev, W4_CMD_ENC_RC_MIN_MAX_QP, reg_val);
		vpu_write_reg(inst->dev, W4_CMD_ENC_RC_BIT_RATIO_LAYER_0_3, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_RC_BIT_RATIO_LAYER_4_7, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_NR_PARAM, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_NR_WEIGHT,
			      p_param->nr_intra_weight_y |
			      (p_param->nr_intra_weight_cb << 5) |
			      (p_param->nr_intra_weight_cr << 10) |
			      (p_param->nr_inter_weight_y << 15) |
			      (p_param->nr_inter_weight_cb << 20) |
			      (p_param->nr_inter_weight_cr << 25));
		vpu_write_reg(inst->dev, W4_CMD_ENC_RC_TARGET_RATE,
			      p_open_param->bit_rate);
		vpu_write_reg(inst->dev, W4_CMD_ENC_RC_TRANS_RATE,
			      p_param->trans_rate);
		vpu_write_reg(inst->dev, W4_CMD_ENC_ROT_PARAM, rot_mir_mode);
		vpu_write_reg(inst->dev, W4_CMD_ENC_NUM_UNITS_IN_TICK, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_TIME_SCALE, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_NUM_TICKS_POC_DIFF_ONE, 0);

		return send_firmware_command(inst, W4_ENC_SET_PARAM, false, NULL, NULL);
	}

	/* SET_PARAM + COMMON */
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_SET_PARAM_OPTION, OPT_COMMON);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_SRC_SIZE, p_open_param->pic_height << 16
			| p_open_param->pic_width);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CUSTOM_MAP_ENDIAN, VDI_LITTLE_ENDIAN);

	reg_val = p_param->profile |
		(p_param->level << 3) |
		(p_param->internal_bit_depth << 14);
	if (inst->std == W_HEVC_ENC)
		reg_val |= (p_param->tier << 12) |
			(p_param->tmvp_enable << 23) |
			(p_param->sao_enable << 24) |
			(p_param->skip_intra_trans << 25) |
			(p_param->strong_intra_smooth_enable << 27) |
			(p_param->en_still_picture << 30);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_SPS_PARAM, reg_val);

	reg_val = (p_param->lossless_enable) |
		(p_param->const_intra_pred_flag << 1) |
		(p_param->lf_cross_slice_boundary_enable << 2) |
		(p_param->wpp_enable << 4) |
		(p_param->disable_deblk << 5) |
		((p_param->beta_offset_div2 & 0xF) << 6) |
		((p_param->tc_offset_div2 & 0xF) << 10) |
		((p_param->chroma_cb_qp_offset & 0x1F) << 14) |
		((p_param->chroma_cr_qp_offset & 0x1F) << 19) |
		(p_param->transform8x8_enable << 29) |
		(p_param->entropy_coding_mode << 30);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_PPS_PARAM, reg_val);

	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_GOP_PARAM, p_param->gop_preset_idx);

	if (inst->std == W_AVC_ENC)
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_INTRA_PARAM, p_param->intra_qp |
			      ((p_param->intra_period & ENC_AVC_INTRA_IDR_PARAM_MASK)
				<< ENC_AVC_INTRA_PERIOD_SHIFT) |
			      ((p_param->avc_idr_period & ENC_AVC_INTRA_IDR_PARAM_MASK)
				<< ENC_AVC_IDR_PERIOD_SHIFT) |
			      (p_param->forced_idr_header_enable
			       << ENC_AVC_FORCED_IDR_HEADER_SHIFT));
	else if (inst->std == W_HEVC_ENC)
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_INTRA_PARAM,
			      p_param->decoding_refresh_type |
			      (p_param->intra_qp << ENC_HEVC_INTRA_QP_SHIFT) |
			      (p_param->forced_idr_header_enable
			       << ENC_HEVC_FORCED_IDR_HEADER_SHIFT) |
			      (p_param->intra_period << ENC_HEVC_INTRA_PERIOD_SHIFT));

	reg_val = (p_param->rdo_skip << 2) |
		(p_param->lambda_scaling_enable << 3) |
		(fixed_cu_size_mode << 5) |
		(p_param->intra_nx_n_enable << 8) |
		(p_param->max_num_merge << 18);

	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_RDO_PARAM, reg_val);

	if (inst->std == W_AVC_ENC)
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_INTRA_REFRESH,
			      p_param->intra_mb_refresh_arg << 16 | p_param->intra_mb_refresh_mode);
	else if (inst->std == W_HEVC_ENC)
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_INTRA_REFRESH,
			      p_param->intra_refresh_arg << 16 | p_param->intra_refresh_mode);

	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_RC_FRAME_RATE, p_open_param->frame_rate_info);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_RC_TARGET_RATE, p_open_param->bit_rate);

	reg_val = p_open_param->rc_enable |
		(p_param->hvs_qp_enable << 2) |
		(p_param->hvs_qp_scale << 4) |
		((p_param->initial_rc_qp & 0x3F) << 14) |
		(p_open_param->vbv_buffer_size << 20);
	if (inst->std == W_AVC_ENC)
		reg_val |= (p_param->mb_level_rc_enable << 1);
	else if (inst->std == W_HEVC_ENC)
		reg_val |= (p_param->cu_level_rc_enable << 1);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_RC_PARAM, reg_val);

	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_RC_WEIGHT_PARAM,
		      p_param->rc_weight_buf << 8 | p_param->rc_weight_param);

	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_RC_MIN_MAX_QP, p_param->min_qp_i |
		      (p_param->max_qp_i << 6) | (p_param->hvs_max_delta_qp << 12));

	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_RC_INTER_MIN_MAX_QP, p_param->min_qp_p |
		      (p_param->max_qp_p << 6) | (p_param->min_qp_b << 12) |
		      (p_param->max_qp_b << 18));

	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_RC_BIT_RATIO_LAYER_0_3, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_RC_BIT_RATIO_LAYER_4_7, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_ROT_PARAM, rot_mir_mode);

	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_BG_PARAM, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CUSTOM_LAMBDA_ADDR, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CONF_WIN_TOP_BOT,
		      p_param->conf_win_bot << 16 | p_param->conf_win_top);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CONF_WIN_LEFT_RIGHT,
		      p_param->conf_win_right << 16 | p_param->conf_win_left);

	if (inst->std == W_AVC_ENC)
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_INDEPENDENT_SLICE,
			      p_param->avc_slice_arg << 16 | p_param->avc_slice_mode);
	else if (inst->std == W_HEVC_ENC)
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_INDEPENDENT_SLICE,
			      p_param->independ_slice_mode_arg << 16 |
			      p_param->independ_slice_mode);

	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_USER_SCALING_LIST_ADDR, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_NUM_UNITS_IN_TICK, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_TIME_SCALE, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_NUM_TICKS_POC_DIFF_ONE, 0);

	if (inst->std == W_HEVC_ENC) {
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CUSTOM_MD_PU04, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CUSTOM_MD_PU08, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CUSTOM_MD_PU16, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CUSTOM_MD_PU32, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CUSTOM_MD_CU08, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CUSTOM_MD_CU16, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_CUSTOM_MD_CU32, 0);
		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_DEPENDENT_SLICE,
			      p_param->depend_slice_mode_arg << 16 | p_param->depend_slice_mode);

		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_NR_PARAM, 0);

		vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_NR_WEIGHT,
			      p_param->nr_intra_weight_y |
			      (p_param->nr_intra_weight_cb << 5) |
			      (p_param->nr_intra_weight_cr << 10) |
			      (p_param->nr_inter_weight_y << 15) |
			      (p_param->nr_inter_weight_cb << 20) |
			      (p_param->nr_inter_weight_cr << 25));
	}
	vpu_write_reg(inst->dev, W4_CMD_ENC_SEQ_VUI_HRD_PARAM, 0);

	/*
	 * Wave4 SET_PARAM completion/status is latched on command interrupt
	 * and consumed in enc_get_seq_info().
	 * Avoid early RET_SUCCESS checks here.
	 */
	return send_firmware_command(inst, W4_ENC_SET_PARAM, false, NULL, NULL);
}

int wave4_vpu_enc_get_seq_info(struct vpu_instance *inst, struct enc_initial_info *info)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;

	if (!vpu_read_reg(inst->dev, W4_RET_SUCCESS)) {
		info->seq_init_err_reason = vpu_read_reg(inst->dev, W4_RET_FAIL_REASON);
		return -EIO;
	}

	info->warn_info = 0;
	info->min_frame_buffer_count = vpu_read_reg(inst->dev, W4_RET_ENC_MIN_FB_NUM);
	info->min_src_frame_count = vpu_read_reg(inst->dev, W4_RET_ENC_MIN_SRC_BUF_NUM);
	info->vlc_buf_size = 0;
	info->param_buf_size = 0;
	p_enc_info->vlc_buf_size = 0;
	p_enc_info->param_buf_size = 0;
	p_enc_info->instance_queue_count = 0;
	p_enc_info->report_queue_count = 0;

	return 0;
}

static u32 calculate_luma_stride(u32 width, u32 bit_depth)
{
	return ALIGN(ALIGN(width, 16) * ((bit_depth > 8) ? 5 : 4), 32);
}

static u32 calculate_chroma_stride(u32 width, u32 bit_depth)
{
	return ALIGN(ALIGN(width / 2, 16) * ((bit_depth > 8) ? 5 : 4), 32);
}

int wave4_vpu_enc_register_framebuffer(struct device *dev, struct vpu_instance *inst,
				       struct frame_buffer *fb_arr, enum tiled_map_type map_type,
				       unsigned int count)
{
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);
	int ret = 0;
	u32 stride;
	u32 start_no, end_no;
	size_t remain, idx, j, i, cnt_8_chunk;
	u32 reg_val = 0, pic_size = 0, mv_col_size, fbc_y_tbl_size, fbc_c_tbl_size;
	u32 sub_sampled_size = 0;
	u32 luma_stride, chroma_stride;
	u32 buf_height = 0, buf_width = 0;
	u32 bit_depth;
	bool avc_encoding = (inst->std == W_AVC_ENC);
	struct vpu_buf vb_mv = {0};
	struct vpu_buf vb_fbc_y_tbl = {0};
	struct vpu_buf vb_fbc_c_tbl = {0};
	struct vpu_buf vb_sub_sam_buf = {0};
	struct enc_open_param *p_open_param;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;

	p_open_param = &p_enc_info->open_param;
	mv_col_size = 0;
	fbc_y_tbl_size = 0;
	fbc_c_tbl_size = 0;
	stride = p_enc_info->stride;
	bit_depth = p_open_param->wave_param.internal_bit_depth;

	if (avc_encoding) {
		buf_width = ALIGN(p_open_param->pic_width, 16);
		buf_height = ALIGN(p_open_param->pic_height, 16);

		if ((p_enc_info->rotation_angle || p_enc_info->mirror_direction) &&
		    !(p_enc_info->rotation_angle == 180 &&
					p_enc_info->mirror_direction == MIRDIR_HOR_VER)) {
			buf_width = ALIGN(p_open_param->pic_width, 16);
			buf_height = ALIGN(p_open_param->pic_height, 16);
		}

		if (p_enc_info->rotation_angle == 90 || p_enc_info->rotation_angle == 270) {
			buf_width = ALIGN(p_open_param->pic_height, 16);
			buf_height = ALIGN(p_open_param->pic_width, 16);
		}
	} else {
		buf_width = ALIGN(p_open_param->pic_width, 8);
		buf_height = ALIGN(p_open_param->pic_height, 8);

		if ((p_enc_info->rotation_angle || p_enc_info->mirror_direction) &&
		    !(p_enc_info->rotation_angle == 180 &&
					p_enc_info->mirror_direction == MIRDIR_HOR_VER)) {
			buf_width = ALIGN(p_open_param->pic_width, 32);
			buf_height = ALIGN(p_open_param->pic_height, 32);
		}

		if (p_enc_info->rotation_angle == 90 || p_enc_info->rotation_angle == 270) {
			buf_width = ALIGN(p_open_param->pic_height, 32);
			buf_height = ALIGN(p_open_param->pic_width, 32);
		}
	}

	pic_size = (buf_width << 16) | buf_height;

	if (avc_encoding) {
		mv_col_size = W4_ENC_AVC_BUF_SIZE(buf_width, buf_height);
		vb_mv.daddr = 0;
		vb_mv.size = ALIGN(mv_col_size * count, BUFFER_MARGIN) + BUFFER_MARGIN;
	} else {
		mv_col_size = W4_ENC_HEVC_BUF_SIZE(buf_width, buf_height);
		mv_col_size = ALIGN(mv_col_size, 16);
		vb_mv.daddr = 0;
		vb_mv.size = ALIGN(mv_col_size * count, BUFFER_MARGIN) + BUFFER_MARGIN;
	}

	ret = wave4_vdi_allocate_dma_memory(vpu_dev, &vb_mv);
	if (ret)
		return ret;

	p_enc_info->vb_mv = vb_mv;

	fbc_y_tbl_size = ALIGN(W4_FBC_LUMA_TABLE_SIZE(buf_width, buf_height), 16);
	fbc_c_tbl_size = ALIGN(W4_FBC_CHROMA_TABLE_SIZE(buf_width, buf_height), 16);

	vb_fbc_y_tbl.daddr = 0;
	vb_fbc_y_tbl.size = ALIGN(fbc_y_tbl_size * count, BUFFER_MARGIN) + BUFFER_MARGIN;
	ret = wave4_vdi_allocate_dma_memory(vpu_dev, &vb_fbc_y_tbl);
	if (ret)
		goto free_vb_fbc_y_tbl;

	p_enc_info->vb_fbc_y_tbl = vb_fbc_y_tbl;

	vb_fbc_c_tbl.daddr = 0;
	vb_fbc_c_tbl.size = ALIGN(fbc_c_tbl_size * count, BUFFER_MARGIN) + BUFFER_MARGIN;
	ret = wave4_vdi_allocate_dma_memory(vpu_dev, &vb_fbc_c_tbl);
	if (ret)
		goto free_vb_fbc_c_tbl;

	p_enc_info->vb_fbc_c_tbl = vb_fbc_c_tbl;

	if (avc_encoding)
		sub_sampled_size = W4_SUBSAMPLED_ONE_SIZE_AVC(buf_width, buf_height);
	else
		sub_sampled_size = W4_SUBSAMPLED_ONE_SIZE(buf_width, buf_height);
	vb_sub_sam_buf.size = ALIGN(sub_sampled_size * count, BUFFER_MARGIN) + BUFFER_MARGIN;
	vb_sub_sam_buf.daddr = 0;
	ret = wave4_vdi_allocate_dma_memory(vpu_dev, &vb_sub_sam_buf);
	if (ret)
		goto free_vb_sam_buf;

	p_enc_info->vb_sub_sam_buf = vb_sub_sam_buf;

	/* set sub-sampled buffer base addr */
	vpu_write_reg(inst->dev, W4_ADDR_SUB_SAMPLED_FB_BASE, vb_sub_sam_buf.daddr);
	/* set sub-sampled buffer size for one frame */
	vpu_write_reg(inst->dev, W4_SUB_SAMPLED_ONE_FB_SIZE, sub_sampled_size);

	vpu_write_reg(inst->dev, W4_PIC_SIZE, pic_size);

	/* set stride of luma/chroma for compressed buffer */
	if ((p_enc_info->rotation_angle || p_enc_info->mirror_direction) &&
	    !(p_enc_info->rotation_angle == 180 &&
	    p_enc_info->mirror_direction == MIRDIR_HOR_VER)) {
		luma_stride = calculate_luma_stride(buf_width, bit_depth);
		chroma_stride = calculate_chroma_stride(buf_width, bit_depth);
	} else {
		luma_stride = calculate_luma_stride(p_open_param->pic_width, bit_depth);
		chroma_stride = calculate_chroma_stride(p_open_param->pic_width, bit_depth);
	}

	vpu_write_reg(inst->dev, W4_FBC_STRIDE, luma_stride << 16 | chroma_stride);
	reg_val = (inst->nv21 << 29) |
		  ((map_type == LINEAR_FRAME_MAP) << 28) |
		  ((map_type >= COMPRESSED_FRAME_MAP ? 0 : inst->cbcr_interleave) << 16) |
		  stride;
	vpu_write_reg(inst->dev, W4_COMMON_PIC_INFO, reg_val);

	remain = count;
	cnt_8_chunk = DIV_ROUND_UP(count, 8);
	idx = 0;
	for (j = 0; j < cnt_8_chunk; j++) {
		reg_val = (j == cnt_8_chunk - 1) << 4 | ((j == 0) << 3);
		vpu_write_reg(inst->dev, W4_SFB_OPTION, reg_val);
		start_no = j * 8;
		end_no = start_no + ((remain >= 8) ? 8 : remain) - 1;

		vpu_write_reg(inst->dev, W4_SET_FB_NUM, (start_no << 8) | end_no);

		for (i = 0; i < 8 && i < remain; i++) {
			vpu_write_reg(inst->dev, W4_ADDR_LUMA_BASE0 + (i << 4), fb_arr[i +
					start_no].buf_y);
			vpu_write_reg(inst->dev, W4_ADDR_CB_BASE0 + (i << 4),
				      fb_arr[i + start_no].buf_cb);
			/* luma FBC offset table */
			vpu_write_reg(inst->dev, W4_ADDR_FBC_Y_OFFSET0 + (i << 4),
				      vb_fbc_y_tbl.daddr + idx * fbc_y_tbl_size);
			/* chroma FBC offset table */
			vpu_write_reg(inst->dev, W4_ADDR_FBC_C_OFFSET0 + (i << 4),
				      vb_fbc_c_tbl.daddr + idx * fbc_c_tbl_size);

			vpu_write_reg(inst->dev, W4_ADDR_MV_COL0 + (i << 2),
				      vb_mv.daddr + idx * mv_col_size);
			idx++;
		}
		remain -= i;

		vpu_write_reg(inst->dev, W4_ADDR_WORK_BASE, p_enc_info->vb_work.daddr);
		vpu_write_reg(inst->dev, W4_WORK_SIZE, p_enc_info->vb_work.size);
		vpu_write_reg(inst->dev, W4_WORK_PARAM, 0);

		ret = send_firmware_command(inst, W4_SET_FB, false, NULL, NULL);
		if (ret)
			goto free_vb_mem;
	}

	ret = wave4_vpu_firmware_command_queue_error_check(vpu_dev, NULL);
	if (ret)
		goto free_vb_mem;
	/* W4_SFB_OPTION shares W4_COMMAND_OPTION; clear it after SET_FB phase. */
	vpu_write_reg(inst->dev, W4_COMMAND_OPTION, 0);

	return ret;

free_vb_mem:
	wave4_vdi_free_dma_memory(vpu_dev, &vb_sub_sam_buf);
free_vb_sam_buf:
	wave4_vdi_free_dma_memory(vpu_dev, &vb_fbc_c_tbl);
free_vb_fbc_c_tbl:
	wave4_vdi_free_dma_memory(vpu_dev, &vb_fbc_y_tbl);
free_vb_fbc_y_tbl:
	wave4_vdi_free_dma_memory(vpu_dev, &vb_mv);
	return ret;
}

static u32 wave4_vpu_enc_validate_sec_axi(struct vpu_instance *inst, u32 *sec_axi_size)
{
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	struct enc_open_param *p_open_param = &p_enc_info->open_param;
	u32 imd_size = 0, rdo_size = 0, lf_size = 0;
	u32 ret = 0, used = 0;
	u32 sram_size = inst->dev->sram_buf.size;
	u32 width = p_open_param->pic_width ?: inst->src_fmt.width;
	u32 lf_luma = 5, lf_chroma = 3;

	if (sec_axi_size)
		*sec_axi_size = 0;

	if (!sram_size || !width)
		return 0;

	imd_size = ALIGN(width, 32);
	if (p_open_param->wave_param.profile == HEVC_PROFILE_MAIN10) {
		lf_luma = 7;
		lf_chroma = 5;
	}
	lf_size = ALIGN(width, 64) * (lf_luma + lf_chroma);
	rdo_size = (ALIGN(width, 64) >> 5) * 22 * 16;

	if (p_enc_info->sec_axi_info.use_ip_enable && sram_size >= imd_size) {
		ret |= BIT(9);
		sram_size -= imd_size;
		used += imd_size;
	}

	if (p_enc_info->sec_axi_info.use_enc_lf_enable && sram_size >= lf_size) {
		ret |= BIT(15);
		sram_size -= lf_size;
		used += lf_size;
	}

	if (p_enc_info->sec_axi_info.use_enc_rdo_enable && sram_size >= rdo_size) {
		ret |= BIT(11);
		sram_size -= rdo_size;
		used += rdo_size;
	}

	if (sec_axi_size)
		*sec_axi_size = used;

	return ret;
}

int wave4_vpu_encode(struct vpu_instance *inst, struct enc_param *option, u32 *fail_res,
		     enum wave4_enc_frame_state *state)
{
	u32 src_frame_format;
	u32 reg_val = 0;
	u32 src_stride_c = 0;
	u32 sec_axi = 0;
	u32 sec_axi_size = 0;
	dma_addr_t temp_base;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;
	struct frame_buffer *p_src_frame = option->source_frame;
	struct enc_open_param *p_open_param = &p_enc_info->open_param;
	bool justified = WTL_RIGHT_JUSTIFIED;
	u32 format_no = WTL_PIXEL_8BIT;
	int ret;

	*state = W4_ENC_FRAME_NOT_SUBMITTED;
	temp_base = inst->dev->common_mem.daddr + W4_MAX_CODE_BUF_SIZE;

	vpu_write_reg(inst->dev, W4_BS_START_ADDR, option->pic_stream_buffer_addr);
	vpu_write_reg(inst->dev, W4_BS_SIZE, option->pic_stream_buffer_size);
	p_enc_info->stream_buf_start_addr = option->pic_stream_buffer_addr;
	p_enc_info->stream_buf_size = option->pic_stream_buffer_size;
	p_enc_info->stream_buf_end_addr =
		option->pic_stream_buffer_addr + option->pic_stream_buffer_size;
	p_enc_info->stream_rd_ptr = option->pic_stream_buffer_addr;
	p_enc_info->stream_wr_ptr = option->pic_stream_buffer_addr;
	vpu_write_reg(inst->dev, W4_BS_RD_PTR, p_enc_info->stream_rd_ptr);
	vpu_write_reg(inst->dev, W4_BS_WR_PTR, p_enc_info->stream_wr_ptr);
	vpu_write_reg(inst->dev, W4_BS_PARAM,
		      (p_enc_info->line_buf_int_en << 6) |
		      wave4_resolve_bs_endian_nibble());

	/* Secondary AXI + scratch regions are reprogrammed for ENC_PIC on Wave4. */
	sec_axi = wave4_vpu_enc_validate_sec_axi(inst, &sec_axi_size);
	sec_axi = wave4_apply_enc_sec_axi_mask(sec_axi);
	vpu_write_reg(inst->dev, W4_ADDR_SEC_AXI, wave4_cmd_addr(inst->dev->sram_buf.daddr));
	vpu_write_reg(inst->dev, W4_SEC_AXI_SIZE, sec_axi ? sec_axi_size : 0);
	vpu_write_reg(inst->dev, W4_USE_SEC_AXI, sec_axi);
	vpu_write_reg(inst->dev, W4_ADDR_WORK_BASE, wave4_cmd_addr(p_enc_info->vb_work.daddr));
	vpu_write_reg(inst->dev, W4_WORK_SIZE, p_enc_info->vb_work.size);
	vpu_write_reg(inst->dev, W4_WORK_PARAM, 0);
	vpu_write_reg(inst->dev, W4_ADDR_TEMP_BASE, wave4_cmd_addr(temp_base));
	vpu_write_reg(inst->dev, W4_TEMP_SIZE, W4_TEMPBUF_SIZE);
	vpu_write_reg(inst->dev, W4_TEMP_PARAM, 0);

	vpu_write_reg(inst->dev, W4_CMD_ENC_ADDR_REPORT_BASE, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_REPORT_SIZE, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_REPORT_PARAM, 0);

	/*
	 * CODEOPT_ENC_VCL is used to implicitly encode header/headers to generate bitstream.
	 * (use ENC_PUT_VIDEO_HEADER for give_command to encode only a header)
	 */
	if (option->code_option.implicit_header_encode)
		vpu_write_reg(inst->dev, W4_CMD_ENC_CODE_OPTION,
			      CODEOPT_ENC_HEADER_IMPLICIT | CODEOPT_ENC_VCL |
			      (option->code_option.encode_aud << 5) |
			      (option->code_option.encode_eos << 6) |
			      (option->code_option.encode_eob << 7));
	else
		vpu_write_reg(inst->dev, W4_CMD_ENC_CODE_OPTION,
			      option->code_option.implicit_header_encode |
			      (option->code_option.encode_vcl << 1) |
			      (option->code_option.encode_vps << 2) |
			      (option->code_option.encode_sps << 3) |
			      (option->code_option.encode_pps << 4) |
			      (option->code_option.encode_aud << 5) |
			      (option->code_option.encode_eos << 6) |
			      (option->code_option.encode_eob << 7) |
			      (option->code_option.encode_vui << 9));

	vpu_write_reg(inst->dev, W4_CMD_ENC_PIC_PARAM,
		      (option->force_pic_type_enable << 20) |
		      (option->force_pic_type << 21));

	if (option->src_end_flag)
		/* no more source images. */
		vpu_write_reg(inst->dev, W4_CMD_ENC_SRC_PIC_IDX, 0xFFFFFFFF);
	else
		vpu_write_reg(inst->dev, W4_CMD_ENC_SRC_PIC_IDX, option->src_idx);

	vpu_write_reg(inst->dev, W4_CMD_ENC_SRC_ADDR_Y, p_src_frame->buf_y);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SRC_ADDR_U, p_src_frame->buf_cb);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SRC_ADDR_V, p_src_frame->buf_cr);

	switch (p_open_param->src_format) {
	case FORMAT_420:
	case FORMAT_422:
	case FORMAT_YUYV:
	case FORMAT_YVYU:
	case FORMAT_UYVY:
	case FORMAT_VYUY:
		justified = WTL_LEFT_JUSTIFIED;
		format_no = WTL_PIXEL_8BIT;
		src_stride_c = inst->cbcr_interleave ? p_src_frame->stride :
			(p_src_frame->stride / 2);
		src_stride_c = (p_open_param->src_format == FORMAT_422) ? src_stride_c * 2 :
			src_stride_c;
		break;
	case FORMAT_420_P10_16BIT_MSB:
	case FORMAT_422_P10_16BIT_MSB:
	case FORMAT_YUYV_P10_16BIT_MSB:
	case FORMAT_YVYU_P10_16BIT_MSB:
	case FORMAT_UYVY_P10_16BIT_MSB:
	case FORMAT_VYUY_P10_16BIT_MSB:
		justified = WTL_RIGHT_JUSTIFIED;
		format_no = WTL_PIXEL_16BIT;
		src_stride_c = inst->cbcr_interleave ? p_src_frame->stride :
			(p_src_frame->stride / 2);
		src_stride_c = (p_open_param->src_format ==
				FORMAT_422_P10_16BIT_MSB) ? src_stride_c * 2 : src_stride_c;
		break;
	case FORMAT_420_P10_16BIT_LSB:
	case FORMAT_422_P10_16BIT_LSB:
	case FORMAT_YUYV_P10_16BIT_LSB:
	case FORMAT_YVYU_P10_16BIT_LSB:
	case FORMAT_UYVY_P10_16BIT_LSB:
	case FORMAT_VYUY_P10_16BIT_LSB:
		justified = WTL_LEFT_JUSTIFIED;
		format_no = WTL_PIXEL_16BIT;
		src_stride_c = inst->cbcr_interleave ? p_src_frame->stride :
			(p_src_frame->stride / 2);
		src_stride_c = (p_open_param->src_format ==
				FORMAT_422_P10_16BIT_LSB) ? src_stride_c * 2 : src_stride_c;
		break;
	case FORMAT_420_P10_32BIT_MSB:
	case FORMAT_422_P10_32BIT_MSB:
	case FORMAT_YUYV_P10_32BIT_MSB:
	case FORMAT_YVYU_P10_32BIT_MSB:
	case FORMAT_UYVY_P10_32BIT_MSB:
	case FORMAT_VYUY_P10_32BIT_MSB:
		justified = WTL_RIGHT_JUSTIFIED;
		format_no = WTL_PIXEL_32BIT;
		src_stride_c = inst->cbcr_interleave ? p_src_frame->stride :
			ALIGN(p_src_frame->stride / 2, 16) * BIT(inst->cbcr_interleave);
		src_stride_c = (p_open_param->src_format ==
				FORMAT_422_P10_32BIT_MSB) ? src_stride_c * 2 : src_stride_c;
		break;
	case FORMAT_420_P10_32BIT_LSB:
	case FORMAT_422_P10_32BIT_LSB:
	case FORMAT_YUYV_P10_32BIT_LSB:
	case FORMAT_YVYU_P10_32BIT_LSB:
	case FORMAT_UYVY_P10_32BIT_LSB:
	case FORMAT_VYUY_P10_32BIT_LSB:
		justified = WTL_LEFT_JUSTIFIED;
		format_no = WTL_PIXEL_32BIT;
		src_stride_c = inst->cbcr_interleave ? p_src_frame->stride :
			ALIGN(p_src_frame->stride / 2, 16) * BIT(inst->cbcr_interleave);
		src_stride_c = (p_open_param->src_format ==
				FORMAT_422_P10_32BIT_LSB) ? src_stride_c * 2 : src_stride_c;
		break;
	default:
		return -EINVAL;
	}

	src_frame_format = (inst->cbcr_interleave << 1) | (inst->nv21);
	switch (p_open_param->packed_format) {
	case PACKED_YUYV:
		src_frame_format = 4;
		break;
	case PACKED_YVYU:
		src_frame_format = 5;
		break;
	case PACKED_UYVY:
		src_frame_format = 6;
		break;
	case PACKED_VYUY:
		src_frame_format = 7;
		break;
	default:
		break;
	}

	vpu_write_reg(inst->dev, W4_CMD_ENC_SRC_STRIDE,
		      (p_src_frame->stride << 16) | src_stride_c);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SRC_FORMAT, src_frame_format |
		      (format_no << 3) | (justified << 5) | (PIC_SRC_ENDIANNESS_BIG_ENDIAN << 6));

	vpu_write_reg(inst->dev, W4_CMD_ENC_PREFIX_SEI_INFO, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_PREFIX_SEI_NAL_ADDR, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SUFFIX_SEI_INFO, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SUFFIX_SEI_NAL_ADDR, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_ROI_ADDR_CTU_MAP, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_CTU_MODE_MAP_ADDR, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_CTU_QP_MAP_ADDR, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_CTU_OPT_PARAM, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SRC_TIMESTAMP_LOW, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SRC_TIMESTAMP_HIGH, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_LONGTERM_PIC, 0);
	vpu_write_reg(inst->dev, W4_CMD_ENC_SUB_FRAME_SYNC_CONFIG, 0);

	/* A failed BUSY wait does not establish that hardware released buffers. */
	*state = W4_ENC_FRAME_UNKNOWN;
	ret = send_firmware_command(inst, W4_DEC_ENC_PIC, false, &reg_val, fail_res);
	if (!ret)
		*state = W4_ENC_FRAME_COMPLETE;
	if (ret == -ETIMEDOUT)
		return ret;

	/* Wave420L does not provide stable queue counters on ENC_PIC. */
	p_enc_info->instance_queue_count = 0;
	p_enc_info->report_queue_count = 0;

	if (ret)
		return ret;

	return 0;
}

int wave4_vpu_enc_get_result(struct vpu_instance *inst, struct enc_output_info *result)
{
	u32 reg_val;
	int retry;
	struct enc_info *p_enc_info = &inst->codec_info->enc_info;

	dev_dbg(inst->dev->dev, "%s: enc pic complete\n", __func__);

	/* Wave420L returns ENC_PIC result latches directly on PIC_RUN completion. */
	p_enc_info->instance_queue_count = 0;
	p_enc_info->report_queue_count = 0;

	result->warn_info = 0;
	for (retry = 0; retry < 100; retry++) {
		if (vpu_read_reg(inst->dev, W4_RET_SUCCESS))
			break;
		usleep_range(100, 200);
	}
	if (!vpu_read_reg(inst->dev, W4_RET_SUCCESS)) {
		result->error_reason = vpu_read_reg(inst->dev, W4_RET_FAIL_REASON);
		return -EIO;
	}
	result->error_reason = 0;

	reg_val = vpu_read_reg(inst->dev, W4_RET_ENC_PIC_TYPE);
	result->pic_type = reg_val & 0xFFFF;

	result->enc_vcl_nut = vpu_read_reg(inst->dev, W4_RET_ENC_VCL_NUT);
	/*
	 * To get the reconstructed frame use the following index on
	 * inst->frame_buf
	 */
	result->recon_frame_index = (s32)vpu_read_reg(inst->dev, W4_RET_ENC_PIC_IDX);
	result->enc_pic_byte = vpu_read_reg(inst->dev, W4_RET_ENC_PIC_BYTE);
	result->enc_src_idx = (s32)vpu_read_reg(inst->dev, W4_RET_ENC_USED_SRC_IDX);
	p_enc_info->stream_wr_ptr = vpu_read_reg(inst->dev, W4_BS_WR_PTR);
	p_enc_info->stream_rd_ptr = vpu_read_reg(inst->dev, W4_BS_RD_PTR);

	result->bitstream_buffer = p_enc_info->stream_rd_ptr;
	result->rd_ptr = p_enc_info->stream_rd_ptr;
	result->wr_ptr = p_enc_info->stream_wr_ptr;

	/*result for header only(no vcl) encoding */
	if (result->recon_frame_index == RECON_IDX_FLAG_HEADER_ONLY)
		result->bitstream_size = result->enc_pic_byte;
	else if (result->recon_frame_index < 0)
		result->bitstream_size = 0;
	else
		result->bitstream_size = result->enc_pic_byte;

	result->enc_host_cmd_tick = 0;
	result->enc_encode_end_tick = 0;
	result->frame_cycle = vpu_read_reg(inst->dev, W4_RET_FRAME_CYCLE);
	p_enc_info->first_cycle_check = true;

	return 0;
}

int wave4_vpu_enc_finish_seq(struct vpu_instance *inst, u32 *fail_res)
{
	return send_firmware_command(inst, W4_DESTROY_INSTANCE, true, NULL, fail_res);
}

static bool wave4_vpu_enc_check_common_param_valid(struct vpu_instance *inst,
						   struct enc_open_param *open_param)
{
	bool low_delay = true;
	struct enc_wave_param *param = &open_param->wave_param;
	struct vpu_device *vpu_dev = inst->dev;
	struct device *dev = vpu_dev->dev;
	u32 num_ctu_row = (open_param->pic_height + 64 - 1) / 64;
	u32 num_ctu_col = (open_param->pic_width + 64 - 1) / 64;
	u32 ctu_sz = num_ctu_col * num_ctu_row;

	if (inst->std == W_HEVC_ENC && low_delay &&
	    param->decoding_refresh_type == DEC_REFRESH_TYPE_CRA) {
		dev_warn(dev,
			 "dec_refresh_type(CRA) shouldn't be used together with low delay GOP\n");
		dev_warn(dev, "Suggested configuration parameter: decoding refresh type (IDR)\n");
		param->decoding_refresh_type = 2;
	}

	if (param->wpp_enable && param->independ_slice_mode) {
		unsigned int num_ctb_in_width = ALIGN(open_param->pic_width, 64) >> 6;

		if (param->independ_slice_mode_arg % num_ctb_in_width) {
			dev_err(dev, "independ_slice_mode_arg %u must be a multiple of %u\n",
				param->independ_slice_mode_arg, num_ctb_in_width);
			return false;
		}
	}

	/* multi-slice & wpp */
	if (param->wpp_enable && param->depend_slice_mode) {
		dev_err(dev, "wpp_enable && depend_slice_mode cannot be used simultaneously\n");
		return false;
	}

	if (!param->independ_slice_mode && param->depend_slice_mode) {
		dev_err(dev, "depend_slice_mode requires independ_slice_mode\n");
		return false;
	} else if (param->independ_slice_mode &&
		   param->depend_slice_mode == DEPEND_SLICE_MODE_RECOMMENDED &&
		   param->independ_slice_mode_arg < param->depend_slice_mode_arg) {
		dev_err(dev, "independ_slice_mode_arg: %u must be smaller than %u\n",
			param->independ_slice_mode_arg, param->depend_slice_mode_arg);
		return false;
	}

	if (param->independ_slice_mode && param->independ_slice_mode_arg > 65535) {
		dev_err(dev, "independ_slice_mode_arg: %u must be smaller than 65535\n",
			param->independ_slice_mode_arg);
		return false;
	}

	if (param->depend_slice_mode && param->depend_slice_mode_arg > 65535) {
		dev_err(dev, "depend_slice_mode_arg: %u must be smaller than 65535\n",
			param->depend_slice_mode_arg);
		return false;
	}

	if (param->conf_win_top % 2) {
		dev_err(dev, "conf_win_top: %u, must be a multiple of 2\n", param->conf_win_top);
		return false;
	}

	if (param->conf_win_bot % 2) {
		dev_err(dev, "conf_win_bot: %u, must be a multiple of 2\n", param->conf_win_bot);
		return false;
	}

	if (param->conf_win_left % 2) {
		dev_err(dev, "conf_win_left: %u, must be a multiple of 2\n", param->conf_win_left);
		return false;
	}

	if (param->conf_win_right % 2) {
		dev_err(dev, "conf_win_right: %u, Must be a multiple of 2\n",
			param->conf_win_right);
		return false;
	}

	if (param->lossless_enable && open_param->rc_enable) {
		dev_err(dev, "option rate_control cannot be used with lossless_coding\n");
		return false;
	}

	if (param->lossless_enable && !param->skip_intra_trans) {
		dev_err(dev, "option intra_trans_skip must be enabled with lossless_coding\n");
		return false;
	}

	/* intra refresh */
	if (param->intra_refresh_mode && param->intra_refresh_arg == 0) {
		dev_err(dev, "Invalid refresh argument, mode: %u, refresh: %u must be > 0\n",
			param->intra_refresh_mode, param->intra_refresh_arg);
		return false;
	}
	switch (param->intra_refresh_mode) {
	case REFRESH_MODE_CTU_ROWS:
		if (param->intra_mb_refresh_arg > num_ctu_row)
			goto invalid_refresh_argument;
		break;
	case REFRESH_MODE_CTU_COLUMNS:
		if (param->intra_refresh_arg > num_ctu_col)
			goto invalid_refresh_argument;
		break;
	case REFRESH_MODE_CTU_STEP_SIZE:
		if (param->intra_refresh_arg > ctu_sz)
			goto invalid_refresh_argument;
		break;
	case REFRESH_MODE_CTUS:
		if (param->intra_refresh_arg > ctu_sz)
			goto invalid_refresh_argument;
		if (param->lossless_enable) {
			dev_err(dev, "mode: %u cannot be used lossless_enable",
				param->intra_refresh_mode);
			return false;
		}
	}
	return true;

invalid_refresh_argument:
	dev_err(dev, "Invalid refresh argument, mode: %u, refresh: %u > W(%u)xH(%u)\n",
		param->intra_refresh_mode, param->intra_refresh_arg,
		num_ctu_row, num_ctu_col);
	return false;
}

static bool wave4_vpu_enc_check_param_valid(struct vpu_device *vpu_dev,
					    struct enc_open_param *open_param)
{
	struct enc_wave_param *param = &open_param->wave_param;

	if (open_param->rc_enable) {
		if (param->min_qp_i > param->max_qp_i || param->min_qp_p > param->max_qp_p ||
		    param->min_qp_b > param->max_qp_b) {
			dev_err(vpu_dev->dev, "Configuration failed because min_qp is greater than max_qp\n");
			dev_err(vpu_dev->dev, "Suggested configuration parameters: min_qp = max_qp\n");
			return false;
		}

		if (open_param->bit_rate <= (int)open_param->frame_rate_info) {
			dev_err(vpu_dev->dev,
				"enc_bit_rate: %u must be greater than the frame_rate: %u\n",
				open_param->bit_rate, (int)open_param->frame_rate_info);
			return false;
		}
	}

	return true;
}

struct wave4_hevc_level {
	u32 level;
	u32 max_luma_ps;
	u32 max_luma_sr;
	u32 max_br_main;
	u32 max_br_high;
};

/* HEVC Annex A limits; bitrate columns are in kbit/s. */
static const struct wave4_hevc_level wave4_hevc_levels[] = {
	{ 10,   36864,    552960,   128,      0 },
	{ 20,  122880,   3686400,  1500,      0 },
	{ 21,  245760,   7372800,  3000,      0 },
	{ 30,  552960,  16588800,  6000,      0 },
	{ 31,  983040,  33177600, 10000,      0 },
	{ 40, 2228224,  66846720, 12000,  30000 },
	{ 41, 2228224, 133693440, 20000,  50000 },
	{ 50, 8912896, 267386880, 25000, 100000 },
};

static bool wave4_vpu_enc_check_level(struct vpu_instance *inst,
				    const struct enc_open_param *open_param)
{
	const struct enc_wave_param *param = &open_param->wave_param;
	const struct wave4_hevc_level *level = NULL;
	u32 width = ALIGN(open_param->pic_width, 8);
	u32 height = ALIGN(open_param->pic_height, 8);
	u32 rate_num = open_param->frame_rate_info & 0xffff;
	u32 rate_den = (open_param->frame_rate_info >> 16) + 1;
	u64 luma_ps = (u64)width * height;
	u32 max_br;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(wave4_hevc_levels); i++) {
		if (wave4_hevc_levels[i].level == param->level) {
			level = &wave4_hevc_levels[i];
			break;
		}
	}
	if (!level)
		goto invalid;

	max_br = param->tier ? level->max_br_high : level->max_br_main;
	if (!max_br || luma_ps > level->max_luma_ps ||
	    (u64)width * width > 8ULL * level->max_luma_ps ||
	    (u64)height * height > 8ULL * level->max_luma_ps ||
	    luma_ps * rate_num > (u64)level->max_luma_sr * rate_den)
		goto invalid;

	/* Main and Main 10 use CpbBrNalFactor = 1100 (HEVC Annex A). */
	if (open_param->rc_enable && open_param->bit_rate > (u64)max_br * 1100)
		goto invalid;

	return true;

invalid:
	dev_err(inst->dev->dev, "Encoding configuration exceeds HEVC level %u, tier %u\n",
		param->level, param->tier);
	return false;
}

int wave4_vpu_enc_check_open_param(struct vpu_instance *inst, struct enc_open_param *open_param)
{
	u32 pic_width;
	u32 pic_height;
	struct vpu_attr *p_attr = &inst->dev->attr;
	struct enc_wave_param *param;

	if (!open_param)
		return -EINVAL;

	param = &open_param->wave_param;
	pic_width = open_param->pic_width;
	pic_height = open_param->pic_height;

	if (inst->id >= MAX_NUM_INSTANCE) {
		dev_err(inst->dev->dev, "Too many simultaneous instances: %d (max: %u)\n",
			inst->id, MAX_NUM_INSTANCE);
		return -EOPNOTSUPP;
	}

	if (inst->std != W_HEVC_ENC) {
		dev_err(inst->dev->dev, "Wave4 encoder supports HEVC only\n");
		return -EOPNOTSUPP;
	}

	if (param->internal_bit_depth == 10) {
		if (inst->std == W_HEVC_ENC && !p_attr->support_hevc10bit_enc) {
			dev_err(inst->dev->dev,
				"Flag support_hevc10bit_enc must be set to encode 10bit HEVC\n");
			return -EOPNOTSUPP;
		} else if (inst->std == W_AVC_ENC && !p_attr->support_avc10bit_enc) {
			dev_err(inst->dev->dev,
				"Flag support_avc10bit_enc must be set to encode 10bit AVC\n");
			return -EOPNOTSUPP;
		}
	}

	if (!open_param->frame_rate_info) {
		dev_err(inst->dev->dev, "No frame rate information.\n");
		return -EINVAL;
	}

	if (open_param->bit_rate > MAX_BIT_RATE) {
		dev_err(inst->dev->dev, "Invalid encoding bit-rate: %u (valid: 0-%u)\n",
			open_param->bit_rate, MAX_BIT_RATE);
		return -EINVAL;
	}

	if (pic_width < W4_MIN_ENC_PIC_WIDTH || pic_width > W4_MAX_ENC_PIC_WIDTH ||
	    pic_height < W4_MIN_ENC_PIC_HEIGHT || pic_height > W4_MAX_ENC_PIC_HEIGHT) {
		dev_err(inst->dev->dev, "Invalid encoding dimension: %ux%u\n",
			pic_width, pic_height);
		return -EINVAL;
	}

	if (!wave4_vpu_enc_check_level(inst, open_param))
		return -EINVAL;

	if (param->profile) {
		if (inst->std == W_HEVC_ENC) {
			if ((param->profile != HEVC_PROFILE_MAIN ||
			     (param->profile == HEVC_PROFILE_MAIN &&
			      param->internal_bit_depth > 8)) &&
			    (param->profile != HEVC_PROFILE_MAIN10 ||
			     (param->profile == HEVC_PROFILE_MAIN10 &&
			      param->internal_bit_depth < 10)) &&
			    param->profile != HEVC_PROFILE_STILLPICTURE) {
				dev_err(inst->dev->dev,
					"Invalid HEVC encoding profile: %u (bit-depth: %u)\n",
					param->profile, param->internal_bit_depth);
				return -EINVAL;
			}
		} else if (inst->std == W_AVC_ENC) {
			if ((param->internal_bit_depth > 8 &&
			     param->profile != H264_PROFILE_HIGH10)) {
				dev_err(inst->dev->dev,
					"Invalid AVC encoding profile: %u (bit-depth: %u)\n",
					param->profile, param->internal_bit_depth);
				return -EINVAL;
			}
		}
	}

	if (param->decoding_refresh_type > DEC_REFRESH_TYPE_IDR) {
		dev_err(inst->dev->dev, "Invalid decoding refresh type: %u (valid: 0-2)\n",
			param->decoding_refresh_type);
		return -EINVAL;
	}

	if (param->intra_refresh_mode > REFRESH_MODE_CTUS) {
		dev_err(inst->dev->dev, "Invalid intra refresh mode: %d (valid: 0-4)\n",
			param->intra_refresh_mode);
		return -EINVAL;
	}

	if (inst->std == W_HEVC_ENC && param->independ_slice_mode &&
	    param->depend_slice_mode > DEPEND_SLICE_MODE_BOOST) {
		dev_err(inst->dev->dev,
			"Can't combine slice modes: independent and fast dependent for HEVC\n");
		return -EINVAL;
	}

	if (!param->disable_deblk) {
		if (param->beta_offset_div2 < -6 || param->beta_offset_div2 > 6) {
			dev_err(inst->dev->dev, "Invalid beta offset: %d (valid: -6-6)\n",
				param->beta_offset_div2);
			return -EINVAL;
		}

		if (param->tc_offset_div2 < -6 || param->tc_offset_div2 > 6) {
			dev_err(inst->dev->dev, "Invalid tc offset: %d (valid: -6-6)\n",
				param->tc_offset_div2);
			return -EINVAL;
		}
	}

	if (param->intra_qp > MAX_INTRA_QP) {
		dev_err(inst->dev->dev,
			"Invalid intra quantization parameter: %u (valid: 0-%u)\n",
			param->intra_qp, MAX_INTRA_QP);
		return -EINVAL;
	}

	if (open_param->rc_enable) {
		if (param->min_qp_i > MAX_INTRA_QP || param->max_qp_i > MAX_INTRA_QP ||
		    param->min_qp_p > MAX_INTRA_QP || param->max_qp_p > MAX_INTRA_QP ||
		    param->min_qp_b > MAX_INTRA_QP || param->max_qp_b > MAX_INTRA_QP) {
			dev_err(inst->dev->dev,
				"Invalid quantization parameter min/max values: "
				"I: %u-%u, P: %u-%u, B: %u-%u (valid for each: 0-%u)\n",
				param->min_qp_i, param->max_qp_i, param->min_qp_p, param->max_qp_p,
				param->min_qp_b, param->max_qp_b, MAX_INTRA_QP);
			return -EINVAL;
		}

		if (param->hvs_qp_enable && param->hvs_max_delta_qp > MAX_HVS_MAX_DELTA_QP) {
			dev_err(inst->dev->dev,
				"Invalid HVS max delta quantization parameter: %u (valid: 0-%u)\n",
				param->hvs_max_delta_qp, MAX_HVS_MAX_DELTA_QP);
			return -EINVAL;
		}

		if (open_param->vbv_buffer_size < MIN_VBV_BUFFER_SIZE ||
		    open_param->vbv_buffer_size > MAX_VBV_BUFFER_SIZE) {
			dev_err(inst->dev->dev, "VBV buffer size: %u (valid: %u-%u)\n",
				open_param->vbv_buffer_size, MIN_VBV_BUFFER_SIZE,
				MAX_VBV_BUFFER_SIZE);
			return -EINVAL;
		}
	}

	if (!wave4_vpu_enc_check_common_param_valid(inst, open_param))
		return -EINVAL;

	if (!wave4_vpu_enc_check_param_valid(inst->dev, open_param))
		return -EINVAL;

	if (param->chroma_cb_qp_offset < -12 || param->chroma_cb_qp_offset > 12) {
		dev_err(inst->dev->dev,
			"Invalid chroma Cb quantization parameter offset: %d (valid: -12-12)\n",
			param->chroma_cb_qp_offset);
		return -EINVAL;
	}

	if (param->chroma_cr_qp_offset < -12 || param->chroma_cr_qp_offset > 12) {
		dev_err(inst->dev->dev,
			"Invalid chroma Cr quantization parameter offset: %d (valid: -12-12)\n",
			param->chroma_cr_qp_offset);
		return -EINVAL;
	}

	if (param->intra_refresh_mode == REFRESH_MODE_CTU_STEP_SIZE && !param->intra_refresh_arg) {
		dev_err(inst->dev->dev,
			"Intra refresh mode CTU step-size requires an argument\n");
		return -EINVAL;
	}

	if (inst->std == W_HEVC_ENC) {
		if (param->nr_intra_weight_y > MAX_INTRA_WEIGHT ||
		    param->nr_intra_weight_cb > MAX_INTRA_WEIGHT ||
		    param->nr_intra_weight_cr > MAX_INTRA_WEIGHT) {
			dev_err(inst->dev->dev,
				"Invalid intra weight Y(%u) Cb(%u) Cr(%u) (valid: %u)\n",
				param->nr_intra_weight_y, param->nr_intra_weight_cb,
				param->nr_intra_weight_cr, MAX_INTRA_WEIGHT);
			return -EINVAL;
		}

		if (param->nr_inter_weight_y > MAX_INTER_WEIGHT ||
		    param->nr_inter_weight_cb > MAX_INTER_WEIGHT ||
		    param->nr_inter_weight_cr > MAX_INTER_WEIGHT) {
			dev_err(inst->dev->dev,
				"Invalid inter weight Y(%u) Cb(%u) Cr(%u) (valid: %u)\n",
				param->nr_inter_weight_y, param->nr_inter_weight_cb,
				param->nr_inter_weight_cr, MAX_INTER_WEIGHT);
			return -EINVAL;
		}
	}

	return 0;
}
