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

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include "video_renderer.h"
#include <gst/app/gstappsink.h>


#include <libwebsockets.h>
#include <pthread.h>    // For pthread_create, etc.
#include <string.h>     // For memset, strstr
#include <stdio.h>
#include <stdbool.h>

/*=========================*/
/*   WebSocket Globals     */
/*=========================*/

// Global-ish context for sending frames
static struct lws_context *ws_context;
static struct lws *ws_wsi;  // "websocket interface"
static bool connected = false;

/**
 * A simple background thread that runs the libwebsockets service loop,
 * so it can handle incoming/outgoing messages independently.
 */
static void *ws_service_thread(void *arg) {
    while (1) {
        lws_service(ws_context, 1000); 
        // 1000 ms poll or tweak as needed
    }
    return NULL;
}

/* WebSocket callback. Adjust if you want to handle inbound messages, etc. */
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            connected = true;
            lwsl_user("WS client connected!\n");
            printf("ws_callback: LWS_CALLBACK_CLIENT_ESTABLISHED => connected = true\n");
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            lwsl_user("WS connection error!\n");
            connected = false;
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            // Typically we don't need to handle inbound frames
            break;

        case LWS_CALLBACK_CLOSED:
            connected = false;
            lwsl_user("WS client closed!\n");
            break;

        default:
            break;
    }
    return 0;
}

/**
 * Initializes the libwebsockets client and connects to ws://localhost:8081
 */
static void init_websocket_client() {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = (const struct lws_protocols[]) {
        {
            "my-protocol",
            ws_callback,
            0,                      // per_session_data_size
            65536,                  // rx_buffer_size
            0,                      // id
            NULL,                   // user pointer
            65536                   // tx_packet_size
        },
        { NULL, NULL, 0, 0 }
    };
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.gid = -1;
    info.uid = -1;
    info.fd_limit_per_thread = 1024;
    
    ws_context = lws_create_context(&info);
    if (!ws_context) {
        lwsl_err("lws_create_context failed\n");
        return;
    }

    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = ws_context;
    ccinfo.address = "localhost";
    ccinfo.port = 8081;
    ccinfo.path = "/";
    ccinfo.host = lws_canonical_hostname(ws_context);
    ccinfo.origin = "origin";
    ccinfo.protocol = "my-protocol";
    ccinfo.pwsi = &ws_wsi;
    ccinfo.ssl_connection = 0;
    
    ws_wsi = lws_client_connect_via_info(&ccinfo);
    if (!ws_wsi) {
        lwsl_err("lws_client_connect_via_info failed\n");
        return;
    }

    pthread_t ws_thread;
    pthread_create(&ws_thread, NULL, ws_service_thread, NULL);
    pthread_detach(ws_thread);
}


/*==========================*/
/*      GStreamer Stuff     */
/*==========================*/

#define SECOND_IN_NSECS 1000000000UL

#ifdef X_DISPLAY_FIX
#include <gst/video/navigation.h>
#include "x_display_fix.h"
static bool fullscreen = false;
static bool alt_keypress = false;
static unsigned char X11_search_attempts;
#endif

static GstClockTime gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
static unsigned short width, height, width_source, height_source;  /* not currently used */
static bool first_packet = false;
static bool do_sync = false;
static bool auto_videosink = true;
static bool hls_video = false;
#ifdef X_DISPLAY_FIX
static bool use_x11 = false;
#endif
static bool logger_debug = false;
static bool video_terminate = false;

#define NCODECS  2   /* renderers for h264 and h265 */

struct video_renderer_s {
    GstElement *appsrc, *pipeline;
    GstBus *bus;
    const char *codec;
    bool autovideo, state_pending;
    int id;
    gboolean terminate;
    gint64 duration;
    gint buffering_level;
#ifdef  X_DISPLAY_FIX
    bool use_x11;
    const char * server_name;
    X11_Window_t * gst_window;
#endif
};

