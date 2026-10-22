// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave4 series multi-standard codec IP - platform driver
 *
 * Copyright (C) 2021-2023 CHIPS&MEDIA INC
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include "wave4-vpu.h"
#include "wave4-regdefine.h"
#include "wave4-vpuconfig.h"
#include "wave4.h"

#define VPU_PLATFORM_DEVICE_NAME "vdec-wave4"
#define VPU_CLK_NAME "vcodec"

#define W4_IS_ENC BIT(0)
#define W4_IS_DEC BIT(1)

#define SG2002_VC_SRAM_SHARE_OFFSET	0x24
#define SG2002_VC_SRAM_SHARE_MASK	GENMASK(1, 0)
#define SG2002_VC_SRAM_SHARE_H265	0x2
#define SG2002_VC_CTRL_BASE		0x0B030000
#define SG2002_VC_CTRL_SIZE		0x100
#define W4_POLL_INTERVAL_MS		5
/*
 * Wave420L-specific capability map observed on SG2002:
 * - std_def1 bit16: HEVC encoder path present
 * - std_def1 bit17: HEVC decoder path present
 */
#define W4_STD_DEF1_HEVC_ENC		BIT(16)
#define W4_STD_DEF1_HEVC_DEC		BIT(17)

struct wave4_match_data {
	int flags;
	const char *fw_name;
	u32 sram_size;
	bool allow_internal_sram;
	bool use_std_def1_caps;
	u32 std_def1_enc_mask;
	u32 std_def1_dec_mask;
};

static bool w4_forbid_runtime_pm;
module_param(w4_forbid_runtime_pm, bool, 0644);
MODULE_PARM_DESC(w4_forbid_runtime_pm,
		 "Keep Wave4 runtime PM active (no autosuspend sleep/wake transitions)");

int wave4_vpu_wait_interrupt(struct vpu_instance *inst, unsigned int timeout)
{
	unsigned int wait_ms = timeout;
	unsigned int waited_ms = 0;
	int ret;
	u32 reason;
	u32 reason_usr;
	u32 int_sts;

	/*
	 * Keep the wait bounded so userspace teardown does not get pinned in
	 * long uninterruptible sleeps when firmware command completion stalls.
	 */
	if (wait_ms > 3000)
		wait_ms = 3000;

	while (waited_ms < wait_ms) {
		unsigned int slice_ms = min_t(unsigned int, 5, wait_ms - waited_ms);

		ret = wait_for_completion_timeout(&inst->irq_done,
						  msecs_to_jiffies(slice_ms));
		if (ret) {
			reinit_completion(&inst->irq_done);
			return 0;
		}

		waited_ms += slice_ms;
		int_sts = wave4_vdi_read_register(inst->dev, W4_VPU_VPU_INT_STS);
		reason = wave4_vdi_read_register(inst->dev, W4_VPU_VINT_REASON);
		reason_usr = wave4_vdi_read_register(inst->dev, W4_VPU_VINT_REASON_USR);
		if (reason_usr || (int_sts && reason)) {
			u32 clear_reason = reason | reason_usr;

			wave4_vdi_write_register(inst->dev, W4_VPU_VINT_REASON_CLR,
						 clear_reason);
			wave4_vdi_write_register(inst->dev, W4_VPU_VINT_CLEAR, 0x1);
			return 0;
		}
	}

	return -ETIMEDOUT;
}

static void wave4_vpu_handle_irq(void *dev_id)
{
	u32 irq_reason;
	u32 irq_reason_usr;
	u32 irq_reason_hw;
	struct vpu_instance *inst, *tmp;
	struct vpu_device *dev = dev_id;
	int val;
	unsigned long flags;

	irq_reason_usr = wave4_vdi_read_register(dev, W4_VPU_VINT_REASON_USR);
	irq_reason_hw = wave4_vdi_read_register(dev, W4_VPU_VINT_REASON);
	/*
	 * Wave4 can signal completion bits in either VINT_REASON or
	 * VINT_REASON_USR. Use the union so PIC_RUN completions are not lost
	 * when one bank holds stale non-PIC bits.
	 */
	irq_reason = irq_reason_usr | irq_reason_hw;
	if (irq_reason & W4_VPU_INT_BSBUF)
		dev_warn_ratelimited(dev->dev,
				     "encoder bitstream buffer full (reason=%#x)\n",
				     irq_reason);
	if (irq_reason)
		wave4_vdi_write_register(dev, W4_VPU_VINT_REASON_CLR, irq_reason);
	wave4_vdi_write_register(dev, W4_VPU_VINT_CLEAR, 0x1);

	spin_lock_irqsave(&dev->irq_spinlock, flags);
	list_for_each_entry_safe(inst, tmp, &dev->instances, list) {
		if (irq_reason & READ_ONCE(inst->command_irq_mask))
			complete(&inst->command_done);

		if (irq_reason & W4_VPU_INT_PIC_RUN) {
			val = BIT(INT_W4_DEC_PIC);
			kfifo_in(&inst->irq_status, &val, sizeof(int));
			complete(&inst->irq_done);
		}
	}
	spin_unlock_irqrestore(&dev->irq_spinlock, flags);

	if (dev->irq < 0)
		up(&dev->irq_sem);
}

