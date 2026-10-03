#define _GNU_SOURCE
#include "capture.h"
#include "renderer.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gst/gst.h>
#include <linux/videodev2.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>


#define DEVICE_RESCAN_MS 1000
#define MODE_DEFAULT_W 1280
#define MODE_DEFAULT_H 720
#define MODE_DEFAULT_FPS 60
#define VIDEO_QUEUE_BUFFERS 1
#define AUDIO_QUEUE_BUFFERS 4
#define AUDIO_QUEUE_NS 50000000
#define AUDIO_LATENCY_US 20000
#define AUDIO_BUFFER_US 60000

typedef struct AppState AppState;
struct AppState {
    GtkApplication *application;
    GtkWidget *window;
    GtkWidget *video_area;
    CaptureRenderer *renderer;
    GtkWidget *root_overlay;
    GtkWidget *settings;
    GtkWidget *control_panel;
    GtkWidget *stats_overlay_label;
    GtkWidget *stats_overlay_box;
    GtkWidget *status_overlay;
    GtkWidget *status_overlay_label;
    GtkWidget *dwell_spin;
    GtkWidget *hide_delay_spin;
    GtkWidget *device_label;
    GtkWidget *mode_combo;
    GtkWidget *scale_combo;
    GtkWidget *audio_toggle;
    GtkWidget *volume_scale;
    GtkWidget *status_label;
    GtkWidget *perf_label;
    GPtrArray *modes;
    CaptureDevice *video_device;
    GstDeviceMonitor *device_monitor;
    GstDevice *audio_device;
    gchar *audio_device_id;
    gchar *audio_display_name;
    gchar *audio_sink_name;
    gchar *audio_sink_id;
    gchar *preferred_mode_key;
    gchar *config_path;
    gchar *log_path;
    gchar *renderer_backend_logged;
    gchar *pipeline_error;
    GstElement *pipeline;
    GstBus *pipeline_bus;
    GstElement *video_queue;
    GstElement *video_convert;
    GstElement *fps_sink;
    GstElement *video_sink;
    GstElement *audio_source;
    GstElement *audio_sink;
    guint bus_watch_id;
    guint device_watch_id;
    guint monitor_watch_id;
    guint stats_watch_id;
    guint dwell_watch_id;
    guint panel_hide_watch_id;
    guint config_save_watch_id;
    gint64 retry_after_us;
    guint current_mode;
    guint64 frames_dropped;
    guint64 queue_level;
    gdouble current_fps;
    gdouble average_fps;
    gdouble cpu_percent;
    gint64 audio_source_latency_us;
    gint64 audio_source_buffer_us;
    gint64 latency_min_ns;
    gint64 latency_max_ns;
    gint64 last_cpu_us;
    gint64 last_cpu_time_us;
    gdouble volume;
    gboolean audio_enabled;
    CaptureRendererScaleMode scale_mode;
    gboolean updating_controls;
    guint panel_dwell_ms;
    guint panel_hide_delay_ms;
    gint64 keyboard_active_until_us;
    gboolean fullscreen;
    gboolean closing;
    gboolean uvc_missing;
    gboolean panel_visible;
    gboolean panel_pointer_inside;
    gboolean edge_hotspot_inside;
    gboolean mode_popup_open;
    gboolean interaction_active;
    gboolean pinned;
    gboolean stats_visible;
    gboolean settings_open;
    GMutex stats_mutex;
    GMutex log_mutex;
};