static video_renderer_t *renderer = NULL;
static video_renderer_t *renderer_type[NCODECS] = {0};
static int n_renderers = NCODECS;
static char h264[] = "h264";
static char h265[] = "h265";
static char hls[] = "hls";

static void append_videoflip(GString *launch, const videoflip_t *flip, const videoflip_t *rot) {
    /* videoflip image transform */
    switch (*flip) {
    case INVERT:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_180 ! ");
            break;
        }
        break;
    case HFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_HORIZ ! ");
            break;
        }
        break;
    case VFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_VERT ! ");
            break;
        }
        break;
    default:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
            break;
        default:
            break;
        }
        break;
    }
}

/* Apple uses colorimetry 1:3:7:1 (BT709, sRGB) which older GStreamer versions may not fully parse. */
static const char h264_caps[] = "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[] = "video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";

void video_renderer_size(float *f_width_source, float *f_height_source, float *f_width, float *f_height) {
    width_source = (unsigned short)*f_width_source;
    height_source = (unsigned short)*f_height_source;
    width = (unsigned short)*f_width;
    height = (unsigned short)*f_height;
    logger_log(logger, LOGGER_DEBUG,
        "begin video stream wxh = %dx%d; source %dx%d",
        width, height, width_source, height_source
    );
}

/* Helper to create a videosink for playbin, if not autovideosink */
GstElement *make_video_sink(const char *videosink, const char *videosink_options) {
    GstElement *video_sink = gst_element_factory_make(videosink, "videosink");
    if (!video_sink) {
        return NULL;
    }

    size_t len = strlen(videosink_options);
    if (!len) {
        return video_sink;
    }
    char *options = (char *)malloc(len + 1);
    strncpy(options, videosink_options, len + 1);

    char *end = strchr(options, '!');
    if (end) {
        *end = '\0';
    }

    char *token;
    char *text = options;
    while ((token = strtok_r(text, " ", &text))) {
        char *pval = strchr(token, '=');
        if (pval) {
            *pval = '\0';
            pval++;
            const gchar *property_name = (const gchar *)token;
            const gchar *value = (const gchar *)pval;
            g_print("playbin_videosink property: \"%s\" \"%s\"\n", property_name, value);
            gst_util_set_object_arg(G_OBJECT(video_sink), property_name, value);
        }
    }
    free(options);
    return video_sink;
}

/*=====================*/
/* on_new_sample() CB  */
/*=====================*/
#define WS_MAX_PAYLOAD (16 * 1024) // Adjust as needed for your WebSocket server


#define BUFFER_SIZE 65536  // More conservative buffer size
static unsigned char* pending_buffer = NULL;
static size_t pending_size = 0;

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        if (connected && ws_wsi) {
            printf("map.size = %zu\n", map.size);

            // Allocate a single large buffer for the entire frame
            unsigned char *ws_buf = (unsigned char *)malloc(LWS_PRE + map.size);
            if (ws_buf) {
                // Copy the entire frame into the buffer (after the LWS padding)
                memcpy(ws_buf + LWS_PRE, map.data, map.size);

                // Debug print once
                printf("Sending entire frame: size=%zu bytes\n", map.size);
                if (map.size >= 4) {
                    // Print first 4 bytes
                    printf("First few bytes: %02x %02x %02x %02x\n",
                           map.data[0], map.data[1], map.data[2], map.data[3]);
                }

                // Single WebSocket write call
                int sent = lws_write(ws_wsi, ws_buf + LWS_PRE, map.size, LWS_WRITE_BINARY);
                
                free(ws_buf);

                if (sent < 0) {
                    gst_buffer_unmap(buffer, &map);
                    gst_sample_unref(sample);
                    return GST_FLOW_ERROR;
                }
            }
        }
        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}




/*==============================*/
/*  Initialize WebSocket + Tee  */
/*==============================*/

