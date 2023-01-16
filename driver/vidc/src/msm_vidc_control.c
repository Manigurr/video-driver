// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "msm_vidc_debug.h"
#include "msm_vidc_internal.h"
#include "msm_vidc_driver.h"
#include "msm_venc.h"
#include "msm_vidc_platform.h"

extern struct msm_vidc_core *g_core;

static bool is_priv_ctrl(u32 id)
{
	bool private = false;

	if (IS_PRIV_CTRL(id))
		return true;

	/*
	 * Treat below standard controls as private because
	 * we have added custom values to the controls
	 */
	switch (id) {
	/*
	 * TODO: V4L2_CID_MPEG_VIDEO_HEVC_PROFILE is std ctrl. But
	 * V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10_STILL_PICTURE support is not
	 * available yet. Hence, make this as private ctrl for time being
	 */
	case V4L2_CID_MPEG_VIDEO_HEVC_PROFILE:
		private = true;
		break;
	default:
		private = false;
		break;
	}

	return private;
}

static const char *const mpeg_video_blur_types[] = {
	"Blur None",
	"Blur External",
	"Blur Adaptive",
	NULL,
};

static const char *const mpeg_video_hevc_profile[] = {
	"Main",
	"Main Still Picture",
	"Main 10",
	"Main 10 Still Picture",
	NULL,
};

static const char * const av1_profile[] = {
	"Main",
	"High",
	"Professional",
	NULL,
};

static const char * const av1_level[] = {
	"2.0",
	"2.1",
	"2.2",
	"2.3",
	"3.0",
	"3.1",
	"3.2",
	"3.3",
	"4.0",
	"4.1",
	"4.2",
	"4.3",
	"5.0",
	"5.1",
	"5.2",
	"5.3",
	"6.0",
	"6.1",
	"6.2",
	"6.3",
	"7.0",
	"7.1",
	"7.2",
	"7.3",
	NULL,
};

static const char * const av1_tier[] = {
	"Main",
	"High",
	NULL,
};

static const char *const mpeg_video_vidc_ir_type[] = {
	"Random",
	"Cyclic",
	NULL,
};

static const char * const * msm_vidc_get_qmenu_type(
		struct msm_vidc_inst *inst, u32 cap_id)
{
	switch (cap_id) {
	case BLUR_TYPES:
		return mpeg_video_blur_types;
	case PROFILE:
		if (inst->codec == MSM_VIDC_HEVC || inst->codec == MSM_VIDC_HEIC) {
			return mpeg_video_hevc_profile;
		} else if (inst->codec == MSM_VIDC_AV1) {
			return av1_profile;
		} else {
			i_vpr_e(inst, "%s: invalid codec type %d for cap id %d\n",
				__func__, inst->codec, cap_id);
			return NULL;
		}
	case LEVEL:
		if (inst->codec == MSM_VIDC_AV1) {
			return av1_level;
		} else {
			i_vpr_e(inst, "%s: invalid codec type %d for cap id %d\n",
				__func__, inst->codec, cap_id);
			return NULL;
		}
	case AV1_TIER:
		return av1_tier;
	case IR_TYPE:
		return mpeg_video_vidc_ir_type;
	default:
		i_vpr_e(inst, "%s: No available qmenu for cap id %d\n",
			__func__, cap_id);
		return NULL;
	}
}

static inline bool has_parents(struct msm_vidc_inst_cap *cap)
{
	return !!cap->parents[0];
}

static inline bool has_childrens(struct msm_vidc_inst_cap *cap)
{
	return !!cap->children[0];
}

static inline bool is_root(struct msm_vidc_inst_cap *cap)
{
	return !has_parents(cap);
}

bool is_valid_cap_id(enum msm_vidc_inst_capability_type cap_id)
{
	return cap_id > INST_CAP_NONE && cap_id < INST_CAP_MAX;
}

static inline bool is_valid_cap(struct msm_vidc_inst_cap *cap)
{
	return is_valid_cap_id(cap->cap_id);
}

static inline bool is_all_parents_visited(
	struct msm_vidc_inst_cap *cap, bool lookup[INST_CAP_MAX]) {
	bool found = true;
	int i;

	for (i = 0; i < MAX_CAP_PARENTS; i++) {
		if (cap->parents[i] == INST_CAP_NONE)
			continue;

		if (!lookup[cap->parents[i]]) {
			found = false;
			break;
		}
	}
	return found;
}

