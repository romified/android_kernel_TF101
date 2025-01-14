/*
 * drivers/video/tegra/host/nvhost_syncpt.c
 *
 * Tegra Graphics Host Syncpoints
 *
 * Copyright (c) 2010, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "nvhost_syncpt.h"
#include "dev.h"

#define client_managed(id) (BIT(id) & NVSYNCPTS_CLIENT_MANAGED)
#define syncpt_to_dev(sp) container_of(sp, struct nvhost_master, syncpt)
#define SYNCPT_CHECK_PERIOD 2*HZ

static int timeout_count =0;
static unsigned int old_sec;

static bool check_max(struct nvhost_syncpt *sp, u32 id, u32 real)
{
	u32 max;
	if (client_managed(id))
		return true;
	smp_rmb();
	max = (u32)atomic_read(&sp->max_val[id]);
	return ((s32)(max - real) >= 0);
}

/**
 * Write the current syncpoint value back to hw.
 */
static void reset_syncpt(struct nvhost_syncpt *sp, u32 id)
{
	struct nvhost_master *dev = syncpt_to_dev(sp);
	int min;
	smp_rmb();
	min = atomic_read(&sp->min_val[id]);
	writel(min, dev->sync_aperture + (HOST1X_SYNC_SYNCPT_0 + id * 4));
}

/**
 * Write the current waitbase value back to hw.
 */
static void reset_syncpt_wait_base(struct nvhost_syncpt *sp, u32 id)
{
	struct nvhost_master *dev = syncpt_to_dev(sp);
	writel(sp->base_val[id],
		dev->sync_aperture + (HOST1X_SYNC_SYNCPT_BASE_0 + id * 4));
}

/**
 * Read waitbase value from hw.
 */
static void read_syncpt_wait_base(struct nvhost_syncpt *sp, u32 id)
{
	struct nvhost_master *dev = syncpt_to_dev(sp);
	sp->base_val[id] = readl(dev->sync_aperture +
				(HOST1X_SYNC_SYNCPT_BASE_0 + id * 4));
}

/**
 * Resets syncpoint and waitbase values to sw shadows
 */
void nvhost_syncpt_reset(struct nvhost_syncpt *sp)
{
	u32 i;
	for (i = 0; i < NV_HOST1X_SYNCPT_NB_PTS; i++)
		reset_syncpt(sp, i);
	for (i = 0; i < NV_HOST1X_SYNCPT_NB_BASES; i++)
		reset_syncpt_wait_base(sp, i);
	wmb();
}

/**
 * Updates sw shadow state for client managed registers
 */
void nvhost_syncpt_save(struct nvhost_syncpt *sp)
{
	u32 i;

	for (i = 0; i < NV_HOST1X_SYNCPT_NB_PTS; i++) {
		if (client_managed(i))
			nvhost_syncpt_update_min(sp, i);
		else
			//BUG_ON(!nvhost_syncpt_min_eq_max(sp, i));
			if (!nvhost_syncpt_min_eq_max(sp, i)) //A temporary workaround....
		        printk("BUG! sp=%p, i=%d",sp,i);
	}

	for (i = 0; i < NV_HOST1X_SYNCPT_NB_BASES; i++)
		read_syncpt_wait_base(sp, i);
}

/**
 * Updates the last value read from hardware.
 */
u32 nvhost_syncpt_update_min(struct nvhost_syncpt *sp, u32 id)
{
	struct nvhost_master *dev = syncpt_to_dev(sp);
	void __iomem *sync_regs = dev->sync_aperture;
	u32 old, live;

	do {
		smp_rmb();
		old = (u32)atomic_read(&sp->min_val[id]);
		live = readl(sync_regs + (HOST1X_SYNC_SYNCPT_0 + id * 4));
	} while ((u32)atomic_cmpxchg(&sp->min_val[id], old, live) != old);

	BUG_ON(!check_max(sp, id, live));

	return live;
}

/**
 * Get the current syncpoint value
 */
