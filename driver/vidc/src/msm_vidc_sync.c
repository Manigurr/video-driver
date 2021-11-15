// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <media/v4l2_vidc_extensions.h>

#include "msm_vidc_sync.h"
#include "msm_vidc_driver.h"
#include "msm_vidc_debug.h"

extern struct msm_vidc_core *g_core;

void print_synx_buffer(u32 tag, const char *tag_str, const char *str, struct msm_vidc_inst *inst,
		struct msm_vidc_synx_buffer *sbuf)
{
	struct msm_vidc_sync_fence *wfence = NULL, *sfence = NULL;
	struct dma_fence *wbase = NULL, *sbase = NULL;
	long wref = -1, sref = -1;

	if (!inst || !sbuf || !tag_str || !str)
		return;

	if (sbuf->wfence) {
		wfence = sbuf->wfence;
		wbase = &wfence->base;
		wref = wfence->sync ? file_count(wfence->sync->file) : -1;
	}

	if (sbuf->sfence) {
		sfence = sbuf->sfence;
		sbase = &sfence->base;
		sref = sfence->sync ? file_count(sfence->sync->file) : -1;
	}

	if (wfence && sfence) {
		dprintk_inst(tag, tag_str, inst,
			"synx: %s: idx %2d, wait: ctx %llu seq %llu ref %u sref %ld fd %d synx %llu flag %u, signal: ctx %llu seq %llu ref %u sref %ld fd %d synx %llu flag %u, sflag %u\n",
			str, sbuf->index,
			wbase->context, wbase->seqno, kref_read(&wbase->refcount),
			wref, wfence->fd, wfence->h_synx, wfence->flag,
			sbase->context, sbase->seqno, kref_read(&sbase->refcount),
			sref, sfence->fd, sfence->h_synx, sfence->flag,
			sbuf->flag);
	} else if (wfence) {
		dprintk_inst(tag, tag_str, inst,
			"synx: %s: idx %2d, wait: ctx %llu seq %llu ref %u sref %ld fd %d synx %llu flag %u, sflag %u\n",
			str, sbuf->index,
			sbase->context, sbase->seqno, kref_read(&sbase->refcount),
			wref, wfence->fd, wfence->h_synx, wfence->flag,
			sbuf->flag);
	} else if (sfence) {
		dprintk_inst(tag, tag_str, inst,
			"synx: %s: idx %2d, signal: ctx %llu seq %llu ref %u sref %ld fd %d synx %llu flag %u, sflag %u\n",
			str, sbuf->index,
			sbase->context, sbase->seqno, kref_read(&sbase->refcount),
			sref, sfence->fd, sfence->h_synx, sfence->flag,
			sbuf->flag);
	}
}

void msm_vidc_sync_fence_timeline_destroy(struct kref *kref)
{
	struct msm_vidc_sync_fence_timeline *tl =
		container_of(kref, struct msm_vidc_sync_fence_timeline, kref);

	/* print as error, if inst is closed */
	d_vpr_h("%s: %s\n", __func__, tl->name);

	kfree(tl);
}

static inline struct msm_vidc_sync_fence_timeline *sync_fence_timeline_get(
		struct msm_vidc_sync_fence_timeline *tl)
{
	if (tl)
		tl = (kref_get_unless_zero(&tl->kref) > 0) ? tl : NULL;

	return tl;
}

static inline void sync_fence_timeline_put(struct msm_vidc_sync_fence_timeline *tl)
{
	if (tl)
		kref_put(&tl->kref, msm_vidc_sync_fence_timeline_destroy);
}

static inline struct msm_vidc_sync_fence *to_msm_vidc_sync_fence(struct dma_fence *fence)
{
	return container_of(fence, struct msm_vidc_sync_fence, base);
}

static inline struct msm_vidc_sync_fence_timeline
		*to_msm_vidc_sync_fence_timeline(struct dma_fence *fence)
{
	return container_of(fence->lock, struct msm_vidc_sync_fence_timeline, spin_lock);
}

static inline struct msm_vidc_inst *to_msm_vidc_inst(struct dma_fence *fence)
{
	struct msm_vidc_sync_fence_timeline *tl;

	tl = to_msm_vidc_sync_fence_timeline(fence);
	return tl->instance;
}