static int add_node_list(struct list_head *list, enum msm_vidc_inst_capability_type cap_id)
{
	int rc = 0;
	struct msm_vidc_inst_cap_entry *entry = NULL;

	rc = msm_vidc_vmem_alloc(sizeof(struct msm_vidc_inst_cap_entry),
			(void **)&entry, __func__);
	if (rc)
		return rc;

	INIT_LIST_HEAD(&entry->list);
	entry->cap_id = cap_id;
	list_add_tail(&entry->list, list);

	return rc;
}

static int add_node(
	struct list_head *list, struct msm_vidc_inst_cap *rcap, bool lookup[INST_CAP_MAX])
{
	int rc = 0;

	if (lookup[rcap->cap_id])
		return 0;

	rc = add_node_list(list, rcap->cap_id);
	if (rc)
		return rc;

	lookup[rcap->cap_id] = true;
	return 0;
}

static int swap_node(struct msm_vidc_inst_cap *rcap,
	struct list_head *src_list, bool src_lookup[INST_CAP_MAX],
	struct list_head *dest_list, bool dest_lookup[INST_CAP_MAX])
{
	struct msm_vidc_inst_cap_entry *entry, *temp;
	bool found = false;

	/* cap must be available in src and not present in dest */
	if (!src_lookup[rcap->cap_id] || dest_lookup[rcap->cap_id]) {
		d_vpr_e("%s: not found in src or already found in dest for cap %s\n",
			__func__, cap_name(rcap->cap_id));
		return -EINVAL;
	}

	/* check if entry present in src_list */
	list_for_each_entry_safe(entry, temp, src_list, list) {
		if (entry->cap_id == rcap->cap_id) {
			found = true;
			break;
		}
	}

	if (!found) {
		d_vpr_e("%s: cap %s not found in src list\n",
			__func__, cap_name(rcap->cap_id));
		return -EINVAL;
	}

	/* remove from src_list */
	list_del_init(&entry->list);
	src_lookup[rcap->cap_id] = false;

	/* add it to dest_list */
	list_add_tail(&entry->list, dest_list);
	dest_lookup[rcap->cap_id] = true;

	return 0;
}

static int msm_vidc_add_capid_to_fw_list(struct msm_vidc_inst *inst,
	enum msm_vidc_inst_capability_type cap_id)
{
	struct msm_vidc_inst_cap_entry *entry = NULL;
	int rc = 0;

	/* skip adding if cap_id already present in firmware list */
	list_for_each_entry(entry, &inst->firmware_list, list) {
		if (entry->cap_id == cap_id) {
			i_vpr_l(inst,
				"%s: cap[%d] %s already present in fw list\n",
				__func__, cap_id, cap_name(cap_id));
			return 0;
		}
	}

	rc = add_node_list(&inst->firmware_list, cap_id);
	if (rc)
		return rc;

	return 0;
}

static int msm_vidc_add_children(struct msm_vidc_inst *inst,
	enum msm_vidc_inst_capability_type cap_id)
{
	struct msm_vidc_inst_cap *cap;
	int i, rc = 0;

	cap = &inst->capabilities->cap[cap_id];

	for (i = 0; i < MAX_CAP_CHILDREN; i++) {
		if (!cap->children[i])
			break;

		if (!is_valid_cap_id(cap->children[i]))
			continue;

		rc = add_node_list(&inst->children_list, cap->children[i]);
		if (rc)
			return rc;
	}

	return rc;
}

static int msm_vidc_adjust_cap(struct msm_vidc_inst *inst,
	enum msm_vidc_inst_capability_type cap_id,
	struct v4l2_ctrl *ctrl, const char *func)
{
	struct msm_vidc_inst_cap *cap;
	int rc = 0;

	/* validate cap_id */
	if (!is_valid_cap_id(cap_id))
		return 0;

	/* validate cap */
	cap = &inst->capabilities->cap[cap_id];
	if (!is_valid_cap(cap))
		return 0;

	/* check if adjust supported */
	if (!cap->adjust) {
		if (ctrl)
			msm_vidc_update_cap_value(inst, cap_id, ctrl->val, func);
		return 0;
	}

	/* call adjust */
	rc = cap->adjust(inst, ctrl);
	if (rc) {
		i_vpr_e(inst, "%s: adjust cap failed for %s\n", func, cap_name(cap_id));
		return rc;
	}

	return rc;
}