static irqreturn_t wave4_vpu_irq(int irq, void *dev_id)
{
	struct vpu_device *dev = dev_id;

	if (wave4_vdi_read_register(dev, W4_VPU_VPU_INT_STS)) {
		wave4_vpu_handle_irq(dev);
		return IRQ_WAKE_THREAD;
	}

	return IRQ_HANDLED;
}

static irqreturn_t wave4_vpu_irq_thread(int irq, void *dev_id)
{
	struct vpu_device *dev = dev_id;
	struct vpu_instance *inst, *tmp;
	int irq_status, ret;

	mutex_lock(&dev->irq_lock);
	list_for_each_entry_safe(inst, tmp, &dev->instances, list) {
		while (kfifo_len(&inst->irq_status)) {
			ret = kfifo_out(&inst->irq_status, &irq_status, sizeof(int));
			if (!ret)
				break;

			/*
			 * Encoder completion callbacks are valid only while one
			 * async ENC_PIC job is inflight. Ignore stale/duplicate
			 * PIC notifications that arrive after job completion.
			 */
			if (inst->type == VPU_INST_TYPE_ENC &&
			    !READ_ONCE(inst->codec_info->enc_info.async_pm_ref_held))
				continue;

			inst->ops->finish_process(inst);
		}
	}
	mutex_unlock(&dev->irq_lock);

	return IRQ_HANDLED;
}

static void wave4_vpu_irq_work_fn(struct kthread_work *work)
{
	struct vpu_device *dev = container_of(work, struct vpu_device, work);

	if (wave4_vdi_read_register(dev, W4_VPU_VPU_INT_STS))
		wave4_vpu_handle_irq(dev);
}

static enum hrtimer_restart wave4_vpu_timer_callback(struct hrtimer *timer)
{
	struct vpu_device *dev =
			container_of(timer, struct vpu_device, hrtimer);

	kthread_queue_work(dev->worker, &dev->work);
	hrtimer_forward_now(timer, ns_to_ktime(W4_POLL_INTERVAL_MS * NSEC_PER_MSEC));

	return HRTIMER_RESTART;
}

static int irq_thread(void *data)
{
	struct vpu_device *dev = (struct vpu_device *)data;
	struct vpu_instance *inst, *tmp;
	int irq_status, ret;

	while (!kthread_should_stop()) {
		if (down_interruptible(&dev->irq_sem))
			continue;

		if (kthread_should_stop())
			break;

		mutex_lock(&dev->irq_lock);
		list_for_each_entry_safe(inst, tmp, &dev->instances, list) {
			while (kfifo_len(&inst->irq_status)) {
				ret = kfifo_out(&inst->irq_status, &irq_status, sizeof(int));
				if (!ret)
					break;

				/*
				 * Encoder completion callbacks are valid only while one
				 * async ENC_PIC job is inflight. Ignore stale/duplicate
				 * PIC notifications that arrive after job completion.
				 */
				if (inst->type == VPU_INST_TYPE_ENC &&
				    !READ_ONCE(inst->codec_info->enc_info.async_pm_ref_held))
					continue;

				inst->ops->finish_process(inst);
			}
		}
		mutex_unlock(&dev->irq_lock);
	}

	return 0;
}