void video_renderer_init(logger_t *render_logger, const char *server_name, 
                         videoflip_t videoflip[2], const char *parser,
                         const char *decoder, const char *converter,
                         const char *videosink, const char *videosink_options,
                         bool initial_fullscreen, bool video_sync,
                         bool h265_support, const char *uri) 
{
    GError *error = NULL;
    GstCaps *caps = NULL;

    hls_video = (uri != NULL);
    auto_videosink = (strstr(videosink, "autovideosink") || strstr(videosink, "fpsdisplaysink"));

    logger = render_logger;
    logger_debug = (logger_get_level(logger) >= LOGGER_DEBUG);
    video_terminate = false;

    /* Set the X11 window title if needed */
    const gchar *appname = g_get_application_name();
    if (!appname || strcmp(appname, server_name)) {
        g_set_application_name(server_name);
    }

    // Initialize the WebSocket client once; optional to move it elsewhere
    init_websocket_client();

    if (hls_video) {
        n_renderers = 1;
    } else {
        n_renderers = h265_support ? 2 : 1;
    }
    g_assert(n_renderers <= NCODECS);

    for (int i = 0; i < n_renderers; i++) {
        g_assert(i < 2);
        renderer_type[i] = (video_renderer_t *)calloc(1, sizeof(video_renderer_t));
        g_assert(renderer_type[i]);
        renderer_type[i]->autovideo = auto_videosink;
        renderer_type[i]->id = i;
        renderer_type[i]->bus = NULL;

        /*=================*
         *   HLS BRANCH    *
         *=================*/
        if (hls_video) {
            renderer_type[i]->pipeline =
                gst_element_factory_make("playbin3", "hls-playbin3");
            g_assert(renderer_type[i]->pipeline);
            renderer_type[i]->appsrc = NULL;
            renderer_type[i]->codec = hls;

            if (strcmp(videosink, "autovideosink")) {
                GstElement *playbin_videosink = make_video_sink(videosink, videosink_options);
                if (!playbin_videosink) {
                    logger_log(logger, LOGGER_ERR, 
                               "video_renderer_init: failed to create playbin_videosink");
                } else {
                    logger_log(logger, LOGGER_DEBUG,
                               "video_renderer_init: create playbin_videosink at %p", 
                               playbin_videosink);
                    g_object_set(G_OBJECT(renderer_type[i]->pipeline),
                                 "video-sink", playbin_videosink, NULL);
                }
            }
            g_object_set(G_OBJECT(renderer_type[i]->pipeline), "uri", uri, NULL);

        /*====================*
         * MIRROR MODE BRANCH (h264/h265)
         *====================*/
        } else {
            switch (i) {
            case 0:
                renderer_type[i]->codec = h264;
                caps = gst_caps_from_string(h264_caps);
                break;
            case 1:
                renderer_type[i]->codec = h265;
                caps = gst_caps_from_string(h265_caps);
                break;
            default:
                g_assert(0);
            }

            GString *launch = g_string_new("appsrc name=video_source ! ");
            // g_string_append(launch, "queue ! ");
            g_string_append(launch, parser);
            g_string_append(launch, " ! ");
            g_string_append(launch, decoder);
            g_string_append(launch, " ! ");
            append_videoflip(launch, &videoflip[0], &videoflip[1]);
            g_string_append(launch, converter);
            g_string_append(launch, " ! ");

            /* 
             * Insert tee + appsink + local videosink 
             * We remove the old "videoscale ! videosink" lines 
             */
            g_string_append(launch,
                "tee name=videotee ! "
                "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
                "videoscale ! videorate max-rate=30 ! "  // Limit to 30fps
                "videoconvert ! "
                "video/x-raw,format=RGBA,framerate=30/1 ! "  // Force 30fps
                "appsink name=uxplay_sink sync=false "
                "max-buffers=2 drop=true enable-last-sample=false "
                "emit-signals=true "
                "videotee. ! queue ! videoscale ! "
            );
            g_string_append(launch, videosink);

            g_string_append(launch, " name=");
            g_string_append(launch, videosink);
            g_string_append(launch, "_");
            g_string_append(launch, renderer_type[i]->codec);
            g_string_append(launch, videosink_options);

            // if (video_sync) {
            //     g_string_append(launch, " do_sync=true");
            //     do_sync = true;
            // } else {
            //     g_string_append(launch, " do_sync=false");
            //     do_sync = false;
            // }

            /* fix references if any */
            if (!strcmp(renderer_type[i]->codec, h264)) {
                char *pos = launch->str;
                while ((pos = strstr(pos, h265))) {
                    pos += 3;
                    *pos = '4';
                }
            } else if (!strcmp(renderer_type[i]->codec, h265)) {
                char *pos = launch->str;
                while ((pos = strstr(pos, h264))) {
                    pos += 3;
                    *pos = '5';
                }
            }

            logger_log(logger, LOGGER_DEBUG,
                       "GStreamer video pipeline %d:\n\"%s\"",
                       i + 1, launch->str);

            renderer_type[i]->pipeline = gst_parse_launch(launch->str, &error);
            if (error) {
                g_error("get_parse_launch error (video):\n %s\n", error->message);
                g_clear_error(&error);
            }
            g_assert(renderer_type[i]->pipeline);

            /* Use real-time clock */
            GstClock *clock = gst_system_clock_obtain();
            g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);
            gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer_type[i]->pipeline), clock);

            /* fetch the appsrc by name */
            renderer_type[i]->appsrc = gst_bin_get_by_name(
                GST_BIN(renderer_type[i]->pipeline), "video_source");
            g_assert(renderer_type[i]->appsrc);

            g_object_set(renderer_type[i]->appsrc,
                         "caps", caps,
                         "stream-type", 0,
                         "is-live", TRUE,
                         "format", GST_FORMAT_TIME,
                         NULL);

            g_string_free(launch, TRUE);
            gst_caps_unref(caps);
            gst_object_unref(clock);

            /* Now retrieve the appsink named "uxplay_sink" & set callback */
            GstElement *appsink = gst_bin_get_by_name(
                GST_BIN(renderer_type[i]->pipeline), "uxplay_sink");
            if (appsink) {
                gst_app_sink_set_emit_signals(GST_APP_SINK(appsink), TRUE);
                static GstAppSinkCallbacks appsink_callbacks = {
                    .eos         = NULL,
                    .new_preroll = NULL,
                    .new_sample  = on_new_sample
                };
                gst_app_sink_set_callbacks(GST_APP_SINK(appsink),
                                           &appsink_callbacks,
                                           NULL, NULL);
                gst_object_unref(appsink);
            }
        }