static void app_refresh_ui(AppState *app);
static void app_restart_pipeline(AppState *app);
static void app_log(AppState *app, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static void app_log_session_context(AppState *app);
static void app_log_renderer_backend(AppState *app);
static void app_show_control_panel(AppState *app);
static void app_hide_control_panel(AppState *app);
static void app_schedule_panel_hide(AppState *app);
static void app_save_preferences(AppState *app);
static void app_schedule_preference_save(AppState *app);
static void create_control_panel(AppState *app);
static void create_settings(AppState *app);
static void populate_modes(AppState *app);
static void on_fps_measurements(GstElement *element, gdouble fps, gdouble droprate,
                                gdouble average, gpointer user_data);

static gint64
process_cpu_time_us(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
    return (gint64)usage.ru_utime.tv_sec * G_USEC_PER_SEC + usage.ru_utime.tv_usec +
           (gint64)usage.ru_stime.tv_sec * G_USEC_PER_SEC + usage.ru_stime.tv_usec;
}

static void
app_log(AppState *app, const gchar *format, ...)
{
    va_list args;
    va_start(args, format);
    gchar *message = g_strdup_vprintf(format, args);
    va_end(args);

    GDateTime *now = g_date_time_new_now_local();
    gchar *stamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    gchar *log_directory = g_path_get_dirname(app->log_path);
    g_mkdir_with_parents(log_directory, 0700);
    g_free(log_directory);
    g_mutex_lock(&app->log_mutex);
    FILE *file = fopen(app->log_path, "a");
    if (file != NULL) {
        fprintf(file, "%s %s\n", stamp, message);
        fclose(file);
    }
    g_mutex_unlock(&app->log_mutex);
    g_printerr("%s %s\n", stamp, message);
    g_free(stamp);
    g_date_time_unref(now);
    g_free(message);
}

static void
app_log_session_context(AppState *app)
{
    static const gchar *const variables[] = {
        "XDG_SESSION_TYPE",
        "DISPLAY",
        "WAYLAND_DISPLAY",
        "GDK_BACKEND",
        "GDK_GL",
        "GDK_DEBUG",
        "XDG_CURRENT_DESKTOP",
        "XDG_RUNTIME_DIR",
        "DBUS_SESSION_BUS_ADDRESS",
        "STEAM_RUNTIME",
        "STEAM_COMPAT_DATA_PATH",
        "STEAM_COMPAT_CLIENT_INSTALL_PATH",
        "STEAM_GAME",
        "STEAM_PROCESS_NAME",
        "GAMESCOPE_WAYLAND_DISPLAY",
        "GAMESCOPE_XWAYLAND_DISPLAY",
        "PIPEWIRE_REMOTE",
        "PULSE_SERVER",
        "LIBGL_ALWAYS_SOFTWARE",
        "MESA_LOADER_DRIVER_OVERRIDE",
        "VK_ICD_FILENAMES",
    };

    app_log(app, "Application log path: %s", app->log_path);
    for (guint i = 0; i < G_N_ELEMENTS(variables); i++) {
        const gchar *value = g_getenv(variables[i]);
        app_log(app, "Session environment %s=%s", variables[i],
                value != NULL ? value : "(unset)");
    }

    GdkDisplay *display = gtk_widget_get_display(app->window);
    app_log(app, "GDK display type=%s name=%s",
            display != NULL ? G_OBJECT_TYPE_NAME(display) : "(unavailable)",
            display != NULL ? gdk_display_get_name(display) : "(unavailable)");
}

static void
app_log_renderer_backend(AppState *app)
{
    const gchar *backend = capture_renderer_get_backend_name(app->renderer);
    if (g_strcmp0(app->renderer_backend_logged, backend) == 0)
        return;
    g_free(app->renderer_backend_logged);
    app->renderer_backend_logged = g_strdup(backend);
    app_log(app, "Selected video renderer: %s", backend);
}

static gboolean
get_usb_id(const GstStructure *properties, const gchar *name, guint64 *value)
{
    const GValue *v = gst_structure_get_value(properties, name);
    if (v == NULL)
        return FALSE;
    if (G_VALUE_HOLDS_STRING(v)) {
        const gchar *text = g_value_get_string(v);
        if (text == NULL)
            return FALSE;
        gchar *end = NULL;
        guint64 parsed = g_ascii_strtoull(text, &end, 0);
        if (end == text || *end != '\0')
            return FALSE;
        *value = parsed;
        return TRUE;
    }
    if (G_VALUE_HOLDS_UINT(v)) {
        *value = g_value_get_uint(v);
        return TRUE;
    }
    if (G_VALUE_HOLDS_INT(v)) {
        gint parsed = g_value_get_int(v);
        if (parsed < 0)
            return FALSE;
        *value = (guint64)parsed;
        return TRUE;
    }
    if (G_VALUE_HOLDS_UINT64(v)) {
        *value = g_value_get_uint64(v);
        return TRUE;
    }
    if (G_VALUE_HOLDS_INT64(v)) {
        gint64 parsed = g_value_get_int64(v);
        if (parsed < 0)
            return FALSE;
        *value = (guint64)parsed;
        return TRUE;
    }
    return FALSE;
}

static gchar *
audio_device_identifier(GstDevice *device)
{
    GstStructure *properties = gst_device_get_properties(device);
    const gchar *node = properties ? gst_structure_get_string(properties, "node.name") : NULL;
    const gchar *name = properties ? gst_structure_get_string(properties, "device.name") : NULL;
    const gchar *identity = node != NULL ? node : name;
    gchar *id = identity != NULL ? g_strdup(identity) : gst_device_get_display_name(device);
    if (properties != NULL)
        gst_structure_free(properties);
    return id;
}

static gboolean
is_hagibis_audio_device(GstDevice *device)
{
    gchar *klass = gst_device_get_device_class(device);
    gboolean audio_source = klass != NULL && g_str_has_prefix(klass, "Audio/Source");
    g_free(klass);
    if (!audio_source)
        return FALSE;
    GstStructure *properties = gst_device_get_properties(device);
    if (properties == NULL)
        return FALSE;
    guint64 vendor = 0, product = 0;
    gboolean match = get_usb_id(properties, "device.vendor.id", &vendor) &&
                     get_usb_id(properties, "device.product.id", &product) &&
                     vendor == 0x345f && product == 0x2130;
    gst_structure_free(properties);
    return match;
}

static GstDevice *
find_hagibis_audio_device(AppState *app)
{
    if (app->device_monitor == NULL)
        return NULL;
    GList *devices = gst_device_monitor_get_devices(app->device_monitor);
    GstDevice *found = NULL;
    for (GList *item = devices; item != NULL; item = item->next) {
        GstDevice *device = GST_DEVICE(item->data);
        if (is_hagibis_audio_device(device)) {
            found = g_object_ref(device);
            break;
        }
    }
    g_list_free_full(devices, g_object_unref);
    return found;
}

static gboolean
find_default_audio_output(AppState *app, gchar **display_name, gchar **device_id)
{
    *display_name = NULL;
    *device_id = NULL;
    if (app->device_monitor == NULL)
        return FALSE;

    GList *devices = gst_device_monitor_get_devices(app->device_monitor);
    gboolean found = FALSE;
    for (GList *item = devices; item != NULL; item = item->next) {
        GstDevice *device = GST_DEVICE(item->data);
        gchar *device_class = gst_device_get_device_class(device);
        gboolean audio_sink = g_strcmp0(device_class, "Audio/Sink") == 0;
        g_free(device_class);
        if (!audio_sink)
            continue;
        GstStructure *properties = gst_device_get_properties(device);
        gboolean is_default = FALSE;
        const gchar *id = NULL;
        const gchar *description = NULL;
        if (properties != NULL) {
            gst_structure_get_boolean(properties, "is-default", &is_default);
            id = gst_structure_get_string(properties, "node.name");
            if (id == NULL)
                id = gst_structure_get_string(properties, "device.string");
            description = gst_structure_get_string(properties, "device.description");
        }
        if (is_default) {
            *display_name = description != NULL ? g_strdup(description)
                                                : gst_device_get_display_name(device);
            *device_id = g_strdup(id);
            found = TRUE;
            if (properties != NULL)
                gst_structure_free(properties);
            break;
        }
        if (properties != NULL)
            gst_structure_free(properties);
    }
    g_list_free_full(devices, g_object_unref);
    return found;
}

static void
app_save_preferences(AppState *app)
{
    GKeyFile *key_file = g_key_file_new();
    g_key_file_set_string(key_file, "capture", "mode",
                          app->preferred_mode_key != NULL ? app->preferred_mode_key : "");
    g_key_file_set_boolean(key_file, "audio", "enabled", app->audio_enabled);
    g_key_file_set_double(key_file, "audio", "volume", app->volume);
    g_key_file_set_boolean(key_file, "ui", "panel-pinned", app->pinned);
    g_key_file_set_boolean(key_file, "ui", "stats-visible", app->stats_visible);
    g_key_file_set_integer(key_file, "ui", "dwell-ms", app->panel_dwell_ms);
    g_key_file_set_integer(key_file, "ui", "hide-delay-ms", app->panel_hide_delay_ms);
    g_key_file_set_string(key_file, "ui", "scaling-mode",
                          app->scale_mode == CAPTURE_RENDERER_FILL ? "fill" : "fit");
    gsize length = 0;
    gchar *contents = g_key_file_to_data(key_file, &length, NULL);
    gchar *directory = g_path_get_dirname(app->config_path);
    if (g_mkdir_with_parents(directory, 0700) == 0)
        g_file_set_contents(app->config_path, contents, (gssize)length, NULL);
    g_free(directory);
    g_free(contents);
    g_key_file_unref(key_file);
}

static gboolean
save_preferences_timeout(gpointer user_data)
{
    AppState *app = user_data;
    app->config_save_watch_id = 0;
    app_save_preferences(app);
    return G_SOURCE_REMOVE;
}

static void
app_schedule_preference_save(AppState *app)
{
    if (app->config_save_watch_id != 0)
        g_source_remove(app->config_save_watch_id);
    app->config_save_watch_id = g_timeout_add(250, save_preferences_timeout, app);
}

static void
save_preferred_mode(AppState *app)
{
    if (app->modes == NULL || app->current_mode >= app->modes->len)
        return;
    CaptureMode *mode = g_ptr_array_index(app->modes, app->current_mode);
    gchar *key = capture_mode_key(mode);
    g_free(app->preferred_mode_key);
    app->preferred_mode_key = key;
    app_schedule_preference_save(app);
}

static guint
choose_default_mode(AppState *app)
{
    if (app->modes == NULL || app->modes->len == 0)
        return 0;
    for (guint i = 0; i < app->modes->len; i++) {
        CaptureMode *mode = g_ptr_array_index(app->modes, i);
        if (capture_mode_equal_key(mode, app->preferred_mode_key))
            return i;
    }
    for (guint i = 0; i < app->modes->len; i++) {
        CaptureMode *mode = g_ptr_array_index(app->modes, i);
        if (mode->fourcc == V4L2_PIX_FMT_MJPEG &&
            mode->width == MODE_DEFAULT_W && mode->height == MODE_DEFAULT_H &&
            mode->fps_n == MODE_DEFAULT_FPS && mode->fps_d == 1)
            return i;
    }
    for (guint i = 0; i < app->modes->len; i++) {
        CaptureMode *mode = g_ptr_array_index(app->modes, i);
        if (mode->fourcc == V4L2_PIX_FMT_MJPEG)
            return i;
    }
    return 0;
}

static const gchar *
raw_gst_format(guint32 fourcc)
{
    switch (fourcc) {
    case V4L2_PIX_FMT_YUYV: return "YUY2";
    case V4L2_PIX_FMT_UYVY: return "UYVY";
    case V4L2_PIX_FMT_YVYU: return "YVYU";
    case V4L2_PIX_FMT_NV12: return "NV12";
    case V4L2_PIX_FMT_NV21: return "NV21";
    case V4L2_PIX_FMT_RGB24: return "RGB";
    case V4L2_PIX_FMT_BGR24: return "BGR";
    case V4L2_PIX_FMT_GREY: return "GRAY8";
    default: return NULL;
    }
}

static const gchar *
compressed_gst_caps(guint32 fourcc)
{
    if (fourcc == V4L2_PIX_FMT_MJPEG || fourcc == V4L2_PIX_FMT_JPEG)
        return "image/jpeg";
    if (fourcc == V4L2_PIX_FMT_H264)
        return "video/x-h264";
#ifdef V4L2_PIX_FMT_HEVC
    if (fourcc == V4L2_PIX_FMT_HEVC)
        return "video/x-h265";
#endif
#ifdef V4L2_PIX_FMT_VP9
    if (fourcc == V4L2_PIX_FMT_VP9)
        return "video/x-vp9";
#endif
    return NULL;
}

static GstCaps *
caps_for_mode(const CaptureMode *mode)
{
    const gchar *caps_name = mode->compressed ? compressed_gst_caps(mode->fourcc)
                                              : "video/x-raw";
    if (caps_name == NULL)
        return NULL;
    GstCaps *caps = gst_caps_new_simple(caps_name,
        "width", G_TYPE_INT, (gint)mode->width,
        "height", G_TYPE_INT, (gint)mode->height,
        "framerate", GST_TYPE_FRACTION, (gint)mode->fps_n, (gint)mode->fps_d,
        NULL);
    if (mode->fourcc == V4L2_PIX_FMT_MJPEG || mode->fourcc == V4L2_PIX_FMT_JPEG)
        gst_caps_set_simple(caps, "parsed", G_TYPE_BOOLEAN, TRUE, NULL);
    else if (!mode->compressed) {
        const gchar *format = raw_gst_format(mode->fourcc);
        if (format == NULL) {
            gst_caps_unref(caps);
            return NULL;
        }
        gst_caps_set_simple(caps, "format", G_TYPE_STRING, format, NULL);
    }
    return caps;
}

static gboolean
is_mjpeg_mode(const CaptureMode *mode)
{
    return mode->fourcc == V4L2_PIX_FMT_MJPEG || mode->fourcc == V4L2_PIX_FMT_JPEG;
}

static void
on_decode_pad_added(GstElement *decoder, GstPad *pad, gpointer user_data)
{
    AppState *app = user_data;
    GstPad *sinkpad = gst_element_get_static_pad(app->video_convert, "sink");
    if (sinkpad == NULL) {
        app_log(app, "decodebin produced a pad but video converter has no sink pad");
        return;
    }
    if (!gst_pad_is_linked(sinkpad)) {
        GstCaps *caps = gst_pad_get_current_caps(pad);
        gboolean raw_video = FALSE;
        if (caps != NULL && !gst_caps_is_empty(caps)) {
            const GstStructure *structure = gst_caps_get_structure(caps, 0);
            raw_video = g_str_has_prefix(gst_structure_get_name(structure), "video/x-raw");
        }
        if (raw_video) {
            GstPadLinkReturn linked = gst_pad_link(pad, sinkpad);
            if (linked != GST_PAD_LINK_OK)
                app_log(app, "Could not link decoded video pad: %s", gst_pad_link_get_name(linked));
        }
        if (caps != NULL)
            gst_caps_unref(caps);
    }
    gst_object_unref(sinkpad);
    (void)decoder;
}

static gboolean
setup_video_branch(AppState *app, const CaptureMode *mode, GError **error)
{
    GstElement *source = gst_element_factory_make("v4l2src", "capture-source");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "capture-mode");
    GstElement *queue = gst_element_factory_make("queue", "latest-frame-queue");
    GstElement *convert = gst_element_factory_make("videoconvert", "video-convert");
    GstElement *fps = gst_element_factory_make("fpsdisplaysink", "display-metrics");
    GstElement *sink = capture_renderer_create_sink(app->renderer);
    if (source == NULL || capsfilter == NULL || queue == NULL ||
        convert == NULL || fps == NULL || sink == NULL) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_MISSING_PLUGIN,
                    "Required video elements v4l2src/capsfilter/queue/videoconvert/fpsdisplaysink/appsink are unavailable");
        if (source) gst_object_unref(source);
        if (capsfilter) gst_object_unref(capsfilter);
        if (queue) gst_object_unref(queue);
        if (convert) gst_object_unref(convert);
        if (fps) gst_object_unref(fps);
        if (sink) gst_object_unref(sink);
        return FALSE;
    }

    GstCaps *caps = caps_for_mode(mode);
    if (caps == NULL) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
                    "Unsupported V4L2 format %s for GStreamer", mode->format_name);
        gst_object_unref(source);
        gst_object_unref(capsfilter);
        gst_object_unref(queue);
        gst_object_unref(convert);
        gst_object_unref(fps);
        gst_object_unref(sink);
        return FALSE;
    }
    g_object_set(source, "device", app->video_device->path, "io-mode", 2, NULL);
    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);
    g_object_set(queue,
        "max-size-buffers", VIDEO_QUEUE_BUFFERS,
        "max-size-bytes", 0,
        "max-size-time", (guint64)0,
        "leaky", 2,
        NULL);
    g_object_set(fps,
        "video-sink", sink,
        "text-overlay", FALSE,
        "silent", TRUE,
        "sync", FALSE,
        "fps-update-interval", 1000,
        "signal-fps-measurements", TRUE,
        NULL);

    GstElement *decoder = NULL;
    if (is_mjpeg_mode(mode))
        decoder = gst_element_factory_make("jpegdec", "mjpeg-decoder");
    else if (mode->compressed)
        decoder = gst_element_factory_make("decodebin", "video-decoder");

    if ((is_mjpeg_mode(mode) || mode->compressed) && decoder == NULL) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_MISSING_PLUGIN,
                    "No decoder is available for %s", mode->format_name);
        gst_object_unref(source);
        gst_object_unref(capsfilter);
        gst_object_unref(queue);
        gst_object_unref(convert);
        gst_object_unref(fps);
        gst_object_unref(sink);
        return FALSE;
    }

    gst_bin_add_many(GST_BIN(app->pipeline), source, capsfilter, queue, convert, NULL);
    if (decoder != NULL)
        gst_bin_add(GST_BIN(app->pipeline), decoder);
    gst_bin_add(GST_BIN(app->pipeline), fps);

    app->video_queue = queue;
    app->video_convert = convert;
    app->fps_sink = fps;
    app->video_sink = sink;

    gboolean linked = FALSE;
    if (decoder == NULL) {
        linked = gst_element_link_many(source, capsfilter, queue, convert, fps, NULL);
    } else if (is_mjpeg_mode(mode)) {
        linked = gst_element_link_many(source, capsfilter, queue, decoder, convert,
                                       fps, NULL);
    } else {
        linked = gst_element_link_many(source, capsfilter, queue, decoder, NULL) &&
                 gst_element_link(convert, fps);
        if (linked)
            g_signal_connect(decoder, "pad-added", G_CALLBACK(on_decode_pad_added), app);
    }
    if (!linked) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
                    "Could not link video elements for %s", mode->label);
        return FALSE;
    }

    g_signal_connect(fps, "fps-measurements", G_CALLBACK(on_fps_measurements), app);
    return TRUE;
}