u32 nvhost_syncpt_read(struct nvhost_syncpt *sp, u32 id)
{
	u32 val;

	nvhost_module_busy(&syncpt_to_dev(sp)->mod);
	val = nvhost_syncpt_update_min(sp, id);
	nvhost_module_idle(&syncpt_to_dev(sp)->mod);
	return val;
}

/**
 * Write a cpu syncpoint increment to the hardware, without touching
 * the cache. Caller is responsible for host being powered.
 */
void nvhost_syncpt_cpu_incr(struct nvhost_syncpt *sp, u32 id)
{
	struct nvhost_master *dev = syncpt_to_dev(sp);
	BUG_ON(!nvhost_module_powered(&dev->mod));
	BUG_ON(!client_managed(id) && nvhost_syncpt_min_eq_max(sp, id));
	writel(BIT(id), dev->sync_aperture + HOST1X_SYNC_SYNCPT_CPU_INCR);
	wmb();
}

/**
 * Increment syncpoint value from cpu, updating cache
 */
void nvhost_syncpt_incr(struct nvhost_syncpt *sp, u32 id)
{
	nvhost_syncpt_incr_max(sp, id, 1);
	nvhost_module_busy(&syncpt_to_dev(sp)->mod);
	nvhost_syncpt_cpu_incr(sp, id);
	nvhost_module_idle(&syncpt_to_dev(sp)->mod);
}

/**
 * Main entrypoint for syncpoint value waits.
 */
int nvhost_syncpt_wait_timeout(struct nvhost_syncpt *sp, u32 id,
			u32 thresh, u32 timeout, u32 *value)
{
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(wq);
	void *ref;
	int err = 0;

	if (value)
		*value = 0;

	BUG_ON(!check_max(sp, id, thresh));

	/* first check cache */
	if (nvhost_syncpt_min_cmp(sp, id, thresh)) {
		if (value)
			*value = nvhost_syncpt_read_min(sp, id);
		return 0;
	}

	/* keep host alive */
	nvhost_module_busy(&syncpt_to_dev(sp)->mod);

	if (client_managed(id) || !nvhost_syncpt_min_eq_max(sp, id)) {
		/* try to read from register */
		u32 val = nvhost_syncpt_update_min(sp, id);
		if ((s32)(val - thresh) >= 0) {
			if (value)
				*value = val;
			goto done;
		}
	}

	if (!timeout) {
		err = -EAGAIN;
		goto done;
	}

	/* schedule a wakeup when the syncpoint value is reached */
	err = nvhost_intr_add_action(&(syncpt_to_dev(sp)->intr), id, thresh,
				NVHOST_INTR_ACTION_WAKEUP_INTERRUPTIBLE, &wq, &ref);
	if (err)
		goto done;

	err = -EAGAIN;
	/* wait for the syncpoint, or timeout, or signal */
	while (timeout) {
		u32 check = min_t(u32, SYNCPT_CHECK_PERIOD, timeout);
		int remain = wait_event_interruptible_timeout(wq,
						nvhost_syncpt_min_cmp(sp, id, thresh),
						check);
		if (remain > 0 || nvhost_syncpt_min_cmp(sp, id, thresh)) {
			if (value)
				*value = nvhost_syncpt_read_min(sp, id);
			err = 0;
			break;
		}
		if (remain < 0) {
			err = remain;
			break;
		}
		if (timeout != NVHOST_NO_TIMEOUT)
			timeout -= check;
		if (timeout) {
			static struct timeval t_new_stuck;

			dev_warn(&syncpt_to_dev(sp)->pdev->dev,
				"syncpoint id %d (%s) stuck waiting %d\n",
				id, nvhost_syncpt_name(id), thresh);
			nvhost_syncpt_debug(sp);

			do_gettimeofday(&t_new_stuck);
			if ((t_new_stuck.tv_sec - old_sec) > 1) {
				printk("%s: Long time no C. (new, old) = (%d, %d)\n", __FUNCTION__, t_new_stuck.tv_sec, old_sec);
				timeout_count = 1;
			} else {
				timeout_count++;
				printk("%s: Busy timeout? (%d)", __FUNCTION__, timeout_count);
			}
			old_sec = t_new_stuck.tv_sec;

			if (timeout_count >= 3) {
				dev_warn(&syncpt_to_dev(sp)->pdev->dev,
				"Timeout too many times. Force panic()!!\n");
				panic("Force panic from %s", __FUNCTION__);
			}
		}
	};
	nvhost_intr_put_ref(&(syncpt_to_dev(sp)->intr), ref);

done:
	nvhost_module_idle(&syncpt_to_dev(sp)->mod);
	return err;
}