#ifdef X_DISPLAY_FIX
        use_x11 = (strstr(videosink, "xvimagesink") 
                || strstr(videosink, "ximagesink") 
                || auto_videosink);
        fullscreen = initial_fullscreen;
        renderer_type[i]->server_name = server_name;
        renderer_type[i]->gst_window = NULL;
        renderer_type[i]->use_x11 = false;
        X11_search_attempts = 0;

        if (use_x11) {
            if (i == 0) {
                renderer_type[0]->gst_window = (X11_Window_t *)calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[0]->gst_window);
                get_X11_Display(renderer_type[0]->gst_window);
                if (renderer_type[0]->gst_window->display) {
                    renderer_type[i]->use_x11 = true;
                } else {
                    free(renderer_type[0]->gst_window);
                    renderer_type[0]->gst_window = NULL;
                }
            } else if (renderer_type[0]->use_x11) {
                renderer_type[i]->gst_window = (X11_Window_t *)calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[i]->gst_window);
                memcpy(renderer_type[i]->gst_window, renderer_type[0]->gst_window, sizeof(X11_Window_t));
                renderer_type[i]->use_x11 = true;
            }
        }
#endif

        gst_element_set_state(renderer_type[i]->pipeline, GST_STATE_READY);
        GstState state;
        if (gst_element_get_state(renderer_type[i]->pipeline, &state, NULL, 100 * GST_MSECOND)) {
            if (state == GST_STATE_READY) {
                logger_log(logger, LOGGER_DEBUG, 
                           "Initialized GStreamer video renderer %d", i + 1);
                if (hls_video && i == 0) {
                    renderer = renderer_type[i];
                }
            } else {
                logger_log(logger, LOGGER_ERR,
                           "Failed to initialize GStreamer video renderer %d", i + 1);
            }
        } else {
            logger_log(logger, LOGGER_ERR,
                       "Failed to initialize GStreamer video renderer %d", i + 1);
        }
    } // end for
}

