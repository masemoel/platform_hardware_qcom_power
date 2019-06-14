/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 The LineageOS Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>

#define LOG_TAG "QCOM PowerHAL"
#include <log/log.h>
#include <hardware/hardware.h>
#include <hardware/power.h>

#include "utils.h"
#include "metadata-defs.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"

#define MIN_VAL(X,Y) ((X>Y)?(Y):(X))

static int video_encode_hint_sent;

#ifdef INTERACTION_BOOST
int get_number_of_profiles()
{
    return 0;
}
#endif

/**
 * Returns true if the target is SDM630.
 */
static bool is_target_SDM630(void)
{
    static int is_SDM630 = -1;
    int soc_id;

    if (is_SDM630 >= 0)
        return is_SDM630;

    soc_id = get_soc_id();
    is_SDM630 = soc_id == 318 || soc_id == 327;

    return is_SDM630;
}

static void process_video_encode_hint(void *metadata)
{
    char governor[80];
    int resource_values[20];
    int num_resources;
    struct video_encode_metadata_t video_encode_metadata;

    if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return;
                }
            }
        }
    }

    if (!metadata) {
        return;
    }

    /* Initialize encode metadata struct fields. */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (parse_video_encode_metadata((char *)metadata,
            &video_encode_metadata) == -1) {
        ALOGE("Error occurred while parsing metadata.");
        return;
    }

    if (video_encode_metadata.state == 1) {
        if (is_interactive_governor(governor)) {
            if (is_target_SDM630()) {
                /*
                   1. CPUfreq params
                      - hispeed freq for big - 1113Mhz
                      - go hispeed load for big - 95
                      - above_hispeed_delay for big - 40ms
                      - target loads - 95
                      - nr_run - 5
                   2. BusDCVS V2 params
                      - Sample_ms of 10ms
                 */
                int res[] = {
                    HISPEED_FREQ_BIG, 0x459,
                    GO_HISPEED_LOAD_BIG, 0x5F,
                    ABOVE_HISPEED_DELAY_BIG, 0x4,
                    TARGET_LOADS_BIG, 0x5F,
                    SCHED_SPILL_NR_RUN, 0X5,
                    CPUBW_HWMON_SAMPLE_MS, 0xA
                };
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = ARRAY_SIZE(res);
            } else {
                /*
                   1. CPUfreq params
                      - hispeed freq for little - 902Mhz
                      - go hispeed load for little - 95
                      - above_hispeed_delay for little - 40ms
                   2. BusDCVS V2 params
                      - Sample_ms of 10ms
                 */
                int res[] = {
                    HISPEED_FREQ_LITTLE, 0x386,
                    GO_HISPEED_LOAD_LITTLE, 0x5F,
                    ABOVE_HISPEED_DELAY_LITTLE, 0x4,
                    CPUBW_HWMON_SAMPLE_MS, 0xA
                };
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = ARRAY_SIZE(res);
            }
            if (!video_encode_hint_sent) {
                perform_hint_action(video_encode_metadata.hint_id,
                        resource_values, num_resources);
                video_encode_hint_sent = 1;
            }
        }
    } else if (video_encode_metadata.state == 0) {
        if (is_interactive_governor(governor)) {
            undo_hint_action(video_encode_metadata.hint_id);
            video_encode_hint_sent = 0;
        }
    }
}

int power_hint_override(power_hint_t hint, void *data)
{
    switch (hint) {
        case POWER_HINT_VSYNC:
            break;
        case POWER_HINT_VIDEO_ENCODE:
            process_video_encode_hint(data);
            return HINT_HANDLED;
        default:
            break;
    }
    return HINT_NONE;
}

int set_interactive_override(int on)
{
    char governor[80];
    int resource_values[20];
    int num_resources;

    if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return HINT_NONE;
                }
            }
        }
    }

    if (!on) {
        /* Display off. */
        if (is_interactive_governor(governor)) {
            if (is_target_SDM630()) {
                /*
                   1. CPUfreq params
                      - hispeed freq for big - 1113Mhz
                      - go hispeed load for big - 95
                      - above_hispeed_delay for big - 40ms
                   2. BusDCVS V2 params
                      - Sample_ms of 10ms
                 */
                int res[] = {
                    HISPEED_FREQ_BIG, 0x459,
                    GO_HISPEED_LOAD_BIG, 0x5F,
                    ABOVE_HISPEED_DELAY_BIG, 0x4,
                    CPUBW_HWMON_SAMPLE_MS, 0xA
                };
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = ARRAY_SIZE(res);
            } else {
                /*
                   1. CPUfreq params
                      - hispeed freq for little - 902Mhz
                      - go hispeed load for little - 95
                      - above_hispeed_delay for little - 40ms
                   2. BusDCVS V2 params
                      - Sample_ms of 10ms
                   3. Sched group upmigrate - 500
                 */
                int res[] =  {
                    HISPEED_FREQ_LITTLE, 0x386,
                    GO_HISPEED_LOAD_LITTLE, 0x5F,
                    ABOVE_HISPEED_DELAY_LITTLE, 0x4,
                    CPUBW_HWMON_SAMPLE_MS, 0xA,
                    SCHED_GROUP_UP_MIGRATE, 0x1F4
                };
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = ARRAY_SIZE(res);

            }
            perform_hint_action(DISPLAY_STATE_HINT_ID,
                    resource_values, num_resources);
        }
    } else {
        /* Display on. */
        if (is_interactive_governor(governor)) {
            undo_hint_action(DISPLAY_STATE_HINT_ID);
        }
    }
    return HINT_HANDLED;
}
