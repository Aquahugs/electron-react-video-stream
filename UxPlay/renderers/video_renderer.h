/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-24 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

/*
 * H264 / H265 rendering with GStreamer (+ optional HLS).
 * This header is aligned with the updated video_renderer.c, which
 * includes tee + appsink + WebSocket client functionality.
 */

#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "../lib/logger.h"

/* videoflip_e: controls orientation transforms via GStreamer videoflip. */
typedef enum videoflip_e {
    NONE,
    LEFT,
    RIGHT,
    INVERT,
    VFLIP,
    HFLIP
} videoflip_t;

/* The video_renderer_s struct is defined in video_renderer.c. */
typedef struct video_renderer_s video_renderer_t;

/**
 * video_renderer_init:
 *   Initializes the video rendering pipeline(s):
 *     - HLS or mirror mode (h264 / h265).
 *     - Optionally sets up GStreamer tee + appsink for extracting frames.
 *     - Optionally sets up a WebSocket client to forward frames.
 */
void video_renderer_init(logger_t *logger,
                         const char *server_name,
                         videoflip_t videoflip[2],
                         const char *parser,
                         const char *decoder,
                         const char *converter,
                         const char *videosink,
                         const char *videosink_options,
                         bool initial_fullscreen,
                         bool video_sync,
                         bool h265_support,
                         const char *uri);

/**
 * Start, stop, pause, resume the video renderer(s).
 */
void video_renderer_start();
void video_renderer_stop();
void video_renderer_pause();
void video_renderer_resume();

/**
 * Seek a position in the stream (primarily for HLS).
 */
void video_renderer_seek(float position);

/**
 * Flush the pipeline if needed (optional).
 */
void video_renderer_flush();

/**
 * Renders an incoming compressed frame buffer (from AirPlay) into the pipeline.
 * Typically called by raop_rtp_mirror or similar.
 */
void video_renderer_render_buffer(unsigned char *data,
                                  int *data_len,
                                  int *nal_count,
                                  uint64_t *ntp_time);

/**
 * Query and set display size, used in mirror mode.
 */
void video_renderer_size(float *width_source,
                         float *height_source,
                         float *width,
                         float *height);

/**
 * For local info: if we want to check if the renderer is paused (not implemented in the sample).
 * Keep as a placeholder if needed.
 */
bool video_renderer_is_paused(); /* optional or unimplemented in the new sample code */

/**
 * Listen for GStreamer bus events on a given renderer ID (0 or 1).
 * Called by main loop to handle EOS, errors, etc.
 */
unsigned int video_renderer_listen(void *loop, int id);

/**
 * Destroy all renderers (free resources, set pipeline state to NULL).
 */
void video_renderer_destroy();

/**
 * Some environment may require waiting for an X11 window (if HLS + X11).
 */
bool waiting_for_x11_window();

/**
 * Query playback info in HLS scenario.
 */
bool video_get_playback_info(double *duration, double *position, float *rate);

/**
 * Called to choose between h264/h265 pipelines once the correct codec is detected.
 */
void video_renderer_choose_codec(bool is_h265);

/**
 * Callback used to reset video if something triggers a re-init (internal).
 */
unsigned int video_reset_callback(void *loop);

#ifdef __cplusplus
}
#endif

#endif // VIDEO_RENDERER_H
