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
#include <pthread.h>
#include <stdbool.h>

#define LOG_TAG "QCOM PowerHAL"
#include <log/log.h>
#include <hardware/hardware.h>
#include <hardware/power.h>

#include "utils.h"
#include "metadata-defs.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"

pthread_mutex_t video_encode_lock = PTHREAD_MUTEX_INITIALIZER;
uintptr_t video_encode_hint_counter = 0;
bool video_encode_hint_should_enable = false;
bool video_encode_hint_is_enabled = false;

static int new_hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;
static int cur_hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

static const time_t VIDEO_ENCODE_DELAY_SECONDS = 2;
static const time_t VIDEO_ENCODE_DELAY_NSECONDS = 0;

static void* video_encode_hint_function(void* arg) {
    struct timespec tv = {0};
    tv.tv_sec = VIDEO_ENCODE_DELAY_SECONDS;
    tv.tv_nsec = VIDEO_ENCODE_DELAY_NSECONDS;
    int nanosleep_ret = 0;
    uintptr_t expected_counter = (uintptr_t)arg;

    // delay the hint for two seconds
    // the hint hotplugs the large CPUs, so this prevents the large CPUs from
    // going offline until the camera has had time to startup
    TEMP_FAILURE_RETRY(nanosleep(&tv, &tv));
    pthread_mutex_lock(&video_encode_lock);

    // check to ensure we should still turn on hint from this particular thread
    // if should_enable is true but counter is different, another thread owns hint
    // if should_enable is false, we've already quit the camera
    if (video_encode_hint_should_enable == true && video_encode_hint_counter == expected_counter) {
        /* sched and cpufreq params
         * A57 - offlines
         * A53 - 4 cores online at 1.2GHz
         */
        int resource_values[] = {0x150C, 0x160C, 0x170C, 0x180C, 0x3DFF};

        perform_hint_action(new_hint_id,
                            resource_values, sizeof(resource_values)/sizeof(resource_values[0]));
        cur_hint_id = new_hint_id;
        video_encode_hint_is_enabled = true;
        video_encode_hint_should_enable = false;
    }

    pthread_mutex_unlock(&video_encode_lock);
    return NULL;
}

static void process_video_encode_hint(void *metadata)
{
    char governor[80];
    struct video_encode_metadata_t video_encode_metadata;

    if (get_scaling_governor(governor, sizeof(governor)) == -1) {
        ALOGE("Can't obtain scaling governor.");
        return;
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
	     pthread_t video_encode_hint_thread;
             pthread_mutex_lock(&video_encode_lock);
             new_hint_id = video_encode_metadata.hint_id;
             if (video_encode_hint_counter < 65535) {
            	                video_encode_hint_counter++;
            } else {
	        video_encode_hint_counter = 0;
	            }
            // start new thread to launch hint
            video_encode_hint_should_enable = true;
            if (pthread_create(&video_encode_hint_thread, NULL, video_encode_hint_function, (void*)video_encode_hint_counter) != 0) {
                ALOGE("Error constructing hint thread");
                video_encode_hint_should_enable = false;
                pthread_mutex_unlock(&video_encode_lock);
                return HINT_NONE;
            }
            pthread_detach(video_encode_hint_thread);
            pthread_mutex_unlock(&video_encode_lock);
    } else if (video_encode_metadata.state == 0) {
        if (is_interactive_governor(governor)) {
            pthread_mutex_lock(&video_encode_lock);
            video_encode_hint_should_enable = false;
            if (video_encode_hint_is_enabled == true) {
                undo_hint_action(cur_hint_id);
                video_encode_hint_is_enabled = false;
            }
            pthread_mutex_unlock(&video_encode_lock);
        }
    }
}

static int resources_interaction_fling_boost[] = {
    ALL_CPUS_PWR_CLPS_DIS,
    SCHED_BOOST_ON,
    SCHED_PREFER_IDLE_DIS
};

static int resources_interaction_boost[] = {
    ALL_CPUS_PWR_CLPS_DIS,
    SCHED_PREFER_IDLE_DIS
};

static int resources_launch[] = {
    SCHED_BOOST_ON,
    0x20C
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

    if (get_scaling_governor(governor, sizeof(governor)) == -1) {
        ALOGE("Can't obtain scaling governor.");
        return HINT_NONE;
    }

    if (!on) {
        /* Display off */
        if (is_interactive_governor(governor)) {
            int resource_values[] = {0x41004000, 0x0}; /* 4+0 core config in display off */
            };
            perform_hint_action(DISPLAY_STATE_HINT_ID,
                    resource_values, ARRAY_SIZE(resource_values));
        }
    } else {
        /* Display on */
        if (is_interactive_governor(governor)) {
            undo_hint_action(DISPLAY_STATE_HINT_ID);
        }
    }
    return HINT_HANDLED;
}