static int msm_vidc_set_cap(struct msm_vidc_inst *inst,
	enum msm_vidc_inst_capability_type cap_id,
	const char *func)
{
	struct msm_vidc_inst_cap *cap;
	int rc = 0;

	/* validate cap_id */
	if (!is_valid_cap_id(cap_id))
		return 0;

	/* validate cap */
	cap = &inst->capabilities->cap[cap_id];
	if (!is_valid_cap(cap))
		return 0;

	/* check if set supported */
	if (!cap->set)
		return 0;

	/* call set */
	rc = cap->set(inst, cap_id);
	if (rc) {
		i_vpr_e(inst, "%s: set cap failed for %s\n", func, cap_name(cap_id));
		return rc;
	}

	return rc;
}

static int msm_vidc_adjust_dynamic_property(struct msm_vidc_inst *inst,
	enum msm_vidc_inst_capability_type cap_id, struct v4l2_ctrl *ctrl)
{
	struct msm_vidc_inst_cap_entry *entry = NULL, *temp = NULL;
	struct msm_vidc_inst_capability *capability;
	s32 prev_value;
	int rc = 0;

	if (!inst || !inst->capabilities || !ctrl) {
		d_vpr_e("%s: invalid param\n", __func__);
		return -EINVAL;
	}
	capability = inst->capabilities;

	/* sanitize cap_id */
	if (!is_valid_cap_id(cap_id)) {
		i_vpr_e(inst, "%s: invalid cap_id %u\n", __func__, cap_id);
		return -EINVAL;
	}

	if (!(capability->cap[cap_id].flags & CAP_FLAG_DYNAMIC_ALLOWED)) {
		i_vpr_h(inst,
			"%s: dynamic setting of cap[%d] %s is not allowed\n",
			__func__, cap_id, cap_name(cap_id));
		return -EBUSY;
	}
	i_vpr_h(inst, "%s: cap[%d] %s\n", __func__, cap_id, cap_name(cap_id));

	prev_value = capability->cap[cap_id].value;
	rc = msm_vidc_adjust_cap(inst, cap_id, ctrl, __func__);
	if (rc)
		return rc;

	if (capability->cap[cap_id].value == prev_value && cap_id == GOP_SIZE) {
		/*
		 * Ignore setting same GOP size value to firmware to avoid
		 * unnecessary generation of IDR frame.
		 */
		return 0;
	}

	/* add cap_id to firmware list always */
	rc = msm_vidc_add_capid_to_fw_list(inst, cap_id);
	if (rc)
		goto error;

	/* add children only if cap value modified */
	if (capability->cap[cap_id].value == prev_value)
		return 0;

	rc = msm_vidc_add_children(inst, cap_id);
	if (rc)
		goto error;

	list_for_each_entry_safe(entry, temp, &inst->children_list, list) {
		if (!is_valid_cap_id(entry->cap_id)) {
			rc = -EINVAL;
			goto error;
		}

		if (!capability->cap[entry->cap_id].adjust) {
			i_vpr_e(inst, "%s: child cap must have ajdust function %s\n",
				__func__, cap_name(entry->cap_id));
			rc = -EINVAL;
			goto error;
		}

		prev_value = capability->cap[entry->cap_id].value;
		rc = msm_vidc_adjust_cap(inst, entry->cap_id, NULL, __func__);
		if (rc)
			goto error;

		/* add children if cap value modified */
		if (capability->cap[entry->cap_id].value != prev_value) {
			/* add cap_id to firmware list always */
			rc = msm_vidc_add_capid_to_fw_list(inst, entry->cap_id);
			if (rc)
				goto error;

			rc = msm_vidc_add_children(inst, entry->cap_id);
			if (rc)
				goto error;
		}

		list_del_init(&entry->list);
		msm_vidc_vmem_free((void **)&entry);
	}

	/* expecting children_list to be empty */
	if (!list_empty(&inst->children_list)) {
		i_vpr_e(inst, "%s: child_list is not empty\n", __func__);
		rc = -EINVAL;
		goto error;
	}

	return 0;
error:
	list_for_each_entry_safe(entry, temp, &inst->children_list, list) {
		i_vpr_e(inst, "%s: child list: %s\n", __func__, cap_name(entry->cap_id));
		list_del_init(&entry->list);
		msm_vidc_vmem_free((void **)&entry);
	}
	list_for_each_entry_safe(entry, temp, &inst->firmware_list, list) {
		i_vpr_e(inst, "%s: fw list: %s\n", __func__, cap_name(entry->cap_id));
		list_del_init(&entry->list);
		msm_vidc_vmem_free((void **)&entry);
	}