static int wave4_vpu_load_firmware(struct device *dev, const char *fw_name,
				   u32 *revision)
{
	const struct firmware *fw;
	int ret;
	unsigned int product_id;
	struct vpu_device *vpu_dev = dev_get_drvdata(dev);

	ret = request_firmware(&fw, fw_name, dev);
	if (ret) {
		dev_err(dev, "request_firmware, fail: %d\n", ret);
		return ret;
	}

	ret = wave4_vpu_init_with_bitcode(dev, (u8 *)fw->data, fw->size);
	if (ret) {
		dev_err(dev, "vpu_init_with_bitcode, fail: %d\n", ret);
		release_firmware(fw);
		return ret;
	}
	release_firmware(fw);

	ret = wave4_vpu_get_version_info(dev, revision, &product_id);
	if (ret) {
		if (vpu_dev->fw_running) {
			dev_warn(dev, "vpu_get_version_info fail: %d, continuing with unknown revision\n",
				 ret);
			*revision = 0;
			return 0;
		}
		dev_err(dev, "vpu_get_version_info fail: %d\n", ret);
		return ret;
	}

	dev_dbg(dev, "%s: enum product_id: %08x, fw revision: %u\n",
		__func__, product_id, *revision);

	return 0;
}

static __maybe_unused int wave4_pm_suspend(struct device *dev)
{
	struct vpu_device *vpu = dev_get_drvdata(dev);
	bool restart_timer = false;
	int ret;

	if (w4_forbid_runtime_pm)
		return 0;

	if (pm_runtime_suspended(dev))
		return 0;

	if (vpu->irq < 0)
		restart_timer = hrtimer_cancel(&vpu->hrtimer);

	ret = wave4_vpu_sleep_wake(dev, true, NULL, 0);
	if (ret) {
		if (restart_timer)
			hrtimer_start(&vpu->hrtimer,
				      ns_to_ktime(W4_POLL_INTERVAL_MS * NSEC_PER_MSEC),
				      HRTIMER_MODE_REL_PINNED);
		return ret;
	}
	clk_bulk_disable_unprepare(vpu->num_clks, vpu->clks);

	return 0;
}

static __maybe_unused int wave4_pm_resume(struct device *dev)
{
	struct vpu_device *vpu = dev_get_drvdata(dev);
	int ret = 0;

	if (w4_forbid_runtime_pm)
		return 0;

	ret = clk_bulk_prepare_enable(vpu->num_clks, vpu->clks);
	if (ret) {
		dev_err(dev, "Enabling clocks, fail: %d\n", ret);
		return ret;
	}

	ret = wave4_vpu_sleep_wake(dev, false, NULL, 0);
	if (ret) {
		clk_bulk_disable_unprepare(vpu->num_clks, vpu->clks);
		return ret;
	}

	if (vpu->irq < 0 && !hrtimer_active(&vpu->hrtimer))
		hrtimer_start(&vpu->hrtimer, ns_to_ktime(W4_POLL_INTERVAL_MS * NSEC_PER_MSEC),
			      HRTIMER_MODE_REL_PINNED);

	return ret;
}

static const struct dev_pm_ops wave4_pm_ops = {
	SET_RUNTIME_PM_OPS(wave4_pm_suspend, wave4_pm_resume, NULL)
};

static void wave4_vpu_configure_sg2002_sram_share(struct platform_device *pdev)
{
	struct regmap *syscon;
	void __iomem *vc_ctrl;
	u32 val;
	int ret;

	if (of_find_property(pdev->dev.of_node, "sophgo,syscon", NULL)) {
		syscon = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "sophgo,syscon");
		if (IS_ERR(syscon)) {
			dev_warn(&pdev->dev, "failed to lookup sophgo,syscon: %ld\n",
				 PTR_ERR(syscon));
			goto program_vc_ctrl_direct;
		}
	} else {
		syscon = syscon_regmap_lookup_by_compatible("sophgo,cv1800b-top-syscon");
		if (IS_ERR(syscon)) {
			dev_warn(&pdev->dev, "failed to lookup top syscon: %ld\n", PTR_ERR(syscon));
			goto program_vc_ctrl_direct;
		}
	}

	ret = regmap_update_bits(syscon, SG2002_VC_SRAM_SHARE_OFFSET,
				 SG2002_VC_SRAM_SHARE_MASK,
				 SG2002_VC_SRAM_SHARE_H265);
	if (ret)
		dev_warn(&pdev->dev, "failed to configure VC SRAM share mux: %d\n", ret);