static const char *s_syncpt_names[32] = {
	"gfx_host", "", "", "", "", "", "", "", "", "", "", "",
	"vi_isp_0", "vi_isp_1", "vi_isp_2", "vi_isp_3", "vi_isp_4", "vi_isp_5",
	"2d_0", "2d_1",
	"", "",
	"3d", "mpe", "disp0", "disp1", "vblank0", "vblank1", "mpe_ebm_eof", "mpe_wr_safe",
	"2d_tinyblt", "dsi"
};

const char *nvhost_syncpt_name(u32 id)
{
	BUG_ON(id >= ARRAY_SIZE(s_syncpt_names));
	return s_syncpt_names[id];
}

void nvhost_syncpt_debug(struct nvhost_syncpt *sp)
{
	u32 i;
	for (i = 0; i < NV_HOST1X_SYNCPT_NB_PTS; i++) {
		u32 max = nvhost_syncpt_read_max(sp, i);
		if (!max)
			continue;
		dev_info(&syncpt_to_dev(sp)->pdev->dev,
			"id %d (%s) min %d max %d\n",
			i, nvhost_syncpt_name(i),
			nvhost_syncpt_update_min(sp, i), max);

	}
}

/* returns true, if a <= b < c using wrapping comparison */
static inline bool nvhost_syncpt_is_between(u32 a, u32 b, u32 c)
{
	return b-a < c-a;
}

/* returns true, if x >= y (mod 1 << 32) */
static bool nvhost_syncpt_wrapping_comparison(u32 x, u32 y)
{
	return nvhost_syncpt_is_between(y, x, (1UL<<31UL)+y);
}

/* check for old WAITs to be removed (avoiding a wrap) */
int nvhost_syncpt_wait_check(struct nvmap_client *nvmap,
			struct nvhost_syncpt *sp, u32 waitchk_mask,
			struct nvhost_waitchk *waitp, u32 waitchks)
{
	u32 idx;
	int err = 0;

	/* get current syncpt values */
	for (idx = 0; idx < NV_HOST1X_SYNCPT_NB_PTS; idx++) {
		if (BIT(idx) & waitchk_mask) {
			nvhost_syncpt_update_min(sp, idx);
		}
	}

	BUG_ON(!waitp);

	/* compare syncpt vs wait threshold */
	while (waitchks) {
		u32 syncpt, override;

		BUG_ON(waitp->syncpt_id >= NV_HOST1X_SYNCPT_NB_PTS);

		syncpt = atomic_read(&sp->min_val[waitp->syncpt_id]);
		if (nvhost_syncpt_wrapping_comparison(syncpt, waitp->thresh)) {

			/* wait has completed already, so can be removed */
			dev_dbg(&syncpt_to_dev(sp)->pdev->dev,
					"drop WAIT id %d (%s) thresh 0x%x, syncpt 0x%x\n",
					waitp->syncpt_id,  nvhost_syncpt_name(waitp->syncpt_id),
					waitp->thresh, syncpt);

			/* move wait to a kernel reserved syncpt (that's always 0) */
			override = nvhost_class_host_wait_syncpt(NVSYNCPT_GRAPHICS_HOST, 0);

			/* patch the wait */
			err = nvmap_patch_wait(nvmap,
						(struct nvmap_handle *)waitp->mem,
						waitp->offset, override);
			if (err)
				break;
		}
		waitchks--;
		waitp++;
	}
	return err;
}