	return rc;
}

static int msm_vidc_set_dynamic_property(struct msm_vidc_inst *inst)
{
	struct msm_vidc_inst_cap_entry *entry = NULL, *temp = NULL;
	int rc = 0;

	if (!inst) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	i_vpr_h(inst, "%s()\n", __func__);

	list_for_each_entry_safe(entry, temp, &inst->firmware_list, list) {
		rc = msm_vidc_set_cap(inst, entry->cap_id, __func__);
		if (rc)
			goto error;

		list_del_init(&entry->list);
		msm_vidc_vmem_free((void **)&entry);
	}

	return 0;
error:
	list_for_each_entry_safe(entry, temp, &inst->firmware_list, list) {
		i_vpr_e(inst, "%s: fw list: %s\n", __func__, cap_name(entry->cap_id));
		list_del_init(&entry->list);
		msm_vidc_vmem_free((void **)&entry);
	}

	return rc;
}

int msm_vidc_ctrl_deinit(struct msm_vidc_inst *inst)
{
	if (!inst) {
		d_vpr_e("%s: invalid parameters\n", __func__);
		return -EINVAL;
	}
	i_vpr_h(inst, "%s(): num ctrls %d\n", __func__, inst->num_ctrls);
	v4l2_ctrl_handler_free(&inst->ctrl_handler);
	memset(&inst->ctrl_handler, 0, sizeof(struct v4l2_ctrl_handler));
	msm_vidc_vmem_free((void **)&inst->ctrls);
	inst->ctrls = NULL;

	return 0;
}