static const char *msm_vidc_sync_fence_get_driver_name(struct dma_fence *fence)
{
	return "msm_vidc";
}

static const char *msm_vidc_sync_fence_get_timeline_name(struct dma_fence *fence)
{
	struct msm_vidc_sync_fence_timeline *tl = to_msm_vidc_sync_fence_timeline(fence);

	return tl->name;
}

static bool msm_vidc_sync_fence_enable_signaling(struct dma_fence *fence)
{
	return true;
}

static bool msm_vidc_sync_fence_signaled(struct dma_fence *fence)
{
	struct msm_vidc_sync_fence *f = to_msm_vidc_sync_fence(fence);
	struct msm_vidc_inst *inst = to_msm_vidc_inst(fence);
	bool signal = false;

	/* take inst->kref to avoid use-after-free issues */
	inst = get_inst_ref(g_core, inst);
	if (!inst) {
		d_vpr_e("%s: invalid instance\n", __func__);
		return false;
	}

	/**
	 * Incase of session_error frame processing willnot be continued.
	 * So enable signaled flag to wakeup other running devices/co-processors
	 * to avoid deadlock issues.
	 */
	signal = is_session_error(inst);
	if (signal)
		d_vpr_e("%s: signaled due to session error\n", f->name);

	put_inst(inst);
	return signal;
}

static void msm_vidc_sync_fence_release(struct dma_fence *fence)
{
	struct msm_vidc_sync_fence_timeline *tl;
	struct msm_vidc_sync_fence *f;
	bool is_err = false;

	if (fence) {
		f = to_msm_vidc_sync_fence(fence);
		tl = to_msm_vidc_sync_fence_timeline(fence);

		/* remove node from release_list and destroy sync_fence */
		mutex_lock(&tl->lock);
		is_err = tl->is_inst_closed;
		list_del_init(&f->list);
		mutex_unlock(&tl->lock);

		/* report as error, if inst is closed */
		if (is_err)
			d_vpr_e("%s: %s pending %u\n", __func__, f->name, kref_read(&tl->kref));
		else
			d_vpr_l("%s: %s pending %u\n", __func__, f->name, kref_read(&tl->kref));

		/* decrement timeline refcount */
		sync_fence_timeline_put(tl);
		kfree(f);
	}
}

static void msm_vidc_sync_fence_value_str(struct dma_fence *fence, char *str, int size)
{
	struct msm_vidc_sync_fence *f;

	if (!fence || !str)
		return;

	f = to_msm_vidc_sync_fence(fence);
	snprintf(str, size, "%s", f->name);
}

static void msm_vidc_sync_fence_timeline_value_str(struct dma_fence *fence, char *str,
		int size)
{
	struct msm_vidc_sync_fence_timeline *tl = to_msm_vidc_sync_fence_timeline(fence);

	if (!fence || !str)
		return;

	snprintf(str, size, "%d", tl->next_value);
}

static struct dma_fence_ops msm_vidc_sync_fence_ops = {
	.get_driver_name     = msm_vidc_sync_fence_get_driver_name,
	.get_timeline_name   = msm_vidc_sync_fence_get_timeline_name,
	.enable_signaling    = msm_vidc_sync_fence_enable_signaling,
	.signaled            = msm_vidc_sync_fence_signaled,
	.wait                = dma_fence_default_wait,
	.release             = msm_vidc_sync_fence_release,
	.fence_value_str     = msm_vidc_sync_fence_value_str,
	.timeline_value_str  = msm_vidc_sync_fence_timeline_value_str,
};

static struct msm_vidc_sync_fence *msm_vidc_sync_get_sync_fence(
		struct msm_vidc_inst *inst,
		struct msm_vidc_sync_fence_timeline *tl)
{
	struct msm_vidc_sync_fence *fence;
	u32 val;

	if (!inst || !tl) {
		d_vpr_e("%s: invalid params\n", __func__);
		return NULL;
	}

	/* get a timeline refcount and put when sync_fence released */
	tl = sync_fence_timeline_get(tl);
	if (!tl) {
		i_vpr_e(inst, "%s: timeline lastref released\n", __func__);
		return NULL;
	}

