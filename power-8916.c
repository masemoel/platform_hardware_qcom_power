/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
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

#define MIN_FREQ_CPU0_DISP_OFF 400000
#define MIN_FREQ_CPU0_DISP_ON  960000

const char *scaling_min_freq[4] = {
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu1/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu3/cpufreq/scaling_min_freq"
};

/**
 * Returns true if the target is MSM8916.
 */
static bool is_target_8916(void)
{
    static int is_8916 = -1;
    int soc_id;

    if (is_8916 >= 0)
        return is_8916;

    soc_id = get_soc_id();
    is_8916 = soc_id == 206 || (soc_id >= 247 && soc_id <= 250);

    return is_8916;
}

static int resources_interaction_fling_boost[] = {
    ALL_CPUS_PWR_CLPS_DIS,
    SCHED_BOOST_ON,
    SCHED_PREFER_IDLE_DIS,
    0x20D
};

static int resources_interaction_boost[] = {
    ALL_CPUS_PWR_CLPS_DIS,
    SCHED_PREFER_IDLE_DIS,
    0x20D
};

static int resources_launch[] = {
    ALL_CPUS_PWR_CLPS_DIS,
    SCHED_BOOST_ON,
    SCHED_PREFER_IDLE_DIS,
    0x20F,
    0x1C00,
    0x4001,
    0x4101,
    0x4201
};

int power_hint_override(power_hint_t hint, void *data)
{
    static struct timespec s_previous_boost_timespec;
    struct timespec cur_boost_timespec;
    long long elapsed_time;
    static int s_previous_duration = 0;
    int duration;

    switch (hint) {
        case POWER_HINT_INTERACTION:
            duration = 500; // 500ms by default
            if (data) {
                int input_duration = *((int*)data);
                if (input_duration > duration) {
                    duration = (input_duration > 5000) ? 5000 : input_duration;
                }
            }

            clock_gettime(CLOCK_MONOTONIC, &cur_boost_timespec);

            elapsed_time = calc_timespan_us(s_previous_boost_timespec, cur_boost_timespec);
            // don't hint if previous hint's duration covers this hint's duration
            if ((s_previous_duration * 1000) > (elapsed_time + duration * 1000)) {
                return HINT_HANDLED;
            }
            s_previous_boost_timespec = cur_boost_timespec;
            s_previous_duration = duration;

            if (duration >= 1500) {
                interaction(duration, ARRAY_SIZE(resources_interaction_fling_boost),
                        resources_interaction_fling_boost);
            } else {
                interaction(duration, ARRAY_SIZE(resources_interaction_boost),
                        resources_interaction_boost);
            }
            return HINT_HANDLED;
        case POWER_HINT_LAUNCH:
            duration = 2000;
            interaction(duration, ARRAY_SIZE(resources_launch),
                    resources_launch);
            return HINT_HANDLED;
        case POWER_HINT_VIDEO_ENCODE: /* Do nothing for encode case */
            return HINT_HANDLED;
        case POWER_HINT_VIDEO_DECODE: /* Do nothing for decode case */
            return HINT_HANDLED;
        default:
            break;
    }
    return HINT_NONE;
}

int set_interactive_override(int on)
{
    char governor[80];
    char tmp_str[NODE_MAX];

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
        if (is_target_8916()) {
            if (is_interactive_governor(governor)) {
                int resource_values[] = {
                    TR_MS_50, THREAD_MIGRATION_SYNC_OFF
                };
                perform_hint_action(DISPLAY_STATE_HINT_ID,
                        resource_values, ARRAY_SIZE(resource_values));
            }
        } else {
            if (is_interactive_governor(governor)) {
                int resource_values[] = {
                    TR_MS_CPU0_50, TR_MS_CPU4_50, THREAD_MIGRATION_SYNC_OFF
                };

                /* Set CPU0 MIN FREQ to 400Mhz avoid extra peak power
                   impact in volume key press */
                snprintf(tmp_str, NODE_MAX, "%d", MIN_FREQ_CPU0_DISP_OFF);
                if (sysfs_write(scaling_min_freq[0], tmp_str) != 0) {
                    if (sysfs_write(scaling_min_freq[1], tmp_str) != 0) {
                        if (sysfs_write(scaling_min_freq[2], tmp_str) != 0) {
                            if (sysfs_write(scaling_min_freq[3], tmp_str) != 0) {
                                ALOGE("Failed to write to %s", SCALING_MIN_FREQ);
                            }
                        }
                    }
                }
                perform_hint_action(DISPLAY_STATE_HINT_ID,
                        resource_values, ARRAY_SIZE(resource_values));
            }
        }
    } else {
        /* Display on. */
        if (is_target_8916()) {
            if (is_interactive_governor(governor)) {
                undo_hint_action(DISPLAY_STATE_HINT_ID);
            }
        } else {
            if (is_interactive_governor(governor)) {
                /* Recovering MIN_FREQ in display ON case */
                snprintf(tmp_str, NODE_MAX, "%d", MIN_FREQ_CPU0_DISP_ON);
                if (sysfs_write(scaling_min_freq[0], tmp_str) != 0) {
                    if (sysfs_write(scaling_min_freq[1], tmp_str) != 0) {
                        if (sysfs_write(scaling_min_freq[2], tmp_str) != 0) {
                            if (sysfs_write(scaling_min_freq[3], tmp_str) != 0) {
                                ALOGE("Failed to write to %s", SCALING_MIN_FREQ);
                            }
                        }
                    }
                }
                undo_hint_action(DISPLAY_STATE_HINT_ID);
            }
        }
    }
    return HINT_HANDLED;
}