static gboolean
setup_audio_branch(AppState *app, GError **error)
{
    if (!app->audio_enabled || app->audio_device == NULL)
        return TRUE;

    GstElement *source = gst_device_create_element(app->audio_device, "capture-audio-source");
    GstElement *queue = gst_element_factory_make("queue", "audio-bounded-queue");
    GstElement *convert = gst_element_factory_make("audioconvert", "audio-convert");
    GstElement *resample = gst_element_factory_make("audioresample", "audio-resample");
    GstElement *sink = gst_element_factory_make("pulsesink", "frame-speaker-sink");
    if (source == NULL || queue == NULL || convert == NULL || resample == NULL || sink == NULL) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_MISSING_PLUGIN,
                    "Required audio elements pulsesrc/queue/audioconvert/audioresample/pulsesink are unavailable");
        if (source) gst_object_unref(source);
        if (queue) gst_object_unref(queue);
        if (convert) gst_object_unref(convert);
        if (resample) gst_object_unref(resample);
        if (sink) gst_object_unref(sink);
        return FALSE;
    }

    g_object_set(source,
        "buffer-time", (gint64)AUDIO_BUFFER_US,
        "latency-time", (gint64)AUDIO_LATENCY_US,
        "provide-clock", FALSE,
        NULL);
    g_object_set(queue,
        "max-size-buffers", AUDIO_QUEUE_BUFFERS,
        "max-size-bytes", 0,
        "max-size-time", (guint64)AUDIO_QUEUE_NS,
        "leaky", 0,
        NULL);
    g_object_set(sink,
        "buffer-time", (gint64)AUDIO_BUFFER_US,
        "latency-time", (gint64)AUDIO_LATENCY_US,
        "sync", TRUE,
        "volume", app->volume,
        "client-name", "CaptureViewer",
        NULL);
    gst_bin_add_many(GST_BIN(app->pipeline), source, queue, convert, resample, sink, NULL);
    if (!gst_element_link_many(source, queue, convert, resample, sink, NULL)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
                    "Could not link captured HDMI audio to the default speaker sink");
        return FALSE;
    }
    app->audio_source = source;
    app->audio_sink = sink;
    return TRUE;
}


static void
pipeline_stop(AppState *app)
{
    if (app->bus_watch_id != 0) {
        g_source_remove(app->bus_watch_id);
        app->bus_watch_id = 0;
    }
    if (app->pipeline != NULL) {
        GstState state = GST_STATE_VOID_PENDING;
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        GstStateChangeReturn state_result =
            gst_element_get_state(app->pipeline, &state, NULL, GST_CLOCK_TIME_NONE);
        if (app->renderer != NULL && state_result != GST_STATE_CHANGE_FAILURE &&
            state == GST_STATE_NULL) {
            capture_renderer_pipeline_stopped(app->renderer);
        } else if (app->renderer != NULL) {
            app_log(app, "Renderer teardown deferred: GStreamer pipeline did not reach NULL");
        }
        gst_object_unref(app->pipeline);
    } else if (app->renderer != NULL) {
        capture_renderer_pipeline_stopped(app->renderer);
    }
    if (app->pipeline_bus != NULL)
        gst_object_unref(app->pipeline_bus);
    if (app->video_sink != NULL)
        gst_object_unref(app->video_sink);
    app->pipeline = NULL;
    app->pipeline_bus = NULL;
    app->video_queue = NULL;
    app->video_convert = NULL;
    app->fps_sink = NULL;
    app->video_sink = NULL;
    app->audio_source = NULL;
    app->audio_sink = NULL;
    g_free(app->audio_sink_name);
    g_free(app->audio_sink_id);
    app->audio_sink_name = NULL;
    app->audio_sink_id = NULL;
    g_mutex_lock(&app->stats_mutex);
    app->current_fps = 0.0;
    app->average_fps = 0.0;
    app->frames_dropped = 0;
    app->queue_level = 0;
    app->audio_source_latency_us = -1;
    app->audio_source_buffer_us = -1;
    app->latency_min_ns = -1;
    app->latency_max_ns = -1;
    g_mutex_unlock(&app->stats_mutex);
}


static gboolean
pipeline_bus_message(GstBus *bus, GstMessage *message, gpointer user_data)
{
    AppState *app = user_data;
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        app_log(app, "GStreamer error from %s: %s%s%s", GST_OBJECT_NAME(message->src),
                error ? error->message : "unknown error", debug ? " [" : "",
                debug ? debug : "");
        g_free(app->pipeline_error);
        app->pipeline_error = g_strdup(error ? error->message : "GStreamer pipeline error");
        if (error != NULL)
            g_error_free(error);
        g_free(debug);
        app->retry_after_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        app->bus_watch_id = 0;
        pipeline_stop(app);
        app_refresh_ui(app);
        return G_SOURCE_REMOVE;
    }
    case GST_MESSAGE_WARNING: {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_warning(message, &error, &debug);
        app_log(app, "GStreamer warning from %s: %s", GST_OBJECT_NAME(message->src),
                error ? error->message : "unknown warning");
        if (error) g_error_free(error);
        g_free(debug);
        break;
    }
    case GST_MESSAGE_LATENCY:
        gst_bin_recalculate_latency(GST_BIN(app->pipeline));
        break;
    case GST_MESSAGE_EOS:
        app_log(app, "Unexpected end-of-stream from live capture pipeline");
        app->retry_after_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        app->bus_watch_id = 0;
        pipeline_stop(app);
        app_refresh_ui(app);
        return G_SOURCE_REMOVE;
    default:
        break;
    }
    (void)bus;
    return G_SOURCE_CONTINUE;
}

static void
on_fps_measurements(GstElement *element, gdouble fps, gdouble droprate,
                    gdouble average, gpointer user_data)
{
    AppState *app = user_data;
    g_mutex_lock(&app->stats_mutex);
    app->current_fps = fps;
    app->average_fps = average;
    g_mutex_unlock(&app->stats_mutex);
    (void)element;
    (void)droprate;
}