program_vc_ctrl_direct:
	/*
	 * BSP programs VC SRAM mux in the dedicated VC control window:
	 *   base 0x0B030000, offset 0x24, value 0x2 for H265(Wave4).
	 * Keep this direct path to match shipped behavior.
	 */
	vc_ctrl = devm_ioremap(&pdev->dev, SG2002_VC_CTRL_BASE, SG2002_VC_CTRL_SIZE);
	if (IS_ERR(vc_ctrl)) {
		dev_warn(&pdev->dev, "failed to ioremap VC ctrl @0x%x: %ld\n",
			 SG2002_VC_CTRL_BASE, PTR_ERR(vc_ctrl));
		return;
	}

	val = readl(vc_ctrl + SG2002_VC_SRAM_SHARE_OFFSET);
	val &= ~SG2002_VC_SRAM_SHARE_MASK;
	val |= SG2002_VC_SRAM_SHARE_H265;
	writel(val, vc_ctrl + SG2002_VC_SRAM_SHARE_OFFSET);
	val = readl(vc_ctrl + SG2002_VC_SRAM_SHARE_OFFSET);
	if ((val & SG2002_VC_SRAM_SHARE_MASK) != SG2002_VC_SRAM_SHARE_H265)
		dev_warn(&pdev->dev, "VC SRAM share mux verify failed: reg=0x%x\n", val);
	else
		dev_info(&pdev->dev, "configured VC SRAM share mux to H265 path (reg=0x%x)\n", val);
}