/* Pause the video renderer */
void video_renderer_pause() {
    if (!renderer) return;
    logger_log(logger, LOGGER_DEBUG, "video renderer paused");
    gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
}

/* Resume from pause */
void video_renderer_resume() {
    if (!renderer) return;
    gst_element_set_state(renderer->pipeline, GST_STATE_PLAYING);
    GstState state;
    gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
    const gchar *state_name = gst_element_state_get_name(state);
    logger_log(logger, LOGGER_DEBUG, "video renderer resumed: state %s", state_name);
    if (renderer->appsrc) {
        gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    }
}

/* Start rendering: either for HLS or for mirror-mode (both pipelines) */
void video_renderer_start() {
    if (hls_video) {
        renderer->bus = gst_element_get_bus(renderer->pipeline);
        gst_element_set_state(renderer->pipeline, GST_STATE_PLAYING);
        return;
    }
    /* Mirror mode: start h264/h265 pipelines. We'll pick the right codec later. */
    for (int i = 0; i < n_renderers; i++) {
        gst_element_set_state(renderer_type[i]->pipeline, GST_STATE_PLAYING);
        if (renderer_type[i]->appsrc) {
            gst_video_pipeline_base_time = gst_element_get_base_time(renderer_type[i]->appsrc);
        }
        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);
    }
    renderer = NULL;
    first_packet = true;
#ifdef X_DISPLAY_FIX
    X11_search_attempts = 0;
#endif
}

/* If using playbin (HLS), we wait for a window to appear, if X11 is used */
bool waiting_for_x11_window() {
    if (!hls_video) {
        return false;
    }
#ifdef X_DISPLAY_FIX
    if (use_x11 && renderer->gst_window) {
        get_x_window(renderer->gst_window, renderer->server_name);
        if (!renderer->gst_window->window) {
            return true;
        }
    }
#endif
    return false;
}

/* Called from raop_rtp_mirror to push compressed frames into the pipeline. */
void video_renderer_render_buffer(unsigned char* data, int *data_len, 
                                  int *nal_count, uint64_t *ntp_time) 
{
    GstBuffer *buffer;
    GstClockTime pts = (GstClockTime)*ntp_time; /* in nsecs */
    if (do_sync) {
        if (pts >= gst_video_pipeline_base_time) {
            pts -= gst_video_pipeline_base_time;
        } else {
            logger_log(logger, LOGGER_ERR,
                       "*** invalid ntp_time < gst_video_pipeline_base_time\n"
                       "%8.6f ntp_time\n%8.6f base_time",
                       ((double)*ntp_time) / SECOND_IN_NSECS,
                       ((double)gst_video_pipeline_base_time) / SECOND_IN_NSECS);
            return;
        }
    }
    g_assert(data_len != 0);
    if (data[0]) {
        logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed ");
    } else {
        if (first_packet) {
            logger_log(logger, LOGGER_INFO, 
                       "Begin streaming to GStreamer video pipeline");
            first_packet = false;
        }
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
        g_assert(buffer != NULL);
        if (do_sync) {
            GST_BUFFER_PTS(buffer) = pts;
        }
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer(GST_APP_SRC(renderer->appsrc), buffer);
    }
}

/* Flush the pipeline if needed */
void video_renderer_flush() {
    // implement if needed
}