static gboolean
pipeline_start(AppState *app)
{
    if (app->video_device == NULL || app->modes == NULL ||
        app->current_mode >= app->modes->len)
        return FALSE;
    CaptureMode *mode = g_ptr_array_index(app->modes, app->current_mode);
    GError *error = NULL;
    app->pipeline = gst_pipeline_new("captureviewer-pipeline");
    if (app->pipeline == NULL) {
        app->pipeline_error = g_strdup("Could not allocate GStreamer pipeline");
        app_refresh_ui(app);
        return FALSE;
    }

    if (!setup_video_branch(app, mode, &error) ||
        !setup_audio_branch(app, &error)) {
        app_log(app, "Pipeline setup failed: %s", error ? error->message : "unknown error");
        g_free(app->pipeline_error);
        app->pipeline_error = g_strdup(error ? error->message : "Pipeline setup failed");
        if (error) g_error_free(error);
        pipeline_stop(app);
        app_refresh_ui(app);
        return FALSE;
    }

    app->pipeline_bus = gst_element_get_bus(app->pipeline);
    app->bus_watch_id = gst_bus_add_watch(app->pipeline_bus, pipeline_bus_message, app);
    GstStateChangeReturn state = gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
    if (state == GST_STATE_CHANGE_FAILURE) {
        app_log(app, "GStreamer refused PLAYING for %s", mode->label);
        g_free(app->pipeline_error);
        app->pipeline_error = g_strdup("GStreamer refused to start capture");
        app->retry_after_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        pipeline_stop(app);
        app_refresh_ui(app);
        return FALSE;
    }

    app_log_renderer_backend(app);
    g_free(app->pipeline_error);
    app->pipeline_error = NULL;
    app_log(app, "Started %s on %s; audio %s%s", mode->label,
            app->video_device->path,
            app->audio_enabled && app->audio_device != NULL ? "enabled" : "disabled",
            app->audio_enabled && app->audio_device == NULL ? " (Hagibis source absent)" : "");
    app_refresh_ui(app);
    return TRUE;
}

static void
app_restart_pipeline(AppState *app)
{
    pipeline_stop(app);
    app->retry_after_us = 0;
    if (app->video_device != NULL && app->modes != NULL && app->modes->len != 0)
        pipeline_start(app);
    else
        app_refresh_ui(app);
}

static gboolean
same_video_device(const CaptureDevice *left, const CaptureDevice *right)
{
    return left != NULL && right != NULL &&
        g_strcmp0(left->path, right->path) == 0 &&
        g_strcmp0(left->serial, right->serial) == 0 &&
        g_strcmp0(left->usb_sysfs, right->usb_sysfs) == 0;
}

static void
replace_video_device(AppState *app, CaptureDevice *fresh)
{
    if (same_video_device(app->video_device, fresh)) {
        capture_device_free(fresh);
        return;
    }
    pipeline_stop(app);
    capture_device_free(app->video_device);
    app->video_device = fresh;
    g_free(app->pipeline_error);
    app->pipeline_error = NULL;
    if (app->modes != NULL)
        g_ptr_array_unref(app->modes);
    app->modes = NULL;
    app->current_mode = 0;
    if (fresh != NULL) {
        GError *error = NULL;
        app->modes = capture_modes_enumerate(fresh, &error);
        if (app->modes == NULL) {
            g_free(app->pipeline_error);
            app->pipeline_error = g_strdup(error ? error->message : "Could not enumerate capture modes");
            app_log(app, "Mode enumeration failed: %s", app->pipeline_error);
            if (error) g_error_free(error);
        } else {
            app->current_mode = choose_default_mode(app);
            CaptureMode *mode = g_ptr_array_index(app->modes, app->current_mode);
            app_log(app, "Found %s at %s, USB sysfs %s; %u modes, selected %s",
                    fresh->product ? fresh->product : "Hagibis", fresh->path,
                    fresh->usb_sysfs, app->modes->len, mode->label);
            for (guint i = 0; i < app->modes->len; i++) {
                CaptureMode *item = g_ptr_array_index(app->modes, i);
                gchar *key = capture_mode_key(item);
                app_log(app, "Mode %u: %s [%s]", i, item->label, key);
                g_free(key);
            }
        }
    } else {
        g_free(app->pipeline_error);
        app->pipeline_error = NULL;
        app_log(app, "Hagibis video device disconnected; waiting for reconnect");
    }
    populate_modes(app);
    app_refresh_ui(app);
}

static gboolean
update_devices(gpointer user_data)
{
    AppState *app = user_data;
    if (app->closing)
        return G_SOURCE_REMOVE;

    GError *error = NULL;
    CaptureDevice *fresh_video = capture_device_find_hagibis(&error);
    if (fresh_video != NULL) {
        app->uvc_missing = FALSE;
        replace_video_device(app, fresh_video);
    } else {
        if (app->video_device != NULL) {
            pipeline_stop(app);
            capture_device_free(app->video_device);
            app->video_device = NULL;
            if (app->modes != NULL)
                g_ptr_array_unref(app->modes);
            app->modes = NULL;
            app->current_mode = 0;
            g_free(app->pipeline_error);
            app->pipeline_error = NULL;
            app_log(app, "Hagibis video device disappeared: %s",
                    error ? error->message : "not present");
            populate_modes(app);
        }
        app->uvc_missing = capture_hagibis_usb_detected();
        if (error != NULL)
            g_error_free(error);
    }

    GstDevice *fresh_audio = find_hagibis_audio_device(app);
    gchar *fresh_audio_id = fresh_audio != NULL ? audio_device_identifier(fresh_audio) : NULL;
    gboolean audio_changed = g_strcmp0(fresh_audio_id, app->audio_device_id) != 0;
    if (audio_changed) {
        if (app->audio_enabled)
            pipeline_stop(app);
        if (app->audio_device != NULL)
            gst_object_unref(app->audio_device);
        app->audio_device = fresh_audio;
        fresh_audio = NULL;
        g_free(app->audio_device_id);
        app->audio_device_id = fresh_audio_id;
        fresh_audio_id = NULL;
        g_free(app->audio_display_name);
        app->audio_display_name = app->audio_device != NULL
            ? gst_device_get_display_name(app->audio_device) : NULL;
    }
    if (fresh_audio != NULL)
        gst_object_unref(fresh_audio);
    g_free(fresh_audio_id);

    if (app->video_device != NULL && app->modes != NULL && app->modes->len > 0 &&
        app->pipeline == NULL && g_get_monotonic_time() >= app->retry_after_us)
        pipeline_start(app);
    else if (audio_changed && app->video_device != NULL && app->pipeline == NULL &&
             app->modes != NULL && app->modes->len > 0 &&
             g_get_monotonic_time() >= app->retry_after_us)
        pipeline_start(app);

    app_refresh_ui(app);
    return G_SOURCE_CONTINUE;
}

static gboolean
rescan_devices_idle(gpointer user_data)
{
    update_devices(user_data);
    return G_SOURCE_REMOVE;
}

static gboolean
device_monitor_message(GstBus *bus, GstMessage *message, gpointer user_data)
{
    AppState *app = user_data;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_ADDED ||
        GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_REMOVED) {
        GstDevice *device = NULL;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_ADDED)
            gst_message_parse_device_added(message, &device);
        else
            gst_message_parse_device_removed(message, &device);
        if (device != NULL) {
            gchar *display_name = gst_device_get_display_name(device);
            app_log(app, "GStreamer device %s: %s",
                    GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_ADDED ? "added" : "removed",
                    display_name);
            g_free(display_name);
            gst_object_unref(device);
        }
        g_idle_add(rescan_devices_idle, app);
    }
    (void)bus;
    return G_SOURCE_CONTINUE;
}

static gboolean
stats_update(gpointer user_data)
{
    AppState *app = user_data;
    if (app->closing)
        return G_SOURCE_REMOVE;

    if (app->pipeline != NULL) {
        guint dropped = 0, queued = 0;
        if (app->fps_sink != NULL)
            g_object_get(app->fps_sink, "frames-dropped", &dropped, NULL);
        if (app->video_queue != NULL)
            g_object_get(app->video_queue, "current-level-buffers", &queued, NULL);
        gint64 source_latency = -1, source_buffer = -1;
        if (app->audio_source != NULL) {
            g_object_get(app->audio_source, "actual-latency-time", &source_latency,
                         "actual-buffer-time", &source_buffer, NULL);
        }
        gchar *sink_name = NULL;
        gchar *sink_id = NULL;
        find_default_audio_output(app, &sink_name, &sink_id);
        if (g_strcmp0(app->audio_sink_name, sink_name) != 0 ||
            g_strcmp0(app->audio_sink_id, sink_id) != 0) {
            g_free(app->audio_sink_name);
            g_free(app->audio_sink_id);
            app->audio_sink_name = g_strdup(sink_name);
            app->audio_sink_id = g_strdup(sink_id);
            if (sink_name != NULL || sink_id != NULL)
                app_log(app, "PulseAudio default playback output: name=%s id=%s",
                        sink_name != NULL ? sink_name : "(unavailable)",
                        sink_id != NULL ? sink_id : "(unavailable)");
        }
        g_free(sink_name);
        g_free(sink_id);
        GstQuery *query = gst_query_new_latency();
        gint64 min_latency = -1, max_latency = -1;
        gboolean live = FALSE;
        if (gst_element_query(app->pipeline, query)) {
            GstClockTime min = GST_CLOCK_TIME_NONE, max = GST_CLOCK_TIME_NONE;
            gst_query_parse_latency(query, &live, &min, &max);
            min_latency = GST_CLOCK_TIME_IS_VALID(min) ? (gint64)min : -1;
            max_latency = GST_CLOCK_TIME_IS_VALID(max) ? (gint64)max : -1;
        }
        gst_query_unref(query);

        gint64 cpu_time = process_cpu_time_us();
        gint64 wall_time = g_get_monotonic_time();
        if (app->last_cpu_time_us > 0 && wall_time > app->last_cpu_time_us) {
            app->cpu_percent = (gdouble)(cpu_time - app->last_cpu_us) * 100.0 /
                               (wall_time - app->last_cpu_time_us);
        }
        app->last_cpu_us = cpu_time;
        app->last_cpu_time_us = wall_time;
        g_mutex_lock(&app->stats_mutex);
        app->frames_dropped = dropped;
        app->queue_level = queued;
        app->audio_source_latency_us = source_latency;
        app->audio_source_buffer_us = source_buffer;
        app->latency_min_ns = min_latency;
        app->latency_max_ns = max_latency;
        g_mutex_unlock(&app->stats_mutex);
    }
    app_log_renderer_backend(app);
    app_refresh_ui(app);
    return G_SOURCE_CONTINUE;
}