static int wave4_vpu_probe(struct platform_device *pdev)
{
	int ret;
	struct vpu_device *dev;
	const struct wave4_match_data *match_data;
	u32 fw_revision;

	match_data = device_get_match_data(&pdev->dev);
	if (!match_data) {
		dev_err(&pdev->dev, "missing device match data\n");
		return -EINVAL;
	}

	/* physical addresses limited to 32 bits */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask: %d\n", ret);
		return ret;
	}

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->has_encoder = !!(match_data->flags & W4_IS_ENC);
	dev->has_decoder = !!(match_data->flags & W4_IS_DEC);
	dev->hw_cap_from_std_def1 = match_data->use_std_def1_caps;
	dev->hw_std_def1_enc_mask = match_data->std_def1_enc_mask;
	dev->hw_std_def1_dec_mask = match_data->std_def1_dec_mask;

	dev->vdb_register = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dev->vdb_register))
		return PTR_ERR(dev->vdb_register);
	ida_init(&dev->inst_ida);

	mutex_init(&dev->dev_lock);
	mutex_init(&dev->hw_lock);
	mutex_init(&dev->irq_lock);
	spin_lock_init(&dev->irq_spinlock);
	dev_set_drvdata(&pdev->dev, dev);
	dev->dev = &pdev->dev;

	ret = of_reserved_mem_device_init(&pdev->dev);
	if (!ret) {
		dev->reserved_mem_inited = true;
		dev_info(&pdev->dev,
			 "attached memory-region DMA pool for wave4 allocations\n");
	} else if (ret != -ENODEV) {
		return dev_err_probe(&pdev->dev, ret,
				     "failed to attach memory-region DMA pool\n");
	}

	dev->resets = devm_reset_control_array_get_optional_exclusive(&pdev->dev);
	if (IS_ERR(dev->resets)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(dev->resets),
				    "Failed to get reset control\n");
		goto err_rmem_release;
	}

	ret = reset_control_deassert(dev->resets);
	if (ret) {
		ret = dev_err_probe(&pdev->dev, ret, "Failed to deassert resets\n");
		goto err_reset_assert;
	}

	ret = devm_clk_bulk_get_all(&pdev->dev, &dev->clks);

	/* continue without clock, assume externally managed */
	if (ret < 0) {
		dev_warn(&pdev->dev, "Getting clocks, fail: %d\n", ret);
		ret = 0;
	}
	dev->num_clks = ret;

	ret = clk_bulk_prepare_enable(dev->num_clks, dev->clks);
	if (ret) {
		dev_err(&pdev->dev, "Enabling clocks, fail: %d\n", ret);
		goto err_reset_assert;
	}

	wave4_vpu_configure_sg2002_sram_share(pdev);

	dev->sram_size = match_data->sram_size;
	of_property_read_u32(pdev->dev.of_node, "sram-size", &dev->sram_size);
	dev->sram_pool = of_gen_pool_get(pdev->dev.of_node, "sram", 0);
	if (!dev->sram_pool) {
		if (match_data->allow_internal_sram && dev->sram_size) {
			dev_info(&pdev->dev,
				 "sram pool not found, using internal SRAM window at daddr 0x0\n");
		} else {
			dev_warn(&pdev->dev,
				 "sram node not found, falling back to DMA-only buffers\n");
			dev->sram_size = 0;
		}
	}

	dev->product_code = wave4_vdi_read_register(dev, VPU_PRODUCT_CODE_REGISTER);
	ret = wave4_vdi_init(&pdev->dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "wave4_vdi_init, fail: %d\n", ret);
		goto err_clk_dis;
	}
	dev->product = wave4_vpu_get_product_id(dev);

	INIT_LIST_HEAD(&dev->instances);

	dev->irq = platform_get_irq(pdev, 0);
	if (dev->irq < 0) {
		dev_info(&pdev->dev, "using polling IRQ backend\n");
		sema_init(&dev->irq_sem, 1);
		dev->irq_thread = kthread_run(irq_thread, dev, "irq thread");
		hrtimer_setup(&dev->hrtimer, &wave4_vpu_timer_callback, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL_PINNED);
		dev->worker = kthread_run_worker(0, "vpu_irq_thread");
		if (IS_ERR(dev->worker)) {
			dev_err(&pdev->dev, "failed to create vpu irq worker\n");
			ret = PTR_ERR(dev->worker);
			goto err_vdi_release;
		}
		kthread_init_work(&dev->work, wave4_vpu_irq_work_fn);
		/*
		 * In polling mode, keep timer alive from probe when runtime PM
		 * callbacks are intentionally bypassed in bring-up mode.
		 */
		if (w4_forbid_runtime_pm)
			hrtimer_start(&dev->hrtimer,
				      ns_to_ktime(W4_POLL_INTERVAL_MS * NSEC_PER_MSEC),
				      HRTIMER_MODE_REL_PINNED);
	} else {
		ret = devm_request_threaded_irq(&pdev->dev, dev->irq, wave4_vpu_irq,
						wave4_vpu_irq_thread, IRQF_ONESHOT, "vpu_irq", dev);
		if (ret) {
			dev_err(&pdev->dev, "Register interrupt handler, fail: %d\n", ret);
			goto err_irq_release;
		}
	}

	ret = wave4_vpu_load_firmware(&pdev->dev, match_data->fw_name, &fw_revision);
	if (ret) {
		dev_err(&pdev->dev, "wave4_vpu_load_firmware, fail: %d\n", ret);
		goto err_irq_release;
	}

	if (dev->hw_cap_queried) {
		dev_info(&pdev->dev,
			 "wave4 capability query: std_def0=0x%08x std_def1=0x%08x conf=0x%08x masks(enc=0x%08x dec=0x%08x) => enc=%u dec=%u\n",
			 dev->hw_std_def0, dev->hw_std_def1, dev->hw_conf_feature,
			 dev->hw_std_def1_enc_mask, dev->hw_std_def1_dec_mask,
			 dev->has_encoder, dev->has_decoder);
	} else if (dev->hw_cap_from_std_def1) {
		dev_warn(&pdev->dev,
			 "wave4 capability query not available; using DT flags (enc=%u dec=%u)\n",
			 dev->has_encoder, dev->has_decoder);
	}

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "v4l2_device_register, fail: %d\n", ret);
		goto err_irq_release;
	}

	if (dev->has_decoder) {
		dev_info(&pdev->dev, "capability: registering decoder device\n");
		ret = wave4_vpu_dec_register_device(dev);
		if (ret) {
			dev_err(&pdev->dev, "wave4_vpu_dec_register_device, fail: %d\n", ret);
			goto err_v4l2_unregister;
		}
	}
	if (dev->has_encoder) {
		dev_info(&pdev->dev, "capability: registering encoder device\n");
		ret = wave4_vpu_enc_register_device(dev);
		if (ret) {
			dev_err(&pdev->dev, "wave4_vpu_enc_register_device, fail: %d\n", ret);
			goto err_dec_unreg;
		}
	} else {
		dev_info(&pdev->dev, "capability: encoder disabled, skip encoder registration\n");
	}

	dev_info(&pdev->dev, "Added wave4 driver with caps: %s %s\n",
		 dev->has_encoder ? "'ENCODE'" : "",
		 dev->has_decoder ? "'DECODE'" : "");
	dev_info(&pdev->dev, "Product Code:      0x%x\n", dev->product_code);
	dev_info(&pdev->dev, "Firmware Revision: %u\n", fw_revision);

	pm_runtime_set_autosuspend_delay(&pdev->dev, 500);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	/*
	 * Do not issue a direct SLEEP command here: that bypasses runtime PM
	 * state accounting and can leave PM core believing the device is active
	 * while firmware is already asleep.
	 */
	if (w4_forbid_runtime_pm) {
		/*
		 * Keep runtime PM API usage intact while preventing
		 * autosuspend transitions in bring-up mode.
		 */
		pm_runtime_forbid(&pdev->dev);
		dev_warn(&pdev->dev,
			 "runtime PM autosuspend forbidden by module param (w4_forbid_runtime_pm=1)\n");
	}

	return 0;