/* Stop the pipeline (NULL state) */
void video_renderer_stop() {
    if (renderer) {
        if (renderer->appsrc) {
            gst_app_src_end_of_stream(GST_APP_SRC(renderer->appsrc));
        }
        gst_element_set_state(renderer->pipeline, GST_STATE_NULL);
    }
}

/* Helper to destroy one renderer */
static void video_renderer_destroy_h26x(video_renderer_t *renderer) {
    if (!renderer) return;
    GstState state;
    gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
    if (state != GST_STATE_NULL) {
        if (!hls_video) {
            gst_app_src_end_of_stream(GST_APP_SRC(renderer->appsrc));
        }
        gst_element_set_state(renderer->pipeline, GST_STATE_NULL);
    }
    gst_object_unref(renderer->bus);
    if (renderer->appsrc) {
        gst_object_unref(renderer->appsrc);
    }
    gst_object_unref(renderer->pipeline);

#ifdef X_DISPLAY_FIX
    if (renderer->gst_window) {
        free(renderer->gst_window);
        renderer->gst_window = NULL;
    }
#endif
    free(renderer);
}

/* Destroy all renderers */
void video_renderer_destroy() {
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i]) {
            video_renderer_destroy_h26x(renderer_type[i]);
        }
    }
}

/* Our GStreamer bus callback for handling error/EOS, etc. */
gboolean gstreamer_pipeline_bus_callback(GstBus *bus, GstMessage *message, void *loop) {
    int type = -1;
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i]->bus == bus) {
            type = i;
            break;
        }
    }
    g_assert(type != -1);

    if (logger_debug) {
        g_print("GStreamer %s bus message: %s %s\n",
                renderer_type[type]->codec,
                GST_MESSAGE_SRC_NAME(message),
                GST_MESSAGE_TYPE_NAME(message));
    }

    if (logger_debug && hls_video) {
        gint64 pos;
        gst_element_query_position(renderer_type[type]->pipeline, GST_FORMAT_TIME, &pos);
        if (GST_CLOCK_TIME_IS_VALID(pos)) {
            g_print("GStreamer bus message %s %s; position: %" GST_TIME_FORMAT "\n",
                    GST_MESSAGE_SRC_NAME(message),
                    GST_MESSAGE_TYPE_NAME(message),
                    GST_TIME_ARGS(pos));
        } else {
            g_print("GStreamer bus message %s %s; position: none\n",
                    GST_MESSAGE_SRC_NAME(message),
                    GST_MESSAGE_TYPE_NAME(message));
        }
    }

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_DURATION:
        renderer_type[type]->duration = GST_CLOCK_TIME_NONE;
        break;

    case GST_MESSAGE_BUFFERING:
        if (hls_video) {
            gint percent = -1;
            gst_message_parse_buffering(message, &percent);
            if (percent >= 0) {
                renderer_type[type]->buffering_level = percent;
                logger_log(logger, LOGGER_DEBUG, 
                           "Buffering :%u percent done", percent);
                if (percent < 100) {
                    gst_element_set_state(
                        renderer_type[type]->pipeline, GST_STATE_PAUSED
                    );
                } else {
                    gst_element_set_state(
                        renderer_type[type]->pipeline, GST_STATE_PLAYING
                    );
                }
            }
        }
        break;

    case GST_MESSAGE_ERROR: {
        GError *err;
        gchar *debug;
        gst_message_parse_error(message, &err, &debug);
        logger_log(logger, LOGGER_INFO, 
                   "GStreamer error: %s %s",
                   GST_MESSAGE_SRC_NAME(message),
                   err->message);

        if (!hls_video && strstr(err->message, "Internal data stream error")) {
            logger_log(logger, LOGGER_INFO,
                       "*** This is a generic GStreamer error indicating an inability\n"
                       "*** to construct a working video pipeline.\n"
                       "*** Try using -avdec or a different -vs <sink>.\n"
                       "*** Raspberry Pi might need \"-bt709\" option.\n");
        }
        g_error_free(err);
        g_free(debug);

        if (renderer_type[type]->appsrc) {
            gst_app_src_end_of_stream(
                GST_APP_SRC(renderer_type[type]->appsrc)
            );
        }
        gst_bus_set_flushing(bus, TRUE);
        gst_element_set_state(
            renderer_type[type]->pipeline, GST_STATE_READY
        );
        renderer_type[type]->terminate = TRUE;
        g_main_loop_quit((GMainLoop *)loop);
        break;
    }

    case GST_MESSAGE_EOS:
        logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream");
        if (hls_video) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state(renderer_type[type]->pipeline, GST_STATE_READY);
            renderer_type[type]->terminate = TRUE;
            g_main_loop_quit((GMainLoop *)loop);
        }
        break;

    case GST_MESSAGE_STATE_CHANGED:
        if (renderer_type[type]->state_pending &&
            strstr(GST_MESSAGE_SRC_NAME(message), "pipeline")) {
            GstState state;
            gst_element_get_state(
                renderer_type[type]->pipeline, &state, NULL, 100 * GST_MSECOND
            );
            if (state == GST_STATE_NULL) {
                gst_element_set_state(renderer_type[type]->pipeline, GST_STATE_PLAYING);
            } else if (state == GST_STATE_PLAYING) {
                renderer_type[type]->state_pending = false;
            }
        }
        if (renderer_type[type]->autovideo) {
            char *sink = strstr(GST_MESSAGE_SRC_NAME(message), "-actual-sink-");
            if (sink) {
                sink += strlen("-actual-sink-");
                if (strstr(GST_MESSAGE_SRC_NAME(message),
                           renderer_type[type]->codec)) {
                    logger_log(logger, LOGGER_DEBUG,
                               "GStreamer: automatically-selected videosink"
                               " (renderer %d: %s) is \"%ssink\"",
                               renderer_type[type]->id + 1,
                               renderer_type[type]->codec, sink);
#ifdef X_DISPLAY_FIX
                    renderer_type[type]->use_x11 = 
                        (strstr(sink, "ximage") || strstr(sink, "xvimage"));
#endif
                    renderer_type[type]->autovideo = false;
                }
            }
        }
        break;

