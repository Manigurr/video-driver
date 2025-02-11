/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022,2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _MSM_VIDC_CORE_H_
#define _MSM_VIDC_CORE_H_

#include <linux/platform_device.h>

#include "msm_vidc_internal.h"
#include "venus_hfi_queue.h"
#include "resources.h"

struct msm_vidc_core;

#define MAX_EVENTS 30

#define FOREACH_CORE_STATE(CORE_STATE) {               \
	CORE_STATE(CORE_DEINIT)                        \
	CORE_STATE(CORE_INIT_WAIT)                     \
	CORE_STATE(CORE_INIT)                          \
}

#define call_venus_op(d, op, ...)			\
	(((d) && (d)->venus_ops && (d)->venus_ops->op) ? \
	((d)->venus_ops->op(__VA_ARGS__)):0)

struct msm_vidc_venus_ops {
	int (*boot_firmware)(struct msm_vidc_core *core);
	int (*raise_interrupt)(struct msm_vidc_core *core);
	int (*clear_interrupt)(struct msm_vidc_core *core);
	int (*prepare_pc)(struct msm_vidc_core *core);
	int (*power_on)(struct msm_vidc_core *core);
	int (*power_off)(struct msm_vidc_core *core);
	int (*watchdog)(struct msm_vidc_core *core, u32 intr_status);
	int (*noc_error_info)(struct msm_vidc_core *core);
	int (*setup_intr)(struct msm_vidc_core *core);
};

struct msm_vidc_mem_addr {
	u32 align_device_addr;
	u8 *align_virtual_addr;
	u32 mem_size;
	struct msm_vidc_map   map;
	struct msm_vidc_alloc alloc;
};

struct msm_vidc_iface_q_info {
	void *q_hdr;
	struct msm_vidc_mem_addr q_array;
};

struct msm_video_device {
	enum msm_vidc_domain_type              type;
	struct video_device                    vdev;
	struct v4l2_m2m_dev                   *m2m_dev;
};

struct msm_vidc_core_power {
	u64 clk_freq;
	u64 bw_ddr;
	u64 bw_llcc;
};

enum msm_vidc_core_state FOREACH_CORE_STATE(GENERATE_MSM_VIDC_ENUM);

enum msm_vidc_core_sub_state {
	CORE_SUBSTATE_NONE                   = 0x0,
	CORE_SUBSTATE_POWER_ENABLE           = BIT(0),
	CORE_SUBSTATE_GDSC_HANDOFF           = BIT(1),
	CORE_SUBSTATE_PM_SUSPEND             = BIT(2),
	CORE_SUBSTATE_FW_PWR_CTRL            = BIT(3),
	CORE_SUBSTATE_PAGE_FAULT             = BIT(4),
	CORE_SUBSTATE_CPU_WATCHDOG           = BIT(5),
	CORE_SUBSTATE_VIDEO_UNRESPONSIVE     = BIT(6),
	CORE_SUBSTATE_MAX                    = BIT(7),
};

struct msm_vidc_core {
	struct platform_device                *pdev;
	struct msm_video_device                vdev[2];
	struct v4l2_device                     v4l2_dev;
	struct media_device                    media_dev;
	struct list_head                       instances;
	struct list_head                       dangling_instances;
	struct dentry                         *debugfs_parent;
	struct dentry                         *debugfs_root;
	char                                   fw_version[MAX_NAME_LENGTH];
	enum msm_vidc_core_state               state;
	enum msm_vidc_core_sub_state           sub_state;
	char                                   sub_state_name[MAX_NAME_LENGTH];
	struct mutex                           lock;
	struct msm_vidc_resource              *resource;
	struct msm_vidc_platform              *platform;
	u32                                    intr_status;
	u32                                    spur_count;
	u32                                    reg_count;
	u32                                    codecs_count;
	bool                                   is_hw_virt;
	u32                                    vmid;
	u32                                    device_core_mask;
	bool                                   is_gvm_open;
	struct task_struct                    *pvm_event_handler_thread;
	struct msm_vidc_core_capability       *capabilities;
	struct msm_vidc_inst_capability       *inst_caps;
	struct msm_vidc_mem_addr               sfr;
	struct msm_vidc_mem_addr               iface_q_table;
	struct msm_vidc_iface_q_info           iface_queues[VIDC_IFACEQ_NUMQ];
	struct delayed_work                    pm_work;
	struct workqueue_struct               *pm_workq;
	struct workqueue_struct               *batch_workq;
	struct delayed_work                    fw_unload_work;
	struct work_struct                     ssr_work;
	struct work_struct                     hw_virt_ssr_work;
	u32                                    ssr_dev;
	struct msm_vidc_core_power             power;
	struct msm_vidc_ssr                    ssr;
	u32                                    skip_pc_count;
	u32                                    last_packet_type;
	u8                                    *packet;
	u32                                    packet_size;
	u8                                    *response_packet;
	struct v4l2_file_operations           *v4l2_file_ops;
	struct v4l2_ioctl_ops                 *v4l2_ioctl_ops_enc;
	struct v4l2_ioctl_ops                 *v4l2_ioctl_ops_dec;
	struct v4l2_ctrl_ops                  *v4l2_ctrl_ops;
	struct vb2_ops                        *vb2_ops;
	struct vb2_mem_ops                    *vb2_mem_ops;
	struct v4l2_m2m_ops                   *v4l2_m2m_ops;
	struct msm_vidc_venus_ops             *venus_ops;
	const struct msm_vidc_resources_ops   *res_ops;
	struct msm_vidc_session_ops           *session_ops;
	struct msm_vidc_memory_ops            *mem_ops;
	struct media_device_ops               *media_device_ops;
	u32                                    header_id;
	u32                                    packet_id;
	u32                                    sys_init_id;
};

static inline bool core_in_valid_state(struct msm_vidc_core *core)
{
	return core->state != MSM_VIDC_CORE_DEINIT;
}

#endif // _MSM_VIDC_CORE_H_