static void
app_refresh_ui(AppState *app)
{
    if (app->window == NULL)
        return;
    gchar *device_text;
    if (app->video_device != NULL) {
        device_text = g_strdup_printf("Connected: %s — %s — %s — %u modes\nUSB %s · serial %s · interface %s%s",
            app->video_device->product ? app->video_device->product : "Hagibis",
            app->video_device->manufacturer ? app->video_device->manufacturer : "MACROSILICON",
            app->video_device->path, app->modes ? app->modes->len : 0,
            app->video_device->usb_sysfs ? app->video_device->usb_sysfs : "unknown",
            app->video_device->serial ? app->video_device->serial : "unavailable",
            app->video_device->interface_sysfs ? app->video_device->interface_sysfs : "unknown",
            app->pipeline_error != NULL ? " · capture error" : "");
    } else if (app->uvc_missing) {
        device_text = g_strdup("Hagibis USB capture device detected, but no UVC video interface is available. Check that the UVC driver is bound and video-device access is available.");
    } else {
        device_text = g_strdup("Waiting for Hagibis HDMI capture card (USB 345f:2130)");
    }

    if (app->device_label != NULL)
        gtk_label_set_text(GTK_LABEL(app->device_label), device_text);
    if (app->audio_toggle != NULL) {
        gtk_widget_set_sensitive(app->audio_toggle, app->audio_device != NULL);
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->audio_toggle)) != app->audio_enabled) {
            app->updating_controls = TRUE;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->audio_toggle), app->audio_enabled);
            app->updating_controls = FALSE;
        }
    }
    if (app->volume_scale != NULL)
        gtk_widget_set_sensitive(app->volume_scale, app->audio_enabled && app->audio_device != NULL);

    gchar *mode_text = NULL;
    gchar *overlay_text = NULL;
    if (app->modes != NULL && app->current_mode < app->modes->len) {
        CaptureMode *mode = g_ptr_array_index(app->modes, app->current_mode);
        mode_text = g_strdup(mode->label);
    } else {
        mode_text = g_strdup("No capture mode");
    }

    g_mutex_lock(&app->stats_mutex);
    gchar *latency_text = app->latency_min_ns >= 0
        ? (app->latency_max_ns >= 0
            ? g_strdup_printf("Pipeline-reported latency %.1f–%.1f ms (not end-to-end)",
                app->latency_min_ns / 1000000.0, app->latency_max_ns / 1000000.0)
            : g_strdup_printf("Pipeline-reported latency ≥ %.1f ms (not end-to-end)",
                app->latency_min_ns / 1000000.0))
        : g_strdup("Pipeline-reported latency unavailable (not end-to-end)");
    gchar *perf = g_strdup_printf("FPS %.1f avg %.1f · dropped %" G_GUINT64_FORMAT
        " · queue %" G_GUINT64_FORMAT " · CPU %.1f%% · %s",
        app->current_fps, app->average_fps, app->frames_dropped,
        app->queue_level, app->cpu_percent, latency_text);
    if (app->stats_overlay_label != NULL) {
        gchar *overlay_latency = app->latency_min_ns >= 0
            ? (app->latency_max_ns >= 0
                ? g_strdup_printf("Pipeline-reported latency %.0f–%.0f ms (not end-to-end)",
                    app->latency_min_ns / 1000000.0,
                    app->latency_max_ns / 1000000.0)
                : g_strdup_printf("Pipeline-reported latency ≥ %.0f ms (not end-to-end)",
                    app->latency_min_ns / 1000000.0))
            : g_strdup("Pipeline-reported latency unavailable");
        overlay_text = g_strdup_printf(
            "%s\nFPS %.1f avg %.1f · dropped %" G_GUINT64_FORMAT
            " · CPU %.1f%% · %s",
            mode_text, app->current_fps, app->average_fps,
            app->frames_dropped, app->cpu_percent, overlay_latency);
        g_free(overlay_latency);
    }
    gchar *audio_stats;
    if (app->audio_device != NULL && app->audio_enabled && app->audio_source != NULL) {
        audio_stats = app->audio_source_latency_us >= 0 && app->audio_source_buffer_us >= 0
            ? g_strdup_printf(" · audio source latency %.1f ms / buffer %.1f ms",
                app->audio_source_latency_us / 1000.0,
                app->audio_source_buffer_us / 1000.0)
            : g_strdup(" · audio source timing unavailable");
    } else {
        audio_stats = g_strdup("");
    }
    g_mutex_unlock(&app->stats_mutex);

    gchar *sink_display = app->audio_sink_name != NULL && *app->audio_sink_name != '\0'
        ? (app->audio_sink_id != NULL && *app->audio_sink_id != '\0'
            ? g_strdup_printf("%s (%s)", app->audio_sink_name, app->audio_sink_id)
            : g_strdup(app->audio_sink_name))
        : (app->audio_sink_id != NULL && *app->audio_sink_id != '\0'
            ? g_strdup(app->audio_sink_id) : NULL);
    gchar *audio_route;
    if (!app->audio_enabled)
        audio_route = g_strdup("off");
    else if (app->audio_device == NULL)
        audio_route = g_strdup("waiting for Hagibis HDMI audio source");
    else if (app->audio_source == NULL)
        audio_route = g_strdup_printf("%s detected; playback inactive (capture pipeline not running)",
            app->audio_display_name != NULL ? app->audio_display_name : "Hagibis HDMI audio");
    else if (sink_display != NULL)
        audio_route = g_strdup_printf("%s → PulseAudio default output: %s",
            app->audio_display_name != NULL ? app->audio_display_name : "Hagibis HDMI audio",
            sink_display);
    else
        audio_route = g_strdup_printf("%s → PulseAudio default output unavailable",
            app->audio_display_name != NULL ? app->audio_display_name : "Hagibis HDMI audio");
    gchar *status = g_strdup_printf("%s\nMode: %s\nAudio: %s%s\n%s%s",
        device_text, mode_text,
        audio_route, audio_stats, perf,
        app->pipeline_error != NULL ? "\nPipeline: " : "");
    if (app->pipeline_error != NULL) {
        gchar *with_error = g_strconcat(status, app->pipeline_error, NULL);
        g_free(status);
        status = with_error;
    }
    if (app->status_label != NULL)
        gtk_label_set_text(GTK_LABEL(app->status_label), status);
    if (app->status_overlay_label != NULL && app->status_overlay != NULL) {
        const gchar *message = NULL;
        if (app->uvc_missing)
            message = "Hagibis capture device detected\nNo UVC video interface is available. Check that UVC support is installed and the driver is bound.";
        else if (app->video_device == NULL)
            message = "Waiting for Hagibis HDMI capture device";
        else if (app->pipeline_error != NULL)
            message = "Capture error\nOpen Advanced for diagnostics.";
        else if (app->pipeline == NULL)
            message = "Capture device connected\nWaiting for capture to start.";
        gboolean show_status = message != NULL;
        if (show_status &&
            g_strcmp0(gtk_label_get_text(GTK_LABEL(app->status_overlay_label)), message) != 0)
            gtk_label_set_text(GTK_LABEL(app->status_overlay_label), message);
        if (show_status && !gtk_widget_get_visible(app->status_overlay))
            gtk_widget_show(app->status_overlay);
        else if (!show_status && gtk_widget_get_visible(app->status_overlay))
            gtk_widget_hide(app->status_overlay);
    }

    if (app->perf_label != NULL)
        gtk_label_set_text(GTK_LABEL(app->perf_label), perf);
    if (app->stats_overlay_label != NULL) {
        gtk_label_set_text(GTK_LABEL(app->stats_overlay_label), overlay_text);
    }

    g_free(device_text);
    g_free(mode_text);
    g_free(perf);
    g_free(audio_stats);
    g_free(overlay_text);
    g_free(sink_display);
    g_free(audio_route);
    g_free(latency_text);
    g_free(status);
}

static void
mode_changed(GtkComboBox *combo, gpointer user_data)
{
    AppState *app = user_data;
    if (app->updating_controls || app->modes == NULL)
        return;
    gint active = gtk_combo_box_get_active(combo);
    if (active < 0 || (guint)active >= app->modes->len || (guint)active == app->current_mode)
        return;
    app->current_mode = (guint)active;
    save_preferred_mode(app);
    CaptureMode *mode = g_ptr_array_index(app->modes, app->current_mode);
    app_log(app, "Selected capture mode %s", mode->label);
    app_restart_pipeline(app);
}

static void
audio_toggled(GtkToggleButton *button, gpointer user_data)
{
    AppState *app = user_data;
    if (app->updating_controls)
        return;
    app->audio_enabled = gtk_toggle_button_get_active(button);
    app_schedule_preference_save(app);
    app_log(app, "HDMI audio %s", app->audio_enabled ? "enabled" : "disabled");
    app_restart_pipeline(app);
}

static void
scaling_mode_changed(GtkComboBox *combo, gpointer user_data)
{
    AppState *app = user_data;
    if (app->updating_controls)
        return;

    const gchar *mode_id = gtk_combo_box_get_active_id(combo);
    CaptureRendererScaleMode mode =
        g_strcmp0(mode_id, "fill") == 0 ? CAPTURE_RENDERER_FILL
                                        : CAPTURE_RENDERER_FIT;
    if (mode == app->scale_mode)
        return;

    app->scale_mode = mode;
    capture_renderer_set_scale_mode(app->renderer, mode);
    app_schedule_preference_save(app);
    app_log(app, "Video scaling mode set to %s",
            mode == CAPTURE_RENDERER_FILL ? "fill" : "fit");
}