	/* allocate sync_fence and attach to submit_list */
	fence = kzalloc(sizeof(struct msm_vidc_sync_fence), GFP_KERNEL);
	if (!fence) {
		sync_fence_timeline_put(tl);
		i_vpr_e(inst, "%s: %s: sync fence alloc failed\n", __func__, tl->name);
		return NULL;
	}
	val = ++(tl->next_value);
	/* init native dma_fence */
	dma_fence_init(&fence->base, &msm_vidc_sync_fence_ops, &tl->spin_lock,
		tl->context, val);
	snprintf(fence->name, sizeof(fence->name), "%s seq %llu", tl->fence_name, val);

	/* attach newly created node to submit_list */
	INIT_LIST_HEAD(&fence->list);
	mutex_lock(&tl->lock);
	list_add_tail(&fence->list, &tl->fence_tracker.submit_list);
	mutex_unlock(&tl->lock);

	return fence;
}

static int msm_vidc_sync_put_sync_fence(struct msm_vidc_inst *inst,
		struct msm_vidc_sync_fence *fence)
{
	if (!inst || !fence) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	/* send signal with fence error for other clients to recover */
	if (fence->flag & MSM_VIDC_SYNC_FLAG_ERROR) {
		dma_fence_set_error(&fence->base, -ENOENT);
		dma_fence_signal(&fence->base);
	}

	/* decr native dma_fence refcount */
	dma_fence_put(&fence->base);

	return 0;
}

static int msm_vidc_sync_get_sync_file_fd(struct msm_vidc_inst *inst,
		struct msm_vidc_sync_fence *fence)
{
	struct sync_file *sync_file;
	int fd = 0;

	if (!inst || !fence) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	/* pick unused fd from current->fdt table */
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		i_vpr_e(inst, "%s: fail to get unused fd\n", __func__);
		return fd;
	}

	/* create sync_file using native dma_fence */
	sync_file = sync_file_create(&fence->base);
	if (!sync_file) {
		put_unused_fd(fd);
		i_vpr_e(inst, "%s: %s failed to create sync file\n", __func__, fence->name);
		return -ENOMEM;
	}

	/* attach sync_file with fd */
	fd_install(fd, sync_file->file);
	fence->sync = sync_file;
	fence->fd = fd;

	return fd;
}

static int msm_vidc_sync_put_sync_file_fd(struct msm_vidc_inst *inst,
		struct msm_vidc_sync_fence *fence)
{
	struct sync_file *sync_file;

	if (!inst || !fence || !fence->sync) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	sync_file = fence->sync;

	/* decr file->refcount to release sync_file */
	fput(sync_file->file);
	fence->sync = NULL;

	return 0;
}

static struct msm_vidc_sync_fence *get_first_entry(struct msm_vidc_inst *inst,
		struct msm_vidc_sync_fence_timeline *tl)
{
	struct msm_vidc_sync_fence *fence = NULL;

	if (!inst || !tl) {
		d_vpr_e("%s: invalid params\n", __func__);
		return NULL;
	}

	mutex_lock(&tl->lock);
	if (list_empty(&tl->fence_tracker.submit_list))
		goto unlock;

	/* always pick 1st node to maintain FIFO */
	fence = list_first_entry(&tl->fence_tracker.submit_list,
		struct msm_vidc_sync_fence, list);
	list_del_init(&fence->list);

unlock:
	mutex_unlock(&tl->lock);
	return fence;
}

static u32 msm_vidc_sync_get_synx_handle(struct msm_vidc_inst *inst,
		struct msm_vidc_sync_fence *fence)
{
	struct msm_vidc_sync_fence_timeline *tl;
	struct synx_import_params params;
	u32 h_synx = 0;
	int rc = 0;

	if (!inst || !fence) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	tl = to_msm_vidc_sync_fence_timeline(&fence->base);

	memset(&params, 0, sizeof(struct synx_import_params));
	params.type = SYNX_IMPORT_INDV_PARAMS;
	params.indv.fence = &fence->base;
	params.indv.flags = SYNX_IMPORT_DMA_FENCE | SYNX_IMPORT_GLOBAL_FENCE;
	params.indv.new_h_synx = &h_synx;
	/* import synx handle from dma_fence */
	rc = synx_import(tl->synx_session, &params);
	if (rc) {
		i_vpr_e(inst, "%s: synx_import failed - %s\n", __func__, fence->name);
		return -EINVAL;
	}
	fence->h_synx = h_synx;