int msm_vidc_ctrl_init(struct msm_vidc_inst *inst)
{
	int rc = 0;
	struct msm_vidc_inst_capability *capability;
	struct msm_vidc_core *core;
	int idx = 0;
	struct v4l2_ctrl_config ctrl_cfg = {0};
	int num_ctrls = 0, ctrl_idx = 0;

	if (!inst || !inst->core || !inst->capabilities) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	core = inst->core;
	capability = inst->capabilities;

	if (!core->v4l2_ctrl_ops) {
		i_vpr_e(inst, "%s: no control ops\n", __func__);
		return -EINVAL;
	}

	for (idx = 0; idx < INST_CAP_MAX; idx++) {
		if (capability->cap[idx].v4l2_id)
			num_ctrls++;
	}
	if (!num_ctrls) {
		i_vpr_e(inst, "%s: no ctrls available in cap database\n",
			__func__);
		return -EINVAL;
	}
	rc = msm_vidc_vmem_alloc(num_ctrls * sizeof(struct v4l2_ctrl *),
			(void **)&inst->ctrls, __func__);
	if (rc)
		return rc;

	rc = v4l2_ctrl_handler_init(&inst->ctrl_handler, num_ctrls);
	if (rc) {
		i_vpr_e(inst, "control handler init failed, %d\n",
				inst->ctrl_handler.error);
		goto error;
	}

	for (idx = 0; idx < INST_CAP_MAX; idx++) {
		struct v4l2_ctrl *ctrl;

		if (!capability->cap[idx].v4l2_id)
			continue;

		if (ctrl_idx >= num_ctrls) {
			i_vpr_e(inst,
				"%s: invalid ctrl %#x, max allowed %d\n",
				__func__, capability->cap[idx].v4l2_id,
				num_ctrls);
			rc = -EINVAL;
			goto error;
		}
		i_vpr_l(inst,
			"%s: cap[%d] %24s, value %d min %d max %d step_or_mask %#x flags %#x v4l2_id %#x hfi_id %#x\n",
			__func__, idx, cap_name(idx),
			capability->cap[idx].value,
			capability->cap[idx].min,
			capability->cap[idx].max,
			capability->cap[idx].step_or_mask,
			capability->cap[idx].flags,
			capability->cap[idx].v4l2_id,
			capability->cap[idx].hfi_id);

		memset(&ctrl_cfg, 0, sizeof(struct v4l2_ctrl_config));

		if (is_priv_ctrl(capability->cap[idx].v4l2_id)) {
			/* add private control */
			ctrl_cfg.def = capability->cap[idx].value;
			ctrl_cfg.flags = 0;
			ctrl_cfg.id = capability->cap[idx].v4l2_id;
			ctrl_cfg.max = capability->cap[idx].max;
			ctrl_cfg.min = capability->cap[idx].min;
			ctrl_cfg.ops = core->v4l2_ctrl_ops;
			if (capability->cap[idx].flags & CAP_FLAG_MENU)
				ctrl_cfg.type = V4L2_CTRL_TYPE_MENU;
			else if (capability->cap[idx].flags & CAP_FLAG_BITMASK)
				ctrl_cfg.type = V4L2_CTRL_TYPE_BITMASK;
			else
				ctrl_cfg.type = V4L2_CTRL_TYPE_INTEGER;
			if (is_meta_cap(inst, idx)) {
				/* bitmask is expected to be enabled for meta controls */
				if (ctrl_cfg.type != V4L2_CTRL_TYPE_BITMASK) {
					i_vpr_e(inst,
						"%s: missing bitmask for cap %s\n",
						__func__, cap_name(idx));
					rc = -EINVAL;
					goto error;
				}
			}
			if (ctrl_cfg.type == V4L2_CTRL_TYPE_MENU) {
				ctrl_cfg.menu_skip_mask =
					~(capability->cap[idx].step_or_mask);
				ctrl_cfg.qmenu = msm_vidc_get_qmenu_type(inst,
					capability->cap[idx].cap_id);
			} else {
				ctrl_cfg.step =
					capability->cap[idx].step_or_mask;
			}
			ctrl_cfg.name = cap_name(capability->cap[idx].cap_id);
			if (!ctrl_cfg.name) {
				i_vpr_e(inst, "%s: %#x ctrl name is null\n",
					__func__, ctrl_cfg.id);
				rc = -EINVAL;
				goto error;
			}
			ctrl = v4l2_ctrl_new_custom(&inst->ctrl_handler,
					&ctrl_cfg, NULL);
		} else {
			if (capability->cap[idx].flags & CAP_FLAG_MENU) {
				ctrl = v4l2_ctrl_new_std_menu(
					&inst->ctrl_handler,
					core->v4l2_ctrl_ops,
					capability->cap[idx].v4l2_id,
					capability->cap[idx].max,
					~(capability->cap[idx].step_or_mask),
					capability->cap[idx].value);
			} else {
				ctrl = v4l2_ctrl_new_std(&inst->ctrl_handler,
					core->v4l2_ctrl_ops,
					capability->cap[idx].v4l2_id,
					capability->cap[idx].min,
					capability->cap[idx].max,
					capability->cap[idx].step_or_mask,
					capability->cap[idx].value);
			}
		}
		if (!ctrl) {
			i_vpr_e(inst, "%s: invalid ctrl %#x cap %24s\n", __func__,
				capability->cap[idx].v4l2_id, cap_name(idx));
			rc = -EINVAL;
			goto error;
		}

		rc = inst->ctrl_handler.error;
		if (rc) {
			i_vpr_e(inst,
				"error adding ctrl (%#x) to ctrl handle, %d\n",
				capability->cap[idx].v4l2_id,
				inst->ctrl_handler.error);
			goto error;
		}

		if (capability->cap[idx].flags & CAP_FLAG_VOLATILE)
			ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;

		ctrl->flags |= V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;
		inst->ctrls[ctrl_idx] = ctrl;
		ctrl_idx++;
	}
	inst->num_ctrls = num_ctrls;
	i_vpr_h(inst, "%s(): num ctrls %d\n", __func__, inst->num_ctrls);

	return 0;
error:
	msm_vidc_ctrl_deinit(inst);

	return rc;
}

static int msm_vidc_update_buffer_count_if_needed(struct msm_vidc_inst* inst,
	enum msm_vidc_inst_capability_type cap_id)
{
	int rc = 0;
	bool update_input_port = false, update_output_port = false;

	if (!inst) {
		d_vpr_e("%s: invalid parameters\n", __func__);
		return -EINVAL;
	}

	switch (cap_id) {
	case LAYER_TYPE:
	case ENH_LAYER_COUNT:
	case LAYER_ENABLE:
		update_input_port = true;
		break;
	case THUMBNAIL_MODE:
	case PRIORITY:
		update_input_port = true;
		update_output_port = true;
		break;
	default:
		update_input_port = false;
		update_output_port = false;
		break;
	}