static void
volume_changed(GtkRange *range, gpointer user_data)
{
    AppState *app = user_data;
    app->volume = gtk_range_get_value(range);
    if (app->audio_sink != NULL)
        g_object_set(app->audio_sink, "volume", app->volume, NULL);
    app_schedule_preference_save(app);
}

static gboolean
interaction_holds_panel(AppState *app)
{
    GtkWidget *grab = gtk_grab_get_current();
    return app->pinned || app->panel_pointer_inside || app->edge_hotspot_inside ||
        app->settings_open || app->mode_popup_open || app->interaction_active ||
        (grab != NULL && grab != app->window) ||
        g_get_monotonic_time() < app->keyboard_active_until_us;
}

static gboolean
panel_hide_timeout(gpointer user_data)
{
    AppState *app = user_data;
    app->panel_hide_watch_id = 0;
    if (interaction_holds_panel(app))
        app_schedule_panel_hide(app);
    else
        app_hide_control_panel(app);
    return G_SOURCE_REMOVE;
}

static gboolean
panel_dwell_timeout(gpointer user_data)
{
    AppState *app = user_data;
    app->dwell_watch_id = 0;
    if (app->edge_hotspot_inside)
        app_show_control_panel(app);
    return G_SOURCE_REMOVE;
}

static void
app_schedule_panel_hide(AppState *app)
{
    if (app->panel_hide_watch_id != 0)
        g_source_remove(app->panel_hide_watch_id);
    if (!app->panel_visible)
        return;
    app->panel_hide_watch_id = g_timeout_add(app->panel_hide_delay_ms,
                                             panel_hide_timeout, app);
}

static void
app_show_control_panel(AppState *app)
{
    if (app->dwell_watch_id != 0) {
        g_source_remove(app->dwell_watch_id);
        app->dwell_watch_id = 0;
    }
    if (app->panel_hide_watch_id != 0) {
        g_source_remove(app->panel_hide_watch_id);
        app->panel_hide_watch_id = 0;
    }
    app->panel_visible = TRUE;
    if (app->control_panel != NULL)
        gtk_widget_show(app->control_panel);
}

static void
app_hide_control_panel(AppState *app)
{
    if (app->pinned || app->panel_pointer_inside || app->edge_hotspot_inside ||
        app->settings_open || app->mode_popup_open || app->interaction_active)
        return;
    app->panel_visible = FALSE;
    if (app->control_panel != NULL)
        gtk_widget_hide(app->control_panel);
}

static gboolean
window_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
    AppState *app = user_data;
    gboolean at_edge = event->y <= 8.0;
    if (at_edge != app->edge_hotspot_inside) {
        app->edge_hotspot_inside = at_edge;
        if (at_edge) {
            if (!app->panel_visible && app->dwell_watch_id == 0)
                app->dwell_watch_id = g_timeout_add(app->panel_dwell_ms,
                                                    panel_dwell_timeout, app);
            else
                app_show_control_panel(app);
        } else {
            if (app->dwell_watch_id != 0) {
                g_source_remove(app->dwell_watch_id);
                app->dwell_watch_id = 0;
            }
            app_schedule_panel_hide(app);
        }
    }
    (void)widget;
    return FALSE;
}

static gboolean
panel_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
    AppState *app = user_data;
    app->panel_pointer_inside = TRUE;
    if (app->panel_hide_watch_id != 0) {
        g_source_remove(app->panel_hide_watch_id);
        app->panel_hide_watch_id = 0;
    }
    app_show_control_panel(app);
    (void)widget;
    (void)event;
    return FALSE;
}

static gboolean
panel_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
    AppState *app = user_data;
    if (event->detail != GDK_NOTIFY_INFERIOR) {
        app->panel_pointer_inside = FALSE;
        app_schedule_panel_hide(app);
    }
    (void)widget;
    (void)event;
    return FALSE;
}

static void
mode_popup_notify(GObject *object, GParamSpec *spec, gpointer user_data)
{
    AppState *app = user_data;
    g_object_get(object, "popup-shown", &app->mode_popup_open, NULL);
    if (app->mode_popup_open)
        app_show_control_panel(app);
    else
        app_schedule_panel_hide(app);
    (void)spec;
}

static void
toggle_fullscreen(AppState *app)
{
    if (app->fullscreen) {
        gtk_window_unfullscreen(GTK_WINDOW(app->window));
        app->fullscreen = FALSE;
    } else {
        gtk_window_fullscreen(GTK_WINDOW(app->window));
        app->fullscreen = TRUE;
    }
    gtk_window_present(GTK_WINDOW(app->window));
}

static void
show_settings(AppState *app)
{
    if (app->settings != NULL) {
        app->settings_open = TRUE;
        app_show_control_panel(app);
        gtk_widget_show_all(app->settings);
        gtk_window_present(GTK_WINDOW(app->settings));
    }
}

static void
hide_settings(AppState *app)
{
    app->settings_open = FALSE;
    if (app->settings != NULL)
        gtk_widget_hide(app->settings);
    app_schedule_panel_hide(app);
}

static void
toggle_control_panel(AppState *app)
{
    if (app->panel_visible) {
        app->keyboard_active_until_us = 0;
        if (app->panel_hide_watch_id != 0) {
            g_source_remove(app->panel_hide_watch_id);
            app->panel_hide_watch_id = 0;
        }
        app->panel_visible = FALSE;
        gtk_widget_hide(app->control_panel);
    } else {
        app->keyboard_active_until_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        app_show_control_panel(app);
        app_schedule_panel_hide(app);
    }
}


static gboolean
key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    AppState *app = user_data;
    switch (event->keyval) {
    case GDK_KEY_Escape:
        app->keyboard_active_until_us = 0;
        if (app->settings_open) {
            hide_settings(app);
            return TRUE;
        }
        if (app->mode_popup_open && app->mode_combo != NULL) {
            gtk_combo_box_popdown(GTK_COMBO_BOX(app->mode_combo));
            app->mode_popup_open = FALSE;
            return TRUE;
        }
        if (app->fullscreen)
            toggle_fullscreen(app);
        if (!app->pinned) {
            if (app->panel_hide_watch_id != 0) {
                g_source_remove(app->panel_hide_watch_id);
                app->panel_hide_watch_id = 0;
            }
            app->panel_visible = FALSE;
            if (app->control_panel != NULL)
                gtk_widget_hide(app->control_panel);
        }
        return TRUE;
    case GDK_KEY_F11:
        app->keyboard_active_until_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        app_show_control_panel(app);
        toggle_fullscreen(app);
        app_schedule_panel_hide(app);
        return TRUE;
    case GDK_KEY_s:
    case GDK_KEY_S:
        toggle_control_panel(app);
        return TRUE;
    case GDK_KEY_q:
    case GDK_KEY_Q:
        g_application_quit(G_APPLICATION(app->application));
        return TRUE;
    default:
        app->keyboard_active_until_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        if (!app->settings_open) {
            app_show_control_panel(app);
            app_schedule_panel_hide(app);
        }
        (void)widget;
        return FALSE;
    }
}

static gboolean
close_settings(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    hide_settings(user_data);
    (void)widget;
    (void)event;
    return TRUE;
}

static void
pin_toggled(GtkToggleButton *button, gpointer user_data)
{
    AppState *app = user_data;
    app->pinned = gtk_toggle_button_get_active(button);
    gtk_button_set_label(GTK_BUTTON(button), app->pinned ? "Pinned" : "Pin");
    app_schedule_preference_save(app);
    if (app->pinned)
        app_show_control_panel(app);
    else
        app_schedule_panel_hide(app);
}

static void
stats_toggled(GtkToggleButton *button, gpointer user_data)
{
    AppState *app = user_data;
    app->stats_visible = gtk_toggle_button_get_active(button);
    if (app->stats_visible)
        gtk_widget_show(app->stats_overlay_box);
    else
        gtk_widget_hide(app->stats_overlay_box);
    app_schedule_preference_save(app);
}

static void
dwell_changed(GtkSpinButton *spin, gpointer user_data)
{
    AppState *app = user_data;
    app->panel_dwell_ms = (guint)gtk_spin_button_get_value_as_int(spin);
    app_schedule_preference_save(app);
}

static void
hide_delay_changed(GtkSpinButton *spin, gpointer user_data)
{
    AppState *app = user_data;
    app->panel_hide_delay_ms = (guint)gtk_spin_button_get_value_as_int(spin);
    app_schedule_preference_save(app);
}

static gboolean
slider_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    AppState *app = user_data;
    app->interaction_active = TRUE;
    app_show_control_panel(app);
    (void)widget;
    (void)event;
    return FALSE;
}

static gboolean
slider_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    AppState *app = user_data;
    app->interaction_active = FALSE;
    app_schedule_preference_save(app);
    app_schedule_panel_hide(app);
    (void)widget;
    (void)event;
    return FALSE;
}