#ifdef X_DISPLAY_FIX
    case GST_MESSAGE_ELEMENT:
        if (renderer_type[type]->gst_window && renderer_type[type]->gst_window->window) {
            GstNavigationMessageType message_type =
                gst_navigation_message_get_type(message);
            if (message_type == GST_NAVIGATION_MESSAGE_EVENT) {
                GstEvent *event = NULL;
                if (gst_navigation_message_parse_event(message, &event)) {
                    GstNavigationEventType event_type =
                        gst_navigation_event_get_type(event);
                    const gchar *key;
                    switch (event_type) {
                    case GST_NAVIGATION_EVENT_KEY_PRESS:
                        if (gst_navigation_event_parse_key_event(event, &key)) {
                            if ((strcmp(key, "F11") == 0) ||
                                (alt_keypress && strcmp(key, "Return") == 0)) {
                                fullscreen = !fullscreen;
                                set_fullscreen(
                                    renderer_type[type]->gst_window, &fullscreen
                                );
                            } else if (strcmp(key, "Alt_L") == 0) {
                                alt_keypress = true;
                            }
                        }
                        break;

                    case GST_NAVIGATION_EVENT_KEY_RELEASE:
                        if (gst_navigation_event_parse_key_event(event, &key)) {
                            if (strcmp(key, "Alt_L") == 0) {
                                alt_keypress = false;
                            }
                        }
                        break;

                    default:
                        break;
                    }
                }
                if (event) {
                    gst_event_unref(event);
                }
            }
        }
        break;
#endif

    default:
        break;
    }
    return TRUE;
}