	return h_synx;
}

static int msm_vidc_sync_put_synx_handle(struct msm_vidc_inst *inst,
		struct msm_vidc_sync_fence *fence)
{
	struct msm_vidc_sync_fence_timeline *tl;
	int rc = 0;

	if (!inst || !fence) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	tl = to_msm_vidc_sync_fence_timeline(&fence->base);

	/* release synx handle */
	rc = synx_release(tl->synx_session, fence->h_synx);
	if (rc) {
		i_vpr_e(inst, "%s: synx release failed. rc %d, status %d\n",
			__func__, rc, synx_get_status(tl->synx_session, fence->h_synx));
		return rc;
	}

	return 0;
}

int msm_vidc_sync_release_synx_buffer(struct msm_vidc_inst *inst,
		struct msm_vidc_buffer *buf)
{
	struct msm_vidc_synx_buffer *sbuf, *rbuf;
	struct list_head *slist, *rlist;
	struct dma_fence *base;

	if (!inst || !buf) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	/* pick release eligible node from submit_list(based on buf->index). */
	slist = &inst->synx_tracker.submit_list;
	sbuf = msm_vidc_sync_get_synx_buffer_from_index(inst, slist, buf->index);
	if (!sbuf) {
		i_vpr_e(inst, "%s: index not found %u\n", __func__, buf->index);
		return -EINVAL;
	}
	if (!(sbuf->flag & MSM_VIDC_SYNC_FLAG_QUEUED)) {
		print_synx_buffer(VIDC_ERR, "err ", "not yet queued", inst, sbuf);
		return -EINVAL;
	}
	list_del_init(&sbuf->list);
	/* remove queued flag */
	sbuf->flag &= ~MSM_VIDC_SYNC_FLAG_QUEUED;

	/**
	 * for flushed buffers - attach error flag to notify fence error
	 * with signal to unblock waiting clients.
	 */
	if (!buf->data_size && sbuf->sfence)
		sbuf->sfence->flag |= MSM_VIDC_SYNC_FLAG_ERROR;

	/* move eligible node from submit_list -> release_list */
	print_synx_buffer(VIDC_LOW, "low ", "move to release", inst, sbuf);
	rlist = &inst->synx_tracker.release_list;
	list_add_tail(&sbuf->list, rlist);

	/* for initial 'n' buffers wait fence willnot be set */
	if (!sbuf->wfence)
		return 0;

	base = &sbuf->wfence->base;
	/**
	 * Signal fence seqno is basically ahead of wait fence seqno by 'x' num.
	 * So destroy signal fence only after completing 'x' frames decode.
	 * This canbe achieved by picking synx buffer, for which signal fence seqno
	 * matches with current node wait fence seqno(sbuf->wfence.seqno).
	 */
	rbuf = msm_vidc_sync_get_synx_buffer_from_signal_seqno(inst, rlist, base->seqno);
	if (!rbuf) {
		i_vpr_e(inst, "%s: get synx buffer failed for seqno %u\n", __func__, base->seqno);
		return -EINVAL;
	}
	list_del_init(&rbuf->list);
	print_synx_buffer(VIDC_HIGH, "high", "release", inst, rbuf);

	/* destroy finally chosen synx buffer */
	msm_vidc_sync_destroy_synx_buffer(inst, rbuf);

	return 0;
}

struct msm_vidc_synx_buffer *msm_vidc_sync_get_synx_buffer_from_index(
		struct msm_vidc_inst *inst,
		struct list_head *list_head, u32 index)
{
	struct msm_vidc_synx_buffer *buf = NULL;
	bool found = false;

	if (!inst || !list_head) {
		d_vpr_e("%s: invalid params\n", __func__);
		return NULL;
	}

	/* retrieve synx buffer based on index */
	list_for_each_entry(buf, list_head, list) {
		if (buf->index == index) {
			found = true;
			break;
		}
	}

	return found ? buf : NULL;
}