	if (update_input_port) {
		rc = msm_vidc_update_buffer_count(inst, INPUT_PORT);
		if (rc)
			return rc;
	}
	if (update_output_port) {
		rc = msm_vidc_update_buffer_count(inst, OUTPUT_PORT);
		if (rc)
			return rc;
	}

	return rc;
}

static int msm_vidc_allow_secure_session(struct msm_vidc_inst *inst)
{
	int rc = 0;
	struct msm_vidc_inst *i;
	struct msm_vidc_core *core;
	u32 count = 0;

	if (!inst || !inst->core) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	core = inst->core;

	if (!core->capabilities) {
		i_vpr_e(inst, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	core_lock(core, __func__);
	list_for_each_entry(i, &core->instances, list) {
		if (i->capabilities) {
			if (i->capabilities->cap[SECURE_MODE].value)
				count++;
		}
	}

	if (count > core->capabilities[MAX_SECURE_SESSION_COUNT].value) {
		i_vpr_e(inst,
			"%s: total secure sessions %d exceeded max limit %d\n",
			__func__, count,
			core->capabilities[MAX_SECURE_SESSION_COUNT].value);
		rc = -EINVAL;
	}
	core_unlock(core, __func__);

	return rc;
}

int msm_v4l2_op_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	int rc = 0;
	struct msm_vidc_inst *inst;

	if (!ctrl) {
		d_vpr_e("%s: invalid ctrl parameter\n", __func__);
		return -EINVAL;
	}

	inst = container_of(ctrl->handler,
			    struct msm_vidc_inst, ctrl_handler);
	inst = get_inst_ref(g_core, inst);
	if (!inst) {
		d_vpr_e("%s: could not find inst for ctrl %s id %#x\n",
			__func__, ctrl->name, ctrl->id);
		return -EINVAL;
	}
	client_lock(inst, __func__);
	inst_lock(inst, __func__);

	rc = msm_vidc_get_control(inst, ctrl);
	if (rc) {
		i_vpr_e(inst, "%s: failed for ctrl %s id %#x\n",
			__func__, ctrl->name, ctrl->id);
		goto unlock;
	} else {
		i_vpr_h(inst, "%s: ctrl %s id %#x, value %d\n",
			__func__, ctrl->name, ctrl->id, ctrl->val);
	}

unlock:
	inst_unlock(inst, __func__);
	client_unlock(inst, __func__);
	put_inst(inst);
	return rc;
}

static int msm_vidc_update_static_property(struct msm_vidc_inst *inst,
	enum msm_vidc_inst_capability_type cap_id, struct v4l2_ctrl *ctrl)
{
	int rc = 0;

	if (!inst || !ctrl) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	/* update value to db */
	msm_vidc_update_cap_value(inst, cap_id, ctrl->val, __func__);

	if (cap_id == CLIENT_ID) {
		rc = msm_vidc_update_debug_str(inst);
		if (rc)
			return rc;
	}

	if (cap_id == SECURE_MODE) {
		if (ctrl->val) {
			rc = msm_vidc_allow_secure_session(inst);
			if (rc)
				return rc;
		}
	}

	if (cap_id == ROTATION) {
		struct v4l2_format *output_fmt;

		output_fmt = &inst->fmts[OUTPUT_PORT];
		rc = msm_venc_s_fmt_output(inst, output_fmt);
		if (rc)
			return rc;
	}

	if (cap_id == DELIVERY_MODE) {
		struct v4l2_format *output_fmt;

		output_fmt = &inst->fmts[OUTPUT_PORT];
		rc = msm_venc_s_fmt_output(inst, output_fmt);
		if (rc)
			return rc;
	}

	if (cap_id == BITSTREAM_SIZE_OVERWRITE) {
		rc = msm_vidc_update_bitstream_buffer_size(inst);
		if (rc)
			return rc;
	}

	/* call this explicitly to adjust client priority */
	if (cap_id == PRIORITY) {
		rc = msm_vidc_adjust_session_priority(inst, ctrl);
		if (rc)
			return rc;
	}

	if (cap_id == CRITICAL_PRIORITY)
		msm_vidc_update_cap_value(inst, PRIORITY, 0, __func__);

	if (cap_id == ENH_LAYER_COUNT && inst->codec == MSM_VIDC_HEVC) {
		u32 enable;

		/* enable LAYER_ENABLE cap if HEVC_HIER enh layers > 0 */
		if (ctrl->val > 0)
			enable = 1;
		else
			enable = 0;

		msm_vidc_update_cap_value(inst, LAYER_ENABLE, enable, __func__);
	}
	if (is_meta_cap(inst, cap_id)) {
		rc = msm_vidc_update_meta_port_settings(inst);
		if (rc)
			return rc;
	}

	rc = msm_vidc_update_buffer_count_if_needed(inst, cap_id);
	if (rc)
		return rc;

	return rc;
}

int msm_v4l2_op_s_ctrl(struct v4l2_ctrl *ctrl)
{
	int rc = 0;
	struct msm_vidc_inst *inst;
	enum msm_vidc_inst_capability_type cap_id;
	struct msm_vidc_inst_capability *capability;
	u32 port;

	if (!ctrl) {
		d_vpr_e("%s: invalid ctrl parameter\n", __func__);
		return -EINVAL;
	}

	inst = container_of(ctrl->handler,
		struct msm_vidc_inst, ctrl_handler);
	inst = get_inst_ref(g_core, inst);
	if (!inst || !inst->capabilities) {
		d_vpr_e("%s: invalid parameters for inst\n", __func__);
		return -EINVAL;
	}

	client_lock(inst, __func__);
	inst_lock(inst, __func__);
	capability = inst->capabilities;

	i_vpr_h(inst, FMT_STRING_SET_CTRL,
		__func__, state_name(inst->state), ctrl->name, ctrl->id, ctrl->val);

	cap_id = msm_vidc_get_cap_id(inst, ctrl->id);
	if (!is_valid_cap_id(cap_id)) {
		i_vpr_e(inst, "%s: could not find cap_id for ctrl %s\n",
			__func__, ctrl->name);
		rc = -EINVAL;
		goto unlock;
	}

	if (!msm_vidc_allow_s_ctrl(inst, cap_id)) {
		rc = -EINVAL;
		goto unlock;
	}

	/* mark client set flag */
	capability->cap[cap_id].flags |= CAP_FLAG_CLIENT_SET;

	port = is_encode_session(inst) ? OUTPUT_PORT : INPUT_PORT;
	if (!inst->bufq[port].vb2q->streaming) {
		/* static case */
		rc = msm_vidc_update_static_property(inst, cap_id, ctrl);
		if (rc)
			goto unlock;
	} else {
		/* dynamic case */
		rc = msm_vidc_adjust_dynamic_property(inst, cap_id, ctrl);
		if (rc)
			goto unlock;

		rc = msm_vidc_set_dynamic_property(inst);
		if (rc)
			goto unlock;
	}

unlock:
	inst_unlock(inst, __func__);
	client_unlock(inst, __func__);
	put_inst(inst);
	return rc;
}

int msm_vidc_prepare_dependency_list(struct msm_vidc_inst *inst)
{
	struct list_head root_list, opt_list;
	struct msm_vidc_inst_capability *capability;
	struct msm_vidc_inst_cap *cap, *rcap;
	struct msm_vidc_inst_cap_entry *entry = NULL, *temp = NULL;
	bool root_visited[INST_CAP_MAX];
	bool opt_visited[INST_CAP_MAX];
	int tmp_count_total, tmp_count, num_nodes = 0;
	int i, rc = 0;

	if (!inst || !inst->capabilities) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	capability = inst->capabilities;

	if (!list_empty(&inst->caps_list)) {
		i_vpr_h(inst, "%s: dependency list already prepared\n", __func__);
		return 0;
	}

	/* init local list and lookup table entries */
	INIT_LIST_HEAD(&root_list);
	INIT_LIST_HEAD(&opt_list);
	memset(&root_visited, 0, sizeof(root_visited));
	memset(&opt_visited, 0, sizeof(opt_visited));

	/* populate root nodes first */
	for (i = 1; i < INST_CAP_MAX; i++) {
		rcap = &capability->cap[i];
		if (!is_valid_cap(rcap))
			continue;

		/* sanitize cap value */
		if (i != rcap->cap_id) {
			i_vpr_e(inst, "%s: cap id mismatch. expected %s, actual %s\n",
				__func__, cap_name(i), cap_name(rcap->cap_id));
			rc = -EINVAL;
			goto error;
		}

		/* add all root nodes */
		if (is_root(rcap)) {
			rc = add_node(&root_list, rcap, root_visited);
			if (rc)
				goto error;
		} else {
			rc = add_node(&opt_list, rcap, opt_visited);
			if (rc)
				goto error;
		}
	}

	/* add all dependent parents */
	list_for_each_entry_safe(entry, temp, &root_list, list) {
		rcap = &capability->cap[entry->cap_id];
		/* skip leaf node */
		if (!has_childrens(rcap))
			continue;

		for (i = 0; i < MAX_CAP_CHILDREN; i++) {
			if (!rcap->children[i])
				break;

			if (!is_valid_cap_id(rcap->children[i]))
				continue;

			cap = &capability->cap[rcap->children[i]];
			if (!is_valid_cap(cap))
				continue;

			/**
			 * if child node is already part of root list
			 * then no need to add it again.
			 */
			if (root_visited[cap->cap_id])
				continue;

			/**
			 * if child node's all parents are already present in root list
			 * then add it to root list else remains in optional list.
			 */
			if (is_all_parents_visited(cap, root_visited)) {
				rc = swap_node(cap,
						&opt_list, opt_visited, &root_list, root_visited);
				if (rc)
					goto error;
			}
		}
	}

	/* find total optional list entries */
	list_for_each_entry(entry, &opt_list, list)
		num_nodes++;

	/* used for loop detection */
	tmp_count_total = num_nodes;
	tmp_count = num_nodes;

	/* sort final outstanding nodes */
	list_for_each_entry_safe(entry, temp, &opt_list, list) {
		/* initially remove entry from opt list */
		list_del_init(&entry->list);
		opt_visited[entry->cap_id] = false;
		tmp_count--;
		cap = &capability->cap[entry->cap_id];

		/**
		 * if all parents are visited then add this entry to
		 * root list else add it to the end of optional list.
		 */
		if (is_all_parents_visited(cap, root_visited)) {
			list_add_tail(&entry->list, &root_list);
			root_visited[entry->cap_id] = true;
			tmp_count_total--;
		} else {
			list_add_tail(&entry->list, &opt_list);
			opt_visited[entry->cap_id] = true;
		}

		/* detect loop */
		if (!tmp_count) {
			if (num_nodes == tmp_count_total) {
				i_vpr_e(inst, "%s: loop detected in subgraph %d\n",
					__func__, num_nodes);
				rc = -EINVAL;
				goto error;
			}
			num_nodes = tmp_count_total;
			tmp_count = tmp_count_total;
		}
	}

	/* expecting opt_list to be empty */
	if (!list_empty(&opt_list)) {
		i_vpr_e(inst, "%s: opt_list is not empty\n", __func__);
		rc = -EINVAL;
		goto error;
	}

	/* move elements to &inst->caps_list from local */
	list_replace_init(&root_list, &inst->caps_list);

	return 0;
error:
	list_for_each_entry_safe(entry, temp, &opt_list, list) {
		i_vpr_e(inst, "%s: opt_list: %s\n", __func__, cap_name(entry->cap_id));
		list_del_init(&entry->list);
		msm_vidc_vmem_free((void **)&entry);
	}
	list_for_each_entry_safe(entry, temp, &root_list, list) {
		i_vpr_e(inst, "%s: root_list: %s\n", __func__, cap_name(entry->cap_id));
		list_del_init(&entry->list);
		msm_vidc_vmem_free((void **)&entry);
	}
	return rc;
}

/*
 * Loop over instance capabilities from caps_list
 * and call adjust and set function
 */
int msm_vidc_adjust_set_v4l2_properties(struct msm_vidc_inst *inst)
{
	struct msm_vidc_inst_cap_entry *entry = NULL, *temp = NULL;
	int rc = 0;

	if (!inst || !inst->capabilities) {
		d_vpr_e("%s: invalid params\n", __func__);
		return -EINVAL;
	}
	i_vpr_h(inst, "%s()\n", __func__);

	/* adjust all possible caps from caps_list */
	list_for_each_entry_safe(entry, temp, &inst->caps_list, list) {
		i_vpr_l(inst, "%s: cap: id %3u, name %s\n", __func__,
			entry->cap_id, cap_name(entry->cap_id));

		rc = msm_vidc_adjust_cap(inst, entry->cap_id, NULL, __func__);
		if (rc)
			return rc;
	}

	/* set all caps from caps_list */
	list_for_each_entry_safe(entry, temp, &inst->caps_list, list) {
		rc = msm_vidc_set_cap(inst, entry->cap_id, __func__);
		if (rc)
			return rc;
	}

	return rc;
}