/* Switch between h264/h265 pipelines once we detect the correct codec. */
void video_renderer_choose_codec(bool video_is_h265) {
    g_assert(!hls_video);
    video_renderer_t *renderer_new =
        video_is_h265 ? renderer_type[1] : renderer_type[0];
    if (renderer == renderer_new) {
        return;
    }
    video_renderer_t *renderer_prev = renderer;
    renderer = renderer_new;
    gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);

    if (renderer_prev) {
        gst_app_src_end_of_stream(
            GST_APP_SRC(renderer_prev->appsrc)
        );
        gst_bus_set_flushing(renderer_prev->bus, TRUE);
        gst_element_set_state(renderer_prev->pipeline, GST_STATE_NULL);
        renderer_prev->state_pending = true;
    }
}

/* Called periodically if video_terminate is set. */
unsigned int video_reset_callback(void *loop) {
    if (video_terminate) {
        video_terminate = false;
        if (renderer->appsrc) {
            gst_app_src_end_of_stream(
                GST_APP_SRC(renderer->appsrc)
            );
        }
        gst_bus_set_flushing(renderer->bus, TRUE);
        gst_element_set_state(renderer->pipeline, GST_STATE_NULL);
        g_main_loop_quit((GMainLoop *)loop);
    }
    return (unsigned int)TRUE;
}

/* Query playback info for HLS scenario, if needed. */
bool video_get_playback_info(double *duration, double *position, float *rate) {
    gint64 pos = 0;
    GstState state;
    *duration = 0.0;
    *position = -1.0;
    *rate = 0.0f;

    if (!renderer) {
        return true;
    }
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);

    switch (state) {
    case GST_STATE_PLAYING:
        *rate = 1.0f;
        break;
    default:
        break;
    }

    if (!GST_CLOCK_TIME_IS_VALID(renderer->duration)) {
        if (!gst_element_query_duration(
                 renderer->pipeline, GST_FORMAT_TIME, &renderer->duration)) {
            return true;
        }
    }
    *duration = ((double)renderer->duration) / GST_SECOND;
    if (*duration) {
        if (gst_element_query_position(
                renderer->pipeline, GST_FORMAT_TIME, &pos)
            && GST_CLOCK_TIME_IS_VALID(pos)) {
            *position = ((double)pos) / GST_SECOND;
        }
    }

    logger_log(logger, LOGGER_DEBUG,
               "********* video_get_playback_info: position %" GST_TIME_FORMAT
               " duration %" GST_TIME_FORMAT " %s *********",
               GST_TIME_ARGS(pos),
               GST_TIME_ARGS(renderer->duration),
               gst_element_state_get_name(state));
    return true;
}

/* Seek for HLS scenario (rarely used). */
void video_renderer_seek(float position) {
    double pos = (double)position * (double)GST_SECOND;
    gint64 seek_position = (gint64)pos;
    seek_position = seek_position < 1000 ? 1000 : seek_position;
    if (renderer->duration > 0) {
        if (seek_position > (renderer->duration - 1000)) {
            seek_position = renderer->duration - 1000;
        }
    }

    g_print("SCRUB: seek to %f secs =  %" GST_TIME_FORMAT
            ", duration = %" GST_TIME_FORMAT "\n",
            position,
            GST_TIME_ARGS(seek_position),
            GST_TIME_ARGS(renderer->duration));

    gboolean result = gst_element_seek_simple(
        renderer->pipeline,
        GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
        seek_position
    );
    if (result) {
        g_print("seek succeeded\n");
        gst_element_set_state(renderer->pipeline, GST_STATE_PLAYING);
    } else {
        g_print("seek failed\n");
    }
}

/* Listen for bus messages; used in main_loop. */
unsigned int video_renderer_listen(void *loop, int id) {
    g_assert(id >= 0 && id < n_renderers);
    return (unsigned int)gst_bus_add_watch(
        renderer_type[id]->bus,
        (GstBusFunc)gstreamer_pipeline_bus_callback,
        (gpointer)loop
    );
}