struct msm_vidc_synx_buffer *msm_vidc_sync_get_synx_buffer_from_signal_seqno(
		struct msm_vidc_inst *inst,
		struct list_head *list_head, int seqno)
{
	struct msm_vidc_synx_buffer *buf = NULL;
	struct msm_vidc_sync_fence *fence = NULL;
	bool found = false;

	if (!inst || !list_head) {
		d_vpr_e("%s: invalid params\n", __func__);
		return NULL;
	}

	/* retrieve synx buffer based on signal fence seqno match */
	list_for_each_entry(buf, list_head, list) {
		fence = buf->sfence;
		if (fence && fence->base.seqno == seqno) {
			found = true;
			break;
		}
	}

	return found ? buf : NULL;
}

static int msm_vidc_sync_destroy_sync_fence(struct msm_vidc_inst *inst,
		struct msm_vidc_sync_fence *fence)
{
	struct msm_vidc_sync_fence_timeline *tl;

	tl = to_msm_vidc_sync_fence_timeline(&fence->base);

	/* declare passed fence as release candidate */
	mutex_lock(&tl->lock);
	list_add_tail(&fence->list, &tl->fence_tracker.release_list);
	mutex_unlock(&tl->lock);

	/* release synx handle */
	msm_vidc_sync_put_synx_handle(inst, fence);
	/* release sync_file by decrementing file refcount */
	msm_vidc_sync_put_sync_file_fd(inst, fence);
	/* release sync_fence by decrementing dma_fence refcount */
	msm_vidc_sync_put_sync_fence(inst, fence);

	return 0;
}

static struct msm_vidc_sync_fence *msm_vidc_sync_allocate_sync_fence(
		struct msm_vidc_inst *inst,
		struct msm_vidc_sync_fence_timeline *tl)
{
	struct msm_vidc_sync_fence *fence;
	long handle;
	int fd, rc = 0;

	/* allocate and init sync_fence */
	fence = msm_vidc_sync_get_sync_fence(inst, tl);
	if (!fence)
		return NULL;

	/* create sync_file and install fd */
	fd = msm_vidc_sync_get_sync_file_fd(inst, fence);
	if (fd < 0) {
		rc = -EINVAL;
		goto put_vidc_fence;
	}

	/* import synx handle from native dma_fence */
	handle = msm_vidc_sync_get_synx_handle(inst, fence);
	if (handle < 0) {
		rc = -EINVAL;
		goto put_vidc_fd;
	}

	return fence;
put_vidc_fd:
	msm_vidc_sync_put_sync_file_fd(inst, fence);
put_vidc_fence:
	msm_vidc_sync_put_sync_fence(inst, fence);
	return NULL;
}

int msm_vidc_sync_destroy_synx_buffer(struct msm_vidc_inst *inst,
		struct msm_vidc_synx_buffer *buf)
{
	if (!inst || !buf) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	/* move wait fence to release list and decr fence->refcount */
	if (buf->wfence) {
		msm_vidc_sync_destroy_sync_fence(inst, buf->wfence);
		buf->wfence = NULL;
	}
	/* move signal fence to release list and decr fence->refcount */
	if (buf->sfence) {
		msm_vidc_sync_destroy_sync_fence(inst, buf->sfence);
		buf->sfence = NULL;
	}
	msm_memory_free(inst, buf);

	return 0;
}

static int msm_vidc_sync_create_synx_buffer(struct msm_vidc_inst *inst,
		struct msm_v4l2_synx_fence *f)
{
	struct msm_vidc_synx_buffer *buf;
	int rc = 0;

	if (!inst || !f) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	buf = msm_memory_alloc(inst, MSM_MEM_POOL_SYNX_BUFFER);
	if (!buf) {
		i_vpr_e(inst, "%s: alloc failed\n", __func__);
		return -EINVAL;
	}
	/* treat every fence as deferred fence initially */
	buf->flag |= MSM_VIDC_SYNC_FLAG_DEFERRED;
	buf->index = f->index;
	buf->sfence = get_first_entry(inst, inst->signal_timeline);
	if (f->wait_fd != -1)
		buf->wfence = get_first_entry(inst, inst->wait_timeline);

