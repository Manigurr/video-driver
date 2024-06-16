/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _MSM_VIDC_IRIS33_AU_H_
#define _MSM_VIDC_IRIS33_AU_H_

#include "msm_vidc_core.h"

#if defined(CONFIG_MSM_VIDC_IRIS33_AU)
int msm_vidc_init_iris33_au(struct msm_vidc_core *core);
int msm_vidc_deinit_iris33_au(struct msm_vidc_core *core);
int msm_vidc_adjust_bitrate_boost_iris33_au(void *instance, struct v4l2_ctrl *ctrl);
#else
static inline int msm_vidc_init_iris33_au(struct msm_vidc_core *core)
{
	return -EINVAL;
}
static inline int msm_vidc_deinit_iris33_au(struct msm_vidc_core *core)
{
	return -EINVAL;
}
static inline int msm_vidc_adjust_bitrate_boost_iris33_au(void *instance, struct v4l2_ctrl *ctrl)
{
	return -EINVAL;
}
#endif

#endif // _MSM_VIDC_IRIS33_AU_H_
