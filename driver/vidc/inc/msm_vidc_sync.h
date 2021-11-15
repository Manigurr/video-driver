/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __MSM_VIDC_SYNC__
#define __MSM_VIDC_SYNC__

#include <linux/sync_file.h>
#include <synx_api.h>

#include "msm_vidc_internal.h"

struct msm_vidc_inst;
struct msm_v4l2_synx_fence;

enum msm_vidc_sync_flags {
	MSM_VIDC_SYNC_FLAG_ERROR        =      0x1,
	MSM_VIDC_SYNC_FLAG_DEFERRED     =      0x2,
	MSM_VIDC_SYNC_FLAG_QUEUED       =      0x4,
};

struct msm_vidc_sync_private_ioctl_info {
	u32                                    ioctl;
	const char                            *const name;
	int (*func)(struct msm_vidc_inst *inst, struct msm_v4l2_synx_fence *f);
};

struct dma_fence_tracker {
	struct list_head                       submit_list;
	struct list_head                       release_list;
};

struct msm_vidc_sync_fence_timeline {
	struct kref                            kref;
	struct mutex                           lock;
	spinlock_t                             spin_lock;
	bool                                   is_inst_closed;
	char                                   name[64];
	char                                   fence_name[64];
	u32                                    next_value;
	u64                                    context;
	struct dma_fence_tracker               fence_tracker;
	struct synx_session                   *synx_session;
	struct msm_vidc_inst                  *instance;
};

struct msm_vidc_sync_fence {
	struct dma_fence                       base;
	struct sync_file                      *sync;
	char                                   name[64];
	int                                    fd;
	long                                   h_synx;
	struct list_head                       list;
	u32                                    flag;
};

struct msm_vidc_synx_buffer {
	struct list_head                       list;
	u32                                    index;
	struct msm_vidc_sync_fence            *wfence;
	struct msm_vidc_sync_fence            *sfence;
	u32                                    flag;
};

void print_synx_buffer(u32 tag, const char *tag_str, const char *str,
		struct msm_vidc_inst *inst, struct msm_vidc_synx_buffer *sbuf);
int msm_vidc_sync_release_synx_buffer(struct msm_vidc_inst *inst,
		struct msm_vidc_buffer *buf);
struct msm_vidc_synx_buffer *msm_vidc_sync_get_synx_buffer_from_index(
		struct msm_vidc_inst *inst, struct list_head *list_head, u32 index);
struct msm_vidc_synx_buffer *msm_vidc_sync_get_synx_buffer_from_signal_seqno(
		struct msm_vidc_inst *inst, struct list_head *list_head, int fd);
int msm_vidc_sync_destroy_synx_buffer(struct msm_vidc_inst *inst,
		struct msm_vidc_synx_buffer *buf);
int msm_vidc_sync_create_queue_fences(struct msm_vidc_inst *inst,
		struct msm_v4l2_synx_fence *f);
int msm_vidc_sync_update_timeline_name(struct msm_vidc_inst *inst);
int msm_vidc_sync_init_timeline(struct msm_vidc_inst *inst);
void msm_vidc_sync_deinit_timeline(struct msm_vidc_inst *inst);
void msm_vidc_sync_cleanup_synx_buffers(struct msm_vidc_inst *inst,
		u32 tag, const char *tag_str);
#endif