static void
create_settings(AppState *app)
{
    app->settings = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(app->settings), "Advanced Capture Diagnostics");
    gtk_window_set_transient_for(GTK_WINDOW(app->settings), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(app->settings), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(app->settings), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(app->settings), 620, 420);
    g_signal_connect(app->settings, "delete-event", G_CALLBACK(close_settings), app);
    g_signal_connect(app->settings, "key-press-event", G_CALLBACK(key_press), app);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(app->settings));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 24);
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);
    app->device_label = gtk_label_new("Searching for capture device…");
    gtk_label_set_xalign(GTK_LABEL(app->device_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(app->device_label), TRUE);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Device"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->device_label, 1, 0, 2, 1);
    app->perf_label = gtk_label_new("Waiting for capture");
    gtk_label_set_xalign(GTK_LABEL(app->perf_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(app->perf_label), TRUE);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Capture statistics"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->perf_label, 1, 1, 2, 1);
    app->status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->status_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(app->status_label), TRUE);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Status and route"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->status_label, 1, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Show controls after"), 0, 3, 1, 1);
    app->dwell_spin = gtk_spin_button_new_with_range(100, 250, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->dwell_spin), app->panel_dwell_ms);
    gtk_grid_attach(GTK_GRID(grid), app->dwell_spin, 1, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("ms pointer dwell"), 2, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Hide controls after"), 0, 4, 1, 1);
    app->hide_delay_spin = gtk_spin_button_new_with_range(500, 1000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->hide_delay_spin), app->panel_hide_delay_ms);
    gtk_grid_attach(GTK_GRID(grid), app->hide_delay_spin, 1, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("ms outside panel"), 2, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Video scaling"), 0, 5, 1, 1);
    app->scale_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->scale_combo),
                              "fit", "Fit entire frame");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->scale_combo),
                              "fill", "Fill screen, crop edges");
    app->updating_controls = TRUE;
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->scale_combo),
                                app->scale_mode == CAPTURE_RENDERER_FILL ? "fill" : "fit");
    app->updating_controls = FALSE;
    gtk_widget_set_size_request(app->scale_combo, 260, 48);
    gtk_grid_attach(GTK_GRID(grid), app->scale_combo, 1, 5, 2, 1);
    g_signal_connect(app->scale_combo, "changed",
                     G_CALLBACK(scaling_mode_changed), app);
    g_signal_connect(app->dwell_spin, "value-changed", G_CALLBACK(dwell_changed), app);
    g_signal_connect(app->hide_delay_spin, "value-changed", G_CALLBACK(hide_delay_changed), app);
    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(close, 120, 48);
    gtk_box_pack_end(GTK_BOX(content), close, FALSE, FALSE, 16);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(hide_settings), app);
}

static void
create_control_panel(AppState *app)
{
    GtkWidget *panel = gtk_event_box_new();
    app->control_panel = panel;
    gtk_widget_add_events(panel, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(panel, "enter-notify-event", G_CALLBACK(panel_enter), app);
    g_signal_connect(panel, "leave-notify-event", G_CALLBACK(panel_leave), app);
    gtk_widget_set_halign(panel, GTK_ALIGN_FILL);
    gtk_widget_set_valign(panel, GTK_ALIGN_START);
    gtk_widget_set_margin_start(panel, 18);
    gtk_widget_set_margin_end(panel, 18);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(panel), box);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "eventbox { background-color: rgba(12, 15, 20, 0.88); border-radius: 12px; } "
        "button, combobox, checkbutton { min-height: 44px; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(panel),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    GtkWidget *pin = gtk_toggle_button_new_with_label("Pin");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pin), app->pinned);
    gtk_button_set_label(GTK_BUTTON(pin), app->pinned ? "Pinned" : "Pin");
    gtk_widget_set_size_request(pin, 72, 48);
    gtk_box_pack_start(GTK_BOX(box), pin, FALSE, FALSE, 0);
    g_signal_connect(pin, "toggled", G_CALLBACK(pin_toggled), app);
    app->mode_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(app->mode_combo, 270, 48);
    gtk_box_pack_start(GTK_BOX(box), app->mode_combo, FALSE, FALSE, 0);
    g_signal_connect(app->mode_combo, "changed", G_CALLBACK(mode_changed), app);
    g_signal_connect(app->mode_combo, "notify::popup-shown", G_CALLBACK(mode_popup_notify), app);
    app->audio_toggle = gtk_check_button_new_with_label("HDMI audio");
    gtk_widget_set_size_request(app->audio_toggle, 140, 48);
    gtk_box_pack_start(GTK_BOX(box), app->audio_toggle, FALSE, FALSE, 0);
    g_signal_connect(app->audio_toggle, "toggled", G_CALLBACK(audio_toggled), app);
    GtkWidget *volume_label = gtk_label_new("HDMI volume");
    gtk_box_pack_start(GTK_BOX(box), volume_label, FALSE, FALSE, 0);
    app->volume_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
    gtk_widget_set_size_request(app->volume_scale, 150, 48);
    gtk_range_set_value(GTK_RANGE(app->volume_scale), app->volume);
    gtk_scale_set_draw_value(GTK_SCALE(app->volume_scale), TRUE);
    gtk_box_pack_start(GTK_BOX(box), app->volume_scale, FALSE, FALSE, 0);
    gtk_widget_add_events(app->volume_scale, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(app->volume_scale, "button-press-event", G_CALLBACK(slider_press), app);
    g_signal_connect(app->volume_scale, "button-release-event", G_CALLBACK(slider_release), app);
    g_signal_connect(app->volume_scale, "value-changed", G_CALLBACK(volume_changed), app);
    GtkWidget *stats = gtk_toggle_button_new_with_label("Stats");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(stats), app->stats_visible);
    gtk_widget_set_size_request(stats, 88, 48);
    gtk_box_pack_start(GTK_BOX(box), stats, FALSE, FALSE, 0);
    g_signal_connect(stats, "toggled", G_CALLBACK(stats_toggled), app);
    GtkWidget *advanced = gtk_button_new_with_label("Advanced");
    gtk_widget_set_size_request(advanced, 112, 48);
    gtk_box_pack_start(GTK_BOX(box), advanced, FALSE, FALSE, 0);
    g_signal_connect_swapped(advanced, "clicked", G_CALLBACK(show_settings), app);
    GtkWidget *full = gtk_button_new_with_label("Fullscreen");
    gtk_widget_set_size_request(full, 120, 48);
    gtk_box_pack_start(GTK_BOX(box), full, FALSE, FALSE, 0);
    g_signal_connect_swapped(full, "clicked", G_CALLBACK(toggle_fullscreen), app);
    GtkWidget *quit = gtk_button_new_with_label("Quit");
    gtk_widget_set_size_request(quit, 76, 48);
    gtk_box_pack_start(GTK_BOX(box), quit, FALSE, FALSE, 0);
    g_signal_connect_swapped(quit, "clicked", G_CALLBACK(g_application_quit), app->application);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->root_overlay), panel);
    gtk_widget_set_halign(panel, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(panel, GTK_ALIGN_START);
    app->stats_overlay_box = gtk_event_box_new();
    gtk_widget_set_halign(app->stats_overlay_box, GTK_ALIGN_START);
    gtk_widget_set_valign(app->stats_overlay_box, GTK_ALIGN_END);
    gtk_widget_set_hexpand(app->stats_overlay_box, FALSE);
    gtk_widget_set_vexpand(app->stats_overlay_box, FALSE);
    gtk_widget_set_margin_start(app->stats_overlay_box, 22);
    gtk_widget_set_margin_bottom(app->stats_overlay_box, 22);
    app->stats_overlay_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->stats_overlay_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(app->stats_overlay_label), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(app->stats_overlay_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app->stats_overlay_label), 110);
    gtk_widget_set_hexpand(app->stats_overlay_label, FALSE);
    gtk_widget_set_vexpand(app->stats_overlay_label, FALSE);
    gtk_container_add(GTK_CONTAINER(app->stats_overlay_box), app->stats_overlay_label);
    gtk_widget_show(app->stats_overlay_label);
    gtk_widget_set_name(app->stats_overlay_box, "stats-overlay");
    GtkCssProvider *stats_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(stats_css,
        "#stats-overlay { background-color: rgba(12, 15, 20, 0.78); "
        "padding: 12px; border-radius: 8px; } "
        "#stats-overlay label { color: white; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(app->stats_overlay_box),
        GTK_STYLE_PROVIDER(stats_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(stats_css);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->root_overlay), app->stats_overlay_box);
    gtk_widget_set_no_show_all(app->stats_overlay_box, TRUE);
    if (app->stats_visible)
        gtk_widget_show(app->stats_overlay_box);
    else
        gtk_widget_hide(app->stats_overlay_box);

    app->status_overlay = gtk_event_box_new();
    gtk_widget_set_name(app->status_overlay, "capture-status");
    gtk_widget_set_halign(app->status_overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app->status_overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(app->status_overlay, 680, -1);
    app->status_overlay_label = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(app->status_overlay_label), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(app->status_overlay_label), TRUE);
    gtk_container_add(GTK_CONTAINER(app->status_overlay), app->status_overlay_label);
    gtk_widget_show(app->status_overlay_label);
    GtkCssProvider *status_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(status_css,
        "#capture-status { background-color: rgba(12, 15, 20, 0.90); "
        "border-radius: 12px; padding: 22px; } "
        "#capture-status label { color: white; font-size: 20px; }",
        -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(app->status_overlay),
        GTK_STYLE_PROVIDER(status_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(status_css);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->root_overlay), app->status_overlay);
    gtk_widget_set_no_show_all(app->status_overlay, TRUE);
    gtk_widget_hide(app->status_overlay);
}

static void
populate_modes(AppState *app)
{
    if (app->mode_combo == NULL)
        return;
    app->updating_controls = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->mode_combo));
    if (app->modes != NULL) {
        for (guint i = 0; i < app->modes->len; i++) {
            CaptureMode *mode = g_ptr_array_index(app->modes, i);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->mode_combo), mode->label);
        }
        if (app->modes->len > 0)
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->mode_combo), (gint)app->current_mode);
    }
    gtk_widget_set_sensitive(app->mode_combo, app->modes != NULL && app->modes->len > 0);
    app->updating_controls = FALSE;
}