err_dec_unreg:
	if (dev->has_decoder)
		wave4_vpu_dec_unregister_device(dev);
err_v4l2_unregister:
	v4l2_device_unregister(&dev->v4l2_dev);
err_irq_release:
	if (dev->irq < 0) {
		hrtimer_cancel(&dev->hrtimer);
		kthread_cancel_work_sync(&dev->work);
		kthread_destroy_worker(dev->worker);
	}
err_vdi_release:
	if (dev->irq_thread) {
		kthread_stop(dev->irq_thread);
		up(&dev->irq_sem);
		dev->irq_thread = NULL;
	}
	wave4_vdi_release(&pdev->dev);
err_clk_dis:
	clk_bulk_disable_unprepare(dev->num_clks, dev->clks);
err_reset_assert:
	reset_control_assert(dev->resets);
err_rmem_release:
	if (dev->reserved_mem_inited)
		of_reserved_mem_device_release(&pdev->dev);

	return ret;
}

static void wave4_vpu_remove(struct platform_device *pdev)
{
	struct vpu_device *dev = dev_get_drvdata(&pdev->dev);

	if (dev->has_encoder)
		wave4_vpu_enc_unregister_device(dev);
	if (dev->has_decoder)
		wave4_vpu_dec_unregister_device(dev);
	v4l2_device_unregister(&dev->v4l2_dev);

	if (dev->irq < 0) {
		if (dev->irq_thread) {
			kthread_stop(dev->irq_thread);
			up(&dev->irq_sem);
			dev->irq_thread = NULL;
		}

		hrtimer_cancel(&dev->hrtimer);
		kthread_cancel_work_sync(&dev->work);
		kthread_destroy_worker(dev->worker);
	}

	if (w4_forbid_runtime_pm)
		pm_runtime_allow(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	mutex_destroy(&dev->dev_lock);
	mutex_destroy(&dev->hw_lock);
	mutex_destroy(&dev->irq_lock);
	reset_control_assert(dev->resets);
	clk_bulk_disable_unprepare(dev->num_clks, dev->clks);
	wave4_vdi_release(&pdev->dev);
	if (dev->reserved_mem_inited)
		of_reserved_mem_device_release(&pdev->dev);
	ida_destroy(&dev->inst_ida);
}

static const struct wave4_match_data cv1800b_wave4_data = {
	/* Keep DT match flags as the userspace-visible capability contract. */
	.flags = W4_IS_ENC,
	.fw_name = "cnm/wave420l_codec_fw.bin",
	.allow_internal_sram = true,
	.use_std_def1_caps = true,
	.std_def1_enc_mask = W4_STD_DEF1_HEVC_ENC,
	.std_def1_dec_mask = W4_STD_DEF1_HEVC_DEC,
};

static const struct of_device_id wave4_dt_ids[] = {
	{ .compatible = "sophgo,cv1800b-wave420l", .data = &cv1800b_wave4_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wave4_dt_ids);

static struct platform_driver wave4_vpu_driver = {
	.driver = {
		.name = VPU_PLATFORM_DEVICE_NAME,
		.of_match_table = of_match_ptr(wave4_dt_ids),
		.pm = &wave4_pm_ops,
		},
	.probe = wave4_vpu_probe,
	.remove = wave4_vpu_remove,
};

module_platform_driver(wave4_vpu_driver);
MODULE_DESCRIPTION("chips&media Wave4 VPU V4L2 driver");
MODULE_LICENSE("Dual BSD/GPL");
