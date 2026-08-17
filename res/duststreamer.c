/*
 * duststreamer - camera streaming for vacuum robots
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
 
#include <glib.h>
#include <glib-unix.h>
#include <gst/gst.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>

#ifndef DUSTSTREAMER_VERSION
#define DUSTSTREAMER_VERSION "unknown"
#endif

typedef enum {
    PLATFORM_INVALID = 0,
    PLATFORM_DREAME_MR813,
    PLATFORM_DREAME_MR536,
    PLATFORM_MIDEA_RK3566,
} Platform;

static const struct { const gchar *name; Platform platform; } platform_table[] = {
    { "dreame_mr813",  PLATFORM_DREAME_MR813  },
    { "dreame_mr536",  PLATFORM_DREAME_MR536  },
    { "midea_rk3566",  PLATFORM_MIDEA_RK3566  },
};

static gboolean opt_version = FALSE;
static gchar *opt_platform = NULL;
static gchar *opt_device = NULL;
static gint opt_width = 0;
static gint opt_height = 0;
static gint opt_framerate = 0;
static gint opt_bitrate = 0;

static gchar *opt_host = NULL;
static gint opt_port_ts = 12727;
static gint opt_nice = 10;

static GOptionEntry entries[] = {
    { "version", 'v', 0, G_OPTION_ARG_NONE, &opt_version,
        "Show version information and exit", NULL },
    { "platform", 0, 0, G_OPTION_ARG_STRING, &opt_platform,
        "Target platform [required]: dreame_mr813 | dreame_mr536 | midea_rk3566", "NAME" },
    { "device", 'd', 0, G_OPTION_ARG_STRING, &opt_device,
        "V4L2 device node [required]", "PATH" },
    { "width", 'w', 0, G_OPTION_ARG_INT, &opt_width,
        "Width [required]", "PX" },
    { "height", 'h', 0, G_OPTION_ARG_INT, &opt_height,
        "Height [required]", "PX" },
    { "framerate", 'f', 0, G_OPTION_ARG_INT, &opt_framerate,
        "Framerate [required]", "FPS" },
    { "bitrate", 'b', 0, G_OPTION_ARG_INT, &opt_bitrate,
        "Bitrate in bps [required]", "BPS" },
    { "host", 'H', 0, G_OPTION_ARG_STRING, &opt_host,
        "UDP target host [default: 127.127.127.127]", "IP" },
    { "port-ts", 0, 0, G_OPTION_ARG_INT, &opt_port_ts,
        "UDP port for MPEG-TS [default: 12727]", "PORT" },
    { "nice", 'n', 0, G_OPTION_ARG_INT, &opt_nice,
        "Process nice value [default: 10]", "NICE" },
    { NULL }
};

static gboolean
on_terminate_signal(gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static GstPadProbeReturn
on_source_caps(GstPad *pad G_GNUC_UNUSED, GstPadProbeInfo *info, gpointer user_data G_GNUC_UNUSED)
{
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);

    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
        GstCaps *caps;
        gst_event_parse_caps(event, &caps);
        if (caps) {
            GstStructure *s = gst_caps_get_structure(caps, 0);
            gint width = 0, height = 0, fps_num = 0, fps_den = 1;
            const gchar *format = gst_structure_get_string(s, "format");

            if (gst_structure_get_int(s, "width", &width) &&
                gst_structure_get_int(s, "height", &height)) {

                gst_structure_get_fraction(s, "framerate", &fps_num, &fps_den);
                double actual_fps = (fps_den > 0) ? ((double)fps_num / fps_den) : 0.0;
                
                g_printerr("duststreamer: [SOURCE] camera resolution: %dx%d @ %.2f fps (format: %s)\n",
                           width, height, actual_fps, format ? format : "unknown");
                g_printerr("duststreamer: [TARGET] stream resolution: %dx%d @ %d fps\n", 
                           opt_width, opt_height, opt_framerate);

                double src_ar = (double)width / (double)height;
                double target_ar = (double)opt_width / (double)opt_height;
                double diff = src_ar - target_ar;

                if (diff < -0.05 || diff > 0.05) {
                    g_printerr("duststreamer: WARNING: Aspect ratio mismatch!\n"
                               "            Camera is %.2f:1 but Target is %.2f:1.\n", 
                               src_ar, target_ar);
                }

                return GST_PAD_PROBE_REMOVE;
            }
        }
    }
    return GST_PAD_PROBE_OK;
}

static gboolean
on_error(GstBus *bus G_GNUC_UNUSED, GstMessage *msg, gpointer data)
{
    GError *err = NULL;
    gchar *dbg = NULL;
    GstElement *pipeline = GST_ELEMENT(data);

    gst_message_parse_error(msg, &err, &dbg);
    g_printerr("duststreamer: pipeline error: %s (%s)\n",
               err ? err->message : "(null)", dbg ? dbg : "(null)");
    g_print("ERROR: %s\n", err ? err->message : "(null)");
    g_clear_error(&err);
    g_clear_pointer(&dbg, g_free);

    gst_element_post_message(pipeline,
        gst_message_new_application(GST_OBJECT(pipeline),
            gst_structure_new_empty("duststreamer-quit")));
    return TRUE;
}

static void
on_state_changed(GstBus *bus G_GNUC_UNUSED, GstMessage *msg, gpointer data)
{
    GstElement *pipeline = GST_ELEMENT(data);

    if (GST_MESSAGE_SRC(msg) != GST_OBJECT(pipeline))
        return;

    GstState old, new_state;
    gst_message_parse_state_changed(msg, &old, &new_state, NULL);
    if (new_state == GST_STATE_PLAYING) {
        g_print("PLAYING\n");
    }
}

static void
on_eos(GstBus *bus G_GNUC_UNUSED, GstMessage *msg G_GNUC_UNUSED, gpointer data)
{
    GstElement *pipeline = GST_ELEMENT(data);

    gst_element_post_message(pipeline,
        gst_message_new_application(GST_OBJECT(pipeline),
            gst_structure_new_empty("duststreamer-quit")));
}

int
main(int argc, char **argv)
{
    GstElement *pipeline = NULL;
    GstBus *bus = NULL;
    GMainLoop *loop = NULL;
    GError *error = NULL;
    GOptionContext *context = NULL;
    int ret = 0;
    Platform platform = PLATFORM_INVALID;

    signal(SIGPIPE, SIG_IGN);

    setvbuf(stdout, NULL, _IOLBF, 0);

    context = g_option_context_new("- duststreamer GStreamer daemon");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_add_group(context, gst_init_get_option_group());
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("duststreamer: option parsing failed: %s\n", error->message);
        g_clear_error(&error);
        g_option_context_free(context);
        return 1;
    }

    if (opt_version) {
        g_print("duststreamer version %s\n", DUSTSTREAMER_VERSION);
        g_option_context_free(context);
        return 0;
    }

    for (gsize i = 0; i < G_N_ELEMENTS(platform_table); i++) {
        if (g_strcmp0(opt_platform, platform_table[i].name) == 0) {
            platform = platform_table[i].platform;
            break;
        }
    }

    if (platform == PLATFORM_INVALID || !opt_device || opt_device[0] == '\0' ||
        opt_width <= 0 || opt_height <= 0 || opt_framerate <= 0 || opt_bitrate <= 0) {
        if (!opt_platform) {
            g_printerr("duststreamer: --platform is required\n");
        } else if (platform == PLATFORM_INVALID) {
            g_printerr("duststreamer: unknown platform '%s'\n", opt_platform);
        } else {
            g_printerr("duststreamer: missing required option(s)\n");
        }
        gchar *help = g_option_context_get_help(context, TRUE, NULL);
        g_printerr("%s", help);
        g_free(help);
        g_option_context_free(context);
        return 1;
    }

    static const gint valid_mpeg1_fps[] = { 24, 25, 30, 50, 60 };
    gboolean fps_ok = FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(valid_mpeg1_fps); i++) {
        if (opt_framerate == valid_mpeg1_fps[i]) {
            fps_ok = TRUE;
            break;
        }
    }
    if (!fps_ok) {
        g_printerr("duststreamer: invalid framerate %d: MPEG-1 supports 24, 25, 30, 50 or 60 fps\n",
                   opt_framerate);
        return 1;
    }

    g_option_context_free(context);

    if (setpriority(PRIO_PROCESS, 0, opt_nice) == -1) {
        g_printerr("duststreamer: WARNING: failed to set process priority to %d\n", opt_nice);
    } else {
        g_printerr("duststreamer: running with reduced priority (nice = %d)\n", opt_nice);
    }

    if (!opt_host) opt_host = g_strdup("127.127.127.127");

    gchar *launch_str = NULL;

    /* The default EPZS motion search dominates encode time and the SoC is already
     * saturated by ava. xone is a single-iteration search that still tracks motion,
     * unlike zero, which would fall back to intra whenever the robot is driving. */

    if (platform == PLATFORM_DREAME_MR813 || platform == PLATFORM_DREAME_MR536) {
        g_printerr("duststreamer: platform %s -> source element: dustsrc (device: %s)\n",
                   opt_platform, opt_device);
        launch_str = g_strdup_printf(
            "dustsrc name=cam_src device=%s capture-width=%d capture-height=%d capture-framerate=%d/1 ! "
            "queue name=q_src leaky=downstream max-size-buffers=2 ! "
            "videorate ! "
            "video/x-raw,framerate=%d/1 ! "
            "videoconvert ! "
            "avenc_mpeg1video bitrate=%d motion-est=xone ! "
            "mpegtsmux alignment=7 ! "
            "queue name=q_sink ! "
            "udpsink host=%s port=%d sync=false",
            opt_device, opt_width, opt_height, opt_framerate,
            opt_framerate, opt_bitrate, opt_host, opt_port_ts);
    } else {  /* PLATFORM_MIDEA_RK3566 */
        g_printerr("duststreamer: platform %s -> source element: v4l2src (device: %s)\n",
                   opt_platform, opt_device);
        launch_str = g_strdup_printf(
            "v4l2src device=%s name=cam_src ! "
            "videoscale ! "
            "video/x-raw,format=I420,width=%d,height=%d ! "
            "queue name=q_src leaky=downstream max-size-buffers=2 ! "
            "videorate ! "
            "video/x-raw,framerate=%d/1 ! "
            "avenc_mpeg1video bitrate=%d motion-est=xone ! "
            "mpegtsmux alignment=7 ! "
            "queue name=q_sink ! "
            "udpsink host=%s port=%d sync=false",
            opt_device, opt_width, opt_height,
            opt_framerate, opt_bitrate, opt_host, opt_port_ts);
    }

    g_printerr("duststreamer version %s: starting pipeline...\n", DUSTSTREAMER_VERSION);

    pipeline = gst_parse_launch(launch_str, &error);
    g_free(launch_str);

    if (error || !pipeline) {
        g_printerr("duststreamer: failed to parse launch line: %s\n",
                   error ? error->message : "(null)");
        g_clear_error(&error);
        ret = 2;
        goto out;
    }

    GstElement *cam_src = gst_bin_get_by_name(GST_BIN(pipeline), "cam_src");
    if (cam_src) {
        GstPad *srcpad = gst_element_get_static_pad(cam_src, "src");
        if (srcpad) {
            gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, on_source_caps, NULL, NULL);
            gst_object_unref(srcpad);
        }
        gst_object_unref(cam_src);
    }

    loop = g_main_loop_new(NULL, FALSE);

    g_unix_signal_add(SIGINT, on_terminate_signal, loop);
    g_unix_signal_add(SIGTERM, on_terminate_signal, loop);

    bus = gst_element_get_bus(pipeline);
    gst_bus_add_signal_watch_full(bus, G_PRIORITY_HIGH);
    g_signal_connect(bus, "message::error", G_CALLBACK(on_error), pipeline);
    g_signal_connect(bus, "message::state-changed", G_CALLBACK(on_state_changed), pipeline);
    g_signal_connect(bus, "message::eos", G_CALLBACK(on_eos), pipeline);
    g_signal_connect_swapped(bus, "message::application",
        G_CALLBACK(g_main_loop_quit), loop);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("duststreamer: failed to set pipeline to PLAYING\n");
        ret = 3;
        goto out;
    }

    g_main_loop_run(loop);
    g_printerr("duststreamer: pipeline stopped\n");

out:
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
    if (bus) gst_object_unref(bus);
    if (loop) g_main_loop_unref(loop);
    g_free(opt_platform);
    g_free(opt_host);
    g_free(opt_device);
    return ret;
}