static void
app_activate(GtkApplication *application, gpointer user_data)
{
    AppState *app = user_data;
    app->application = application;
    if (app->window != NULL) {
        gtk_window_present(GTK_WINDOW(app->window));
        return;
    }
    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "CaptureViewer");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1280, 720);
    gtk_widget_set_can_focus(app->window, TRUE);
    gtk_widget_set_app_paintable(app->window, TRUE);
    g_signal_connect(app->window, "key-press-event", G_CALLBACK(key_press), app);
    GError *renderer_error = NULL;
    app->renderer = capture_renderer_new(&renderer_error);
    if (app->renderer == NULL) {
        app_log(app, "Could not create video renderer: %s",
                renderer_error != NULL ? renderer_error->message : "unknown error");
        g_clear_error(&renderer_error);
        g_application_quit(G_APPLICATION(application));
        return;
    }
    capture_renderer_set_scale_mode(app->renderer, app->scale_mode);
    capture_renderer_connect_motion_events(app->renderer, window_motion, app);
    app->root_overlay = gtk_overlay_new();
    app->video_area = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(app->video_area), FALSE);
    gtk_widget_set_hexpand(app->video_area, TRUE);
    gtk_widget_set_vexpand(app->video_area, TRUE);
    gtk_container_add(GTK_CONTAINER(app->video_area),
                      capture_renderer_get_widget(app->renderer));
    gtk_container_add(GTK_CONTAINER(app->root_overlay), app->video_area);
    gtk_container_add(GTK_CONTAINER(app->window), app->root_overlay);
    create_control_panel(app);
    create_settings(app);
    gtk_widget_show_all(app->window);
    gtk_widget_hide(app->settings);
    gtk_widget_hide(app->control_panel);
    if (app->pinned)
        app_show_control_panel(app);
    gtk_widget_realize(app->window);
    gtk_widget_grab_focus(app->window);
    gtk_window_fullscreen(GTK_WINDOW(app->window));
    gtk_window_present(GTK_WINDOW(app->window));
    app_log_session_context(app);
    app_log_renderer_backend(app);
    app->fullscreen = TRUE;
    app->device_monitor = gst_device_monitor_new();
    GstCaps *audio_caps = gst_caps_new_empty_simple("audio/x-raw");
    gst_device_monitor_add_filter(app->device_monitor, "Audio/Source", audio_caps);
    gst_device_monitor_add_filter(app->device_monitor, "Audio/Sink", audio_caps);
    gst_caps_unref(audio_caps);
    if (gst_device_monitor_start(app->device_monitor)) {
        GstBus *bus = gst_device_monitor_get_bus(app->device_monitor);
        app->device_watch_id = gst_bus_add_watch(bus, device_monitor_message, app);
        g_source_set_name_by_id(app->device_watch_id, "captureviewer-device-monitor");
        gst_object_unref(bus);
    } else {
        app_log(app, "GStreamer device monitor could not start; periodic sysfs scan remains active");
    }

    populate_modes(app);
    update_devices(app);
    app->monitor_watch_id = g_timeout_add(DEVICE_RESCAN_MS, update_devices, app);
    app->stats_watch_id = g_timeout_add(333, stats_update, app);
    app_log(app, "Viewer started; S toggles controls, F11 toggles fullscreen, Q quits");
    (void)user_data;
}

static void
app_shutdown(GApplication *application, gpointer user_data)
{
    AppState *app = user_data;
    app->closing = TRUE;
    if (app->dwell_watch_id != 0)
        g_source_remove(app->dwell_watch_id);
    if (app->panel_hide_watch_id != 0)
        g_source_remove(app->panel_hide_watch_id);
    if (app->config_save_watch_id != 0)
        g_source_remove(app->config_save_watch_id);
    app_save_preferences(app);
    if (app->monitor_watch_id != 0)
        g_source_remove(app->monitor_watch_id);
    if (app->stats_watch_id != 0)
        g_source_remove(app->stats_watch_id);
    if (app->device_watch_id != 0)
        g_source_remove(app->device_watch_id);
    pipeline_stop(app);
    if (app->renderer != NULL) {
        capture_renderer_free(app->renderer);
        app->renderer = NULL;
    }
    if (app->device_monitor != NULL) {
        gst_device_monitor_stop(app->device_monitor);
        gst_object_unref(app->device_monitor);
    }
    if (app->audio_device != NULL)
        gst_object_unref(app->audio_device);
    if (app->modes != NULL)
        g_ptr_array_unref(app->modes);
    capture_device_free(app->video_device);
    g_free(app->audio_device_id);
    g_free(app->audio_display_name);
    g_free(app->audio_sink_name);
    g_free(app->audio_sink_id);
    g_free(app->preferred_mode_key);
    g_free(app->config_path);
    g_free(app->pipeline_error);
    g_free(app->renderer_backend_logged);
    app_log(app, "Viewer shut down cleanly");
    g_free(app->log_path);
    g_mutex_clear(&app->stats_mutex);
    g_mutex_clear(&app->log_mutex);
    (void)application;
}

static gint
list_modes(void)
{
    GError *error = NULL;
    CaptureDevice *device = capture_device_find_hagibis(&error);
    if (device == NULL) {
        g_printerr("%s\n", error ? error->message : "Hagibis capture device not found");
        if (error) g_error_free(error);
        return 1;
    }
    GPtrArray *modes = capture_modes_enumerate(device, &error);
    if (modes == NULL) {
        g_printerr("%s\n", error ? error->message : "No capture modes");
        if (error) g_error_free(error);
        capture_device_free(device);
        return 1;
    }
    g_print("%s — %s — %s — %u modes\n", device->manufacturer, device->product,
            device->path, modes->len);
    for (guint i = 0; i < modes->len; i++) {
        CaptureMode *mode = g_ptr_array_index(modes, i);
        gchar *key = capture_mode_key(mode);
        g_print("%03u  %s  [%s]\n", i, mode->label, key);
        g_free(key);
    }
    g_ptr_array_unref(modes);
    capture_device_free(device);
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc > 1 && g_str_equal(argv[1], "--list-modes"))
        return list_modes();

    gst_init(&argc, &argv);
    AppState app = {0};
    g_mutex_init(&app.stats_mutex);
    g_mutex_init(&app.log_mutex);
    app.audio_enabled = TRUE;
    app.volume = 0.8;
    app.panel_dwell_ms = 150;
    app.panel_hide_delay_ms = 700;
    app.audio_source_latency_us = -1;
    app.audio_source_buffer_us = -1;
    app.latency_min_ns = -1;
    app.latency_max_ns = -1;
    app.config_path = g_build_filename(g_get_user_config_dir(), "captureviewer",
                                       "config.ini", NULL);
    app.log_path = g_build_filename(g_get_user_data_dir(), "captureviewer",
                                    "captureviewer.log", NULL);
    gchar *legacy_config_path = g_build_filename(g_get_user_config_dir(),
                                                  "hagibis-viewer", "config.ini", NULL);
    gboolean new_config_exists = g_file_test(app.config_path, G_FILE_TEST_EXISTS);
    GKeyFile *key_file = g_key_file_new();
    gboolean config_loaded = g_key_file_load_from_file(
        key_file, new_config_exists ? app.config_path : legacy_config_path,
        G_KEY_FILE_NONE, NULL);
    if (config_loaded && !new_config_exists) {
        gsize length = 0;
        gchar *contents = g_key_file_to_data(key_file, &length, NULL);
        gchar *directory = g_path_get_dirname(app.config_path);
        if (g_mkdir_with_parents(directory, 0700) != 0 ||
            !g_file_set_contents(app.config_path, contents, (gssize)length, NULL))
            g_warning("Could not migrate existing preferences to %s", app.config_path);
        g_free(directory);
        g_free(contents);
    }
    if (config_loaded) {
        if (g_key_file_has_key(key_file, "capture", "mode", NULL))
            app.preferred_mode_key = g_key_file_get_string(key_file, "capture", "mode", NULL);
        if (g_key_file_has_key(key_file, "audio", "enabled", NULL))
            app.audio_enabled = g_key_file_get_boolean(key_file, "audio", "enabled", NULL);
        if (g_key_file_has_key(key_file, "audio", "volume", NULL)) {
            gdouble stored_volume = g_key_file_get_double(key_file, "audio", "volume", NULL);
            if (stored_volume >= 0.0 && stored_volume <= 1.0)
                app.volume = stored_volume;
        }
        if (g_key_file_has_key(key_file, "ui", "panel-pinned", NULL))
            app.pinned = g_key_file_get_boolean(key_file, "ui", "panel-pinned", NULL);
        if (g_key_file_has_key(key_file, "ui", "stats-visible", NULL))
            app.stats_visible = g_key_file_get_boolean(key_file, "ui", "stats-visible", NULL);
        if (g_key_file_has_key(key_file, "ui", "dwell-ms", NULL)) {
            gint dwell = g_key_file_get_integer(key_file, "ui", "dwell-ms", NULL);
            if (dwell >= 100 && dwell <= 250)
                app.panel_dwell_ms = (guint)dwell;
        }
        if (g_key_file_has_key(key_file, "ui", "hide-delay-ms", NULL)) {
            gint delay = g_key_file_get_integer(key_file, "ui", "hide-delay-ms", NULL);
            if (delay >= 500 && delay <= 1000)
                app.panel_hide_delay_ms = (guint)delay;
        }
        gchar *stored_scale_mode =
            g_key_file_get_string(key_file, "ui", "scaling-mode", NULL);
        app.scale_mode = g_strcmp0(stored_scale_mode, "fill") == 0
            ? CAPTURE_RENDERER_FILL : CAPTURE_RENDERER_FIT;
        g_free(stored_scale_mode);
    }
    g_free(legacy_config_path);
    g_key_file_unref(key_file);
    GtkApplication *application = gtk_application_new("io.github.wully616.captureviewer",
                                                       G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(app_activate), &app);
    g_signal_connect(application, "shutdown", G_CALLBACK(app_shutdown), &app);
    gint status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