	list_add_tail(&buf->list, &inst->synx_tracker.submit_list);

	return rc;
}

int msm_vidc_sync_create_queue_fences(struct msm_vidc_inst *inst,
		struct msm_v4l2_synx_fence *f)
{
	struct msm_vidc_sync_fence *wfence, *sfence;
	struct dma_fence *wbase, *sbase;
	long wref = -1, sref = -1;
	int rc = 0;

	if (!inst || !f) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	/* fence create ioctl is allowed only at output port side */
	if (f->type != OUTPUT_MPLANE) {
		i_vpr_e(inst, "%s: unsupported port %d\n", f->type);
		return -EINVAL;
	}
	/* allocate wait sync_fence and attach to &tl->fence_tracker.submit_list */
	wfence = msm_vidc_sync_allocate_sync_fence(inst, inst->wait_timeline);
	if (!wfence) {
		i_vpr_e(inst, "%s: wait fence allocation failed\n", __func__);
		rc = -EINVAL;
		goto exit;
	}
	/* allocate signal sync_fence and attach to &tl->fence_tracker.submit_list */
	sfence = msm_vidc_sync_allocate_sync_fence(inst, inst->signal_timeline);
	if (!sfence) {
		i_vpr_e(inst, "%s: signal fence allocation failed\n", __func__);
		rc = -EINVAL;
		goto put_wfence;
	}
	/* create synx buffer and attach to &inst->synx_tracker.submit_list */
	rc = msm_vidc_sync_create_synx_buffer(inst, f);
	if (rc) {
		i_vpr_e(inst, "%s: synx buffer creation failed\n", __func__);
		goto put_sfence;
	}
	wbase = &wfence->base;
	sbase = &sfence->base;
	f->wait_fd = wfence->fd;
	f->signal_fd = sfence->fd;
	wref = wfence->sync ? file_count(wfence->sync->file) : -1;
	sref = sfence->sync ? file_count(sfence->sync->file) : -1;

	i_vpr_h(inst,
		"synx: create: idx %2d, wait: ctx %llu seq %llu ref %u sref %ld fd %d synx %llu flag %u, signal: ctx %llu seq %llu ref %u sref %ld fd %d synx %llu flag %u\n",
		f->index,
		wbase->context, wbase->seqno, kref_read(&wbase->refcount),
		wref, wfence->fd, wfence->h_synx, wfence->flag,
		sbase->context, sbase->seqno, kref_read(&sbase->refcount),
		sref, sfence->fd, sfence->h_synx, sfence->flag);

	return 0;
put_sfence:
	msm_vidc_sync_destroy_sync_fence(inst, sfence);
put_wfence:
	msm_vidc_sync_destroy_sync_fence(inst, wfence);
exit:
	return rc;
}

int msm_vidc_sync_update_timeline_name(struct msm_vidc_inst *inst)
{
	struct msm_vidc_sync_fence_timeline *tl;

	if (!inst) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	/* update wait timeline details */
	tl = inst->wait_timeline;
	if (tl) {
		snprintf(tl->name, sizeof(tl->name),
			"%s: wait_timeline: ctx %llu", inst->debug_str, tl->context);
		snprintf(tl->fence_name, sizeof(tl->fence_name),
			"%s: wait_fence: ctx %llu", inst->debug_str, tl->context);
	}

	/* update signal timeline details */
	tl = inst->signal_timeline;
	if (tl) {
		snprintf(tl->name, sizeof(tl->name),
			"%s: signal_timeline: ctx %llu", inst->debug_str, tl->context);
		snprintf(tl->fence_name, sizeof(tl->fence_name),
			"%s: signal_fence: ctx %llu", inst->debug_str, tl->context);
	}

	return 0;
}

int msm_vidc_sync_init_timeline(struct msm_vidc_inst *inst)
{
	struct synx_initialization_params params;
	struct msm_vidc_sync_fence_timeline *tl;
	char synx_name[64];
	int rc = 0;

	if (!inst) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	/* allocate wait sync_fence_timeline */
	tl = kzalloc(sizeof(struct msm_vidc_sync_fence_timeline), GFP_KERNEL);
	if (!tl) {
		i_vpr_e(inst, "%s: wait sync_fence_timeline alloc failed\n", __func__);
		return -EINVAL;
	}
	/* allocate individual context for wait timeline */
	tl->context = dma_fence_context_alloc(1);
	memset(&params, 0, sizeof(struct synx_initialization_params));
	memset(&synx_name, 0, sizeof(synx_name));
	snprintf(synx_name, sizeof(synx_name), "%08x: video_synx_wait: ctx %llu",
		inst->session_id, tl->context);
	params.name = (const char *)synx_name;
	params.id = SYNX_CLIENT_VID_CTX0;
	/* create synx session */
	tl->synx_session = synx_initialize(&params);
	if (IS_ERR_OR_NULL(tl->synx_session)) {
		i_vpr_e(inst, "%s: wait synx_initialize failed\n", __func__);
		kfree(tl);
		return -EINVAL;
	}
	tl->instance = inst;
	kref_init(&tl->kref);
	mutex_init(&tl->lock);
	spin_lock_init(&tl->spin_lock);
	INIT_LIST_HEAD(&tl->fence_tracker.submit_list);
	INIT_LIST_HEAD(&tl->fence_tracker.release_list);
	inst->wait_timeline = tl;

	/* allocate signal sync_fence_timeline */
	tl = kzalloc(sizeof(struct msm_vidc_sync_fence_timeline), GFP_KERNEL);
	if (!tl) {
		i_vpr_e(inst, "%s: signal sync_fence_timeline alloc failed\n", __func__);
		return -EINVAL;
	}
	/* allocate individual context for signal timeline */
	tl->context = dma_fence_context_alloc(1);
	memset(&synx_name, 0, sizeof(synx_name));
	snprintf(synx_name, sizeof(synx_name), "%08x: video_synx_signal: ctx %llu",
		inst->session_id, tl->context);
	params.name = (const char *)synx_name;
	params.id = SYNX_CLIENT_VID_CTX0;
	/* create synx session */
	tl->synx_session = synx_initialize(&params);
	if (IS_ERR_OR_NULL(tl->synx_session)) {
		i_vpr_e(inst, "%s: signal synx_initialize failed\n", __func__);
		kfree(tl);
		return -EINVAL;
	}
	tl->instance = inst;
	kref_init(&tl->kref);
	mutex_init(&tl->lock);
	spin_lock_init(&tl->spin_lock);
	INIT_LIST_HEAD(&tl->fence_tracker.submit_list);
	INIT_LIST_HEAD(&tl->fence_tracker.release_list);
	inst->signal_timeline = tl;

	return rc;
}

void msm_vidc_sync_deinit_timeline(struct msm_vidc_inst *inst)
{
	struct msm_vidc_sync_fence_timeline *tl = NULL;

	if (!inst) {
		d_vpr_e("%s: invalid params\n", __func__);
		return;
	}

	tl = inst->wait_timeline;
	if (tl) {
		/* mark inst_closed flag to print err log */
		tl->is_inst_closed = true;
		/* destroy wait synx session */
		synx_uninitialize(tl->synx_session);
		/* deinit wait sync_fence_timeline */
		sync_fence_timeline_put(tl);
	}

	tl = inst->signal_timeline;
	if (tl) {
		/* mark inst_closed flag to print err log */
		tl->is_inst_closed = true;
		/* destroy signal synx session */
		synx_uninitialize(tl->synx_session);
		/* deinit signal sync_fence_timeline */
		sync_fence_timeline_put(tl);
	}
}

void msm_vidc_sync_cleanup_synx_buffers(struct msm_vidc_inst *inst,
		u32 tag, const char *tag_str)
{
	struct msm_vidc_synx_buffer *sbuf, *dummy_sbuf;
	struct msm_vidc_sync_fence *fence, *dummy_fence;
	struct msm_vidc_sync_fence_timeline *tl;
	struct dma_fence *base;
	struct list_head fence_list;
	long sync_ref = -1;

	if (!inst || !tag_str) {
		d_vpr_e("%s: invalid params\n", __func__);
		return;
	}

	/* flush synx buffers from submit_list  */
	list_for_each_entry_safe(sbuf, dummy_sbuf, &inst->synx_tracker.submit_list, list) {
		list_del_init(&sbuf->list);
		if (sbuf->sfence)
			sbuf->sfence->flag |= MSM_VIDC_SYNC_FLAG_ERROR;
		print_synx_buffer(tag, tag_str, "destroy: submit", inst, sbuf);
		msm_vidc_sync_destroy_synx_buffer(inst, sbuf);
	}

	/* flush synx buffers from release_list  */
	list_for_each_entry_safe(sbuf, dummy_sbuf, &inst->synx_tracker.release_list, list) {
		list_del_init(&sbuf->list);
		if (sbuf->sfence)
			sbuf->sfence->flag |= MSM_VIDC_SYNC_FLAG_ERROR;
		print_synx_buffer(tag, tag_str, "destroy: release", inst, sbuf);
		msm_vidc_sync_destroy_synx_buffer(inst, sbuf);
	}

	tl = inst->wait_timeline;
	if (tl) {
		INIT_LIST_HEAD(&fence_list);
		/* move elements to local list(&fence_list) to avoid locking issues */
		mutex_lock(&tl->lock);
		list_replace_init(&tl->fence_tracker.submit_list, &fence_list);
		mutex_unlock(&tl->lock);

		/* flush sync fence from wait timeline submit list */
		list_for_each_entry_safe(fence, dummy_fence, &fence_list, list) {
			list_del_init(&fence->list);
			base = &fence->base;
			sync_ref = fence->sync ? file_count(fence->sync->file) : -1;
			dprintk_inst(tag, tag_str, inst,
				"synx: destroy: wait_submit: ctx %llu seq %llu ref %u sref %ld fd %d synx %llu flag %u\n",
				base->context, base->seqno, kref_read(&base->refcount),
				sync_ref, fence->fd, fence->h_synx, fence->flag);
			msm_vidc_sync_destroy_sync_fence(inst, fence);
		}

		/* print leaked fences from wait timeline release list */
		mutex_lock(&tl->lock);
		list_for_each_entry(fence, &tl->fence_tracker.release_list, list) {
			base = &fence->base;
			sync_ref = fence->sync ? file_count(fence->sync->file) : -1;
			dprintk_inst(tag, tag_str, inst,
				"synx: destroy: wait_release: ctx %llu seq %llu ref %u sref %ld fd %d synx %llu flag %u\n",
				base->context, base->seqno, kref_read(&base->refcount),
				sync_ref, fence->fd, fence->h_synx, fence->flag);
		}
		mutex_unlock(&tl->lock);
	}

	tl = inst->signal_timeline;
	if (tl) {
		INIT_LIST_HEAD(&fence_list);
		/* move elements to local list(&fence_list) to avoid locking issues */
		mutex_lock(&tl->lock);
		list_replace_init(&tl->fence_tracker.submit_list, &fence_list);
		mutex_unlock(&tl->lock);

		/* flush sync fence from signal timeline submit list */
		list_for_each_entry_safe(fence, dummy_fence, &fence_list, list) {
			list_del_init(&fence->list);
			fence->flag |= MSM_VIDC_SYNC_FLAG_ERROR;
			base = &fence->base;
			sync_ref = fence->sync ? file_count(fence->sync->file) : -1;
			dprintk_inst(tag, tag_str, inst,
				"synx: destroy: signal_submit: ctx %llu seq %llu ref %u sref %ld fd %d synx %llu flag %u\n",
				base->context, base->seqno, kref_read(&base->refcount),
				sync_ref, fence->fd, fence->h_synx, fence->flag);
			msm_vidc_sync_destroy_sync_fence(inst, fence);
		}

		/* print leaked fences from signal timeline release list */
		mutex_lock(&tl->lock);
		list_for_each_entry(fence, &tl->fence_tracker.release_list, list) {
			base = &fence->base;
			sync_ref = fence->sync ? file_count(fence->sync->file) : -1;
			dprintk_inst(tag, tag_str, inst,
				"synx: destroy: signal_release: ctx %llu seq %llu ref %u sref %ld fd %d synx %llu flag %u\n",
				base->context, base->seqno, kref_read(&base->refcount),
				sync_ref, fence->fd, fence->h_synx, fence->flag);
		}
		mutex_unlock(&tl->lock);
	}
}
