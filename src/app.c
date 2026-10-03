#define _GNU_SOURCE
#include "app.h"
#include "capture.h"
#include "audio.h"
#include "pipeline.h"
#include "preferences.h"
#include "renderer.h"
#include "ui.h"
#include <gtk/gtk.h>
#include <gst/gst.h>
#include <linux/videodev2.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>


#define DEVICE_RESCAN_MS 1000
#define MODE_DEFAULT_W 1280
#define MODE_DEFAULT_H 720
#define MODE_DEFAULT_FPS 60

typedef CaptureViewerApp AppState;
struct _CaptureViewerApp {
    GtkApplication *application;
    CaptureUi *ui;
    CaptureRenderer *renderer;
    GPtrArray *modes;
    GPtrArray *video_devices;
    CapturePreferences *preferences;
    CapturePreferencesValues *prefs;
    CaptureDevice *video_device;
    CaptureVideoNode *video_node;
    CaptureAudio *audio;
    gchar *log_path;
    gchar *renderer_backend_logged;
    gchar *pipeline_error;
    CapturePipeline *pipeline;
    CapturePipelineStats pipeline_stats;
    guint monitor_watch_id;
    guint stats_watch_id;
    gint64 retry_after_us;
    guint current_mode;
    gboolean closing;
    GMutex log_mutex;
};

static void app_refresh_ui(AppState *app);
static void app_restart_pipeline(AppState *app);
static void app_log(AppState *app, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static void app_log_session_context(AppState *app);
static void app_log_renderer_backend(AppState *app);
static void handle_ui_action(const CaptureUiAction *action, gpointer user_data);
static void refresh_video_sources(AppState *app);
static void refresh_mode_selectors(AppState *app);
static void refresh_audio_sources(AppState *app);
static void app_shutdown_state(AppState *app);

CaptureViewerApp *
capture_viewer_app_new(void)
{
    AppState *app = g_new0(AppState, 1);
    app->preferences = capture_preferences_new();
    app->prefs = capture_preferences_get_values(app->preferences);
    g_mutex_init(&app->log_mutex);
    app->log_path = g_build_filename(g_get_user_data_dir(), "captureviewer",
                                    "captureviewer.log", NULL);
    return app;
}

void
capture_viewer_app_free(CaptureViewerApp *app)
{
    if (app == NULL)
        return;
    app_shutdown_state(app);
    g_free(app);
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

    GtkWidget *window = capture_ui_get_window(app->ui);
    GdkDisplay *display = window != NULL ? gtk_widget_get_display(window) : NULL;
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



static void
save_preferred_mode(AppState *app)
{
    if (app->video_device == NULL || app->modes == NULL ||
        app->current_mode >= app->modes->len)
        return;
    CaptureMode *mode = g_ptr_array_index(app->modes, app->current_mode);
    gchar *mode_key = capture_mode_key(mode);
    capture_preferences_set_mode_for_device(
        app->preferences, app->video_device->stable_id,
        app->video_device->usb_vid, app->video_device->usb_pid, mode_key);
}

static const gchar *
preferred_mode_for_device(AppState *app)
{
    if (app->video_device == NULL)
        return NULL;
    return capture_preferences_mode_for_device(
        app->preferences, app->video_device->stable_id,
        app->video_device->usb_vid, app->video_device->usb_pid);
}

static guint
choose_default_mode(AppState *app)
{
    if (app->modes == NULL || app->modes->len == 0)
        return 0;
    const gchar *preferred_key = preferred_mode_for_device(app);
    for (guint i = 0; i < app->modes->len; i++) {
        CaptureMode *mode = g_ptr_array_index(app->modes, i);
        if (capture_mode_equal_key(mode, preferred_key))
            return i;
    }

    gboolean hagibis = app->video_device != NULL &&
        app->video_device->usb_vid == 0x345f &&
        app->video_device->usb_pid == 0x2130;
    if (hagibis) {
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

    guint best_index = 0;
    for (guint i = 1; i < app->modes->len; i++) {
        CaptureMode *candidate = g_ptr_array_index(app->modes, i);
        CaptureMode *best = g_ptr_array_index(app->modes, best_index);
        guint64 candidate_rate = (guint64)candidate->fps_n * best->fps_d;
        guint64 best_rate = (guint64)best->fps_n * candidate->fps_d;
        if (candidate_rate > best_rate ||
            (candidate_rate == best_rate &&
             ((candidate->fourcc == V4L2_PIX_FMT_MJPEG) !=
              (best->fourcc == V4L2_PIX_FMT_MJPEG)
                ? candidate->fourcc == V4L2_PIX_FMT_MJPEG
                : (guint64)candidate->width * candidate->height >
                  (guint64)best->width * best->height)))
            best_index = i;
    }
    return best_index;
}


static void
pipeline_log_message(const gchar *message, gpointer user_data)
{
    app_log(user_data, "%s", message);
}

static void
pipeline_state_changed(gpointer user_data)
{
    AppState *app = user_data;
    app->retry_after_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    if (app->pipeline != NULL)
        app->pipeline_stats = capture_pipeline_get_stats(app->pipeline);
    capture_audio_clear_default_output(app->audio);
    app_refresh_ui(app);
}

static void
pipeline_stop(AppState *app)
{
    if (app->pipeline != NULL) {
        capture_pipeline_stop(app->pipeline);
        app->pipeline_stats = capture_pipeline_get_stats(app->pipeline);
    }
    capture_audio_clear_default_output(app->audio);
}

static gboolean
pipeline_start(AppState *app)
{
    if (app->video_device == NULL || app->video_node == NULL ||
        app->modes == NULL || app->current_mode >= app->modes->len ||
        app->pipeline == NULL)
        return FALSE;

    CaptureMode *mode = g_ptr_array_index(app->modes, app->current_mode);
    CaptureAudioState audio_state = capture_audio_get_state(app->audio);
    gboolean started = capture_pipeline_start(
        app->pipeline, app->video_device, app->video_node, mode,
        audio_state.source, app->prefs->audio_enabled, app->prefs->volume,
        audio_state.source_id, audio_state.source_status);
    app->pipeline_stats = capture_pipeline_get_stats(app->pipeline);
    if (started)
        app_log_renderer_backend(app);
    app_refresh_ui(app);
    return started;
}


static gboolean
refresh_audio_source(AppState *app)
{
    CaptureAudioUpdate update = capture_audio_update_source(
        app->audio, app->video_device, app->prefs->audio_selection_id);
    CaptureAudioState audio_state = capture_audio_get_state(app->audio);
    if (update.status_changed)
        app_log(app, "Audio input selection: %s",
                audio_state.source_status != NULL
                    ? audio_state.source_status : "status unavailable");
    if (update.source_changed) {
        if (app->pipeline != NULL &&
            capture_pipeline_is_running(app->pipeline) && app->prefs->audio_enabled)
            pipeline_stop(app);
        if (audio_state.source != NULL)
            app_log(app, "Selected audio input: %s [%s]",
                    audio_state.source_display_name != NULL
                        ? audio_state.source_display_name : "unnamed",
                    audio_state.source_id);
        else
            app_log(app, "No audio input is currently routed");
    }
    app_refresh_ui(app);
    return update.source_changed;
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

static CaptureVideoNode *
first_video_node(CaptureDevice *device)
{
    return device != NULL && device->nodes != NULL && device->nodes->len > 0
        ? g_ptr_array_index(device->nodes, 0) : NULL;
}

static CaptureVideoNode *
first_usable_video_node(CaptureDevice *device)
{
    for (guint i = 0; device != NULL && i < device->nodes->len; i++) {
        CaptureVideoNode *node = g_ptr_array_index(device->nodes, i);
        GError *error = NULL;
        GPtrArray *modes = capture_modes_enumerate(node, &error);
        g_clear_error(&error);
        gboolean usable = FALSE;
        for (guint mode_index = 0; modes != NULL && mode_index < modes->len;
             mode_index++) {
            if (capture_pipeline_mode_is_usable(node, g_ptr_array_index(modes, mode_index))) {
                usable = TRUE;
                break;
            }
        }
        if (modes != NULL)
            g_ptr_array_unref(modes);
        if (usable)
            return node;
    }
    return NULL;
}

static CaptureDevice *
find_hagibis_video_device(GPtrArray *devices)
{
    for (guint i = 0; devices != NULL && i < devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(devices, i);
        if (device->usb_vid == 0x345f && device->usb_pid == 0x2130 &&
            first_usable_video_node(device) != NULL)
            return device;
    }
    return NULL;
}

static gboolean
same_video_device(const CaptureDevice *left_device,
                  const CaptureVideoNode *left_node,
                  const CaptureDevice *right_device,
                  const CaptureVideoNode *right_node)
{
    if (left_device == NULL || right_device == NULL)
        return left_device == right_device;
    return left_node != NULL && right_node != NULL &&
        g_strcmp0(left_device->physical_sysfs, right_device->physical_sysfs) == 0 &&
        g_strcmp0(left_node->path, right_node->path) == 0;
}


static CaptureVideoNode *
find_video_node_by_path(CaptureDevice *device, const gchar *path)
{
    for (guint i = 0; device != NULL && path != NULL &&
         i < device->nodes->len; i++) {
        CaptureVideoNode *node = g_ptr_array_index(device->nodes, i);
        if (g_strcmp0(node->path, path) == 0)
            return node;
    }
    return NULL;
}

static CaptureVideoNode *
find_video_node_by_interface(CaptureDevice *device, const gchar *interface_sysfs)
{
    for (guint i = 0; device != NULL && interface_sysfs != NULL &&
         i < device->nodes->len; i++) {
        CaptureVideoNode *node = g_ptr_array_index(device->nodes, i);
        if (g_strcmp0(node->interface_sysfs, interface_sysfs) == 0)
            return node;
    }
    return NULL;
}

static void
set_video_source(AppState *app, CaptureDevice *device, CaptureVideoNode *node)
{
    if (same_video_device(app->video_device, app->video_node, device, node)) {
        app->video_device = device;
        app->video_node = node;
        return;
    }

    pipeline_stop(app);
    app->video_device = device;
    app->video_node = node;
    g_clear_pointer(&app->pipeline_error, g_free);
    if (app->pipeline != NULL)
        capture_pipeline_clear_error(app->pipeline);
    if (app->modes != NULL)
        g_ptr_array_unref(app->modes);
    app->modes = NULL;
    app->current_mode = 0;
    if (node != NULL) {
        GError *error = NULL;
        app->modes = capture_modes_enumerate(node, &error);
        if (app->modes == NULL) {
            app->pipeline_error = g_strdup(
                error != NULL ? error->message : "Could not enumerate capture modes");
            app_log(app, "Mode enumeration failed on %s (%s): %s",
                    device != NULL ? device->stable_id : "unknown source",
                    node->path, app->pipeline_error);
            g_clear_error(&error);
        } else {
            for (guint i = 0; i < app->modes->len;) {
                CaptureMode *item = g_ptr_array_index(app->modes, i);
                if (!capture_pipeline_mode_is_usable(node, item)) {
                    const gchar *reason =
                        capture_pipeline_mode_unusable_reason(node, item);
                    gchar *key = capture_mode_key(item);
                    app_log(app, "Skipping unusable mode %s [%s] on %s: %s",
                            item->label, key, node->path, reason);
                    g_free(key);
                    g_ptr_array_remove_index(app->modes, i);
                } else {
                    i++;
                }
            }
            if (app->modes->len == 0) {
                app->pipeline_error = g_strdup(
                    "No advertised capture mode has a supported GStreamer caps/decoder path");
                app_log(app, "%s on %s", app->pipeline_error, node->path);
            } else {
                app->current_mode = choose_default_mode(app);
                CaptureMode *mode = g_ptr_array_index(app->modes, app->current_mode);
                app_log(app, "Selected %s at %s, USB sysfs %s; %u usable modes, selected %s",
                        device->display_name, node->path,
                        device->usb_sysfs != NULL ? device->usb_sysfs : "non-USB",
                        app->modes->len, mode->label);
                save_preferred_mode(app);
            }
        }
    } else {
        app_log(app, "Selected video source is unavailable; waiting for reconnect");
    }
    refresh_mode_selectors(app);
    app_refresh_ui(app);
}

static void
replace_video_devices(AppState *app, GPtrArray *fresh_devices)
{
    const gchar *desired_id = app->prefs->selected_video_device_id;
    const gchar *desired_parent = app->prefs->selected_video_parent_path;
    if (desired_id == NULL && app->video_device != NULL)
        desired_id = app->video_device->stable_id;
    if (desired_parent == NULL && app->video_device != NULL)
        desired_parent = app->video_device->physical_sysfs;

    CaptureDevice *fresh_device =
        capture_device_find_by_id(fresh_devices, desired_id);
    if (fresh_device == NULL && desired_parent != NULL)
        fresh_device =
            capture_device_find_by_physical_path(fresh_devices, desired_parent);
    gboolean initial_selection = desired_id == NULL && desired_parent == NULL;
    if (initial_selection) {
        fresh_device = find_hagibis_video_device(fresh_devices);
        if (fresh_device == NULL) {
            for (guint i = 0; i < fresh_devices->len; i++) {
                CaptureDevice *candidate = g_ptr_array_index(fresh_devices, i);
                if (candidate->usb_sysfs != NULL &&
                    first_usable_video_node(candidate) != NULL) {
                    fresh_device = candidate;
                    break;
                }
            }
        }
    }
    if (initial_selection && fresh_device != NULL &&
        (fresh_device->usb_vid != 0x345f || fresh_device->usb_pid != 0x2130))
        g_clear_pointer(&app->prefs->legacy_mode_key, g_free);

    CaptureVideoNode *fresh_node = fresh_device != NULL
        ? find_video_node_by_interface(fresh_device, app->prefs->selected_node_interface) : NULL;
    if (fresh_node == NULL)
        fresh_node = fresh_device != NULL
            ? find_video_node_by_path(fresh_device, app->video_node != NULL
                ? app->video_node->path : NULL) : NULL;
    if (fresh_node == NULL)
        fresh_node = initial_selection
            ? first_usable_video_node(fresh_device) : first_video_node(fresh_device);

    if (fresh_device != NULL) {
        if (desired_parent != NULL &&
            g_strcmp0(desired_parent, fresh_device->physical_sysfs) == 0)
            capture_preferences_migrate_device_id(
                app->preferences, desired_id, fresh_device->stable_id);
        gboolean selection_changed =
            g_strcmp0(app->prefs->selected_video_device_id, fresh_device->stable_id) != 0 ||
            g_strcmp0(app->prefs->selected_video_parent_path,
                      fresh_device->physical_sysfs) != 0;
        g_free(app->prefs->selected_video_device_id);
        app->prefs->selected_video_device_id = g_strdup(fresh_device->stable_id);
        g_free(app->prefs->selected_video_parent_path);
        app->prefs->selected_video_parent_path = g_strdup(fresh_device->physical_sysfs);
        if (fresh_node != NULL) {
            g_free(app->prefs->selected_node_interface);
            app->prefs->selected_node_interface = g_strdup(fresh_node->interface_sysfs);
        }
        if (selection_changed)
            capture_preferences_schedule_save(app->preferences);
    }

    GPtrArray *old_devices = app->video_devices;
    app->video_devices = fresh_devices;
    set_video_source(app, fresh_device, fresh_node);
    refresh_video_sources(app);
    if (old_devices != NULL)
        g_ptr_array_unref(old_devices);
    app_refresh_ui(app);
}

static gboolean
update_devices(gpointer user_data)
{
    AppState *app = user_data;
    if (app->closing)
        return G_SOURCE_REMOVE;

    GError *error = NULL;
    GPtrArray *fresh_devices = capture_devices_enumerate(&error);
    if (fresh_devices == NULL) {
        app_log(app, "Capture-device enumeration failed: %s",
                error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    } else {
        replace_video_devices(app, fresh_devices);
    }

    refresh_audio_source(app);

    if (app->video_device != NULL && app->modes != NULL && app->modes->len > 0 &&
        app->pipeline != NULL && !capture_pipeline_is_running(app->pipeline) &&
        g_get_monotonic_time() >= app->retry_after_us)
        pipeline_start(app);
    return G_SOURCE_CONTINUE;
}


static gboolean
rescan_devices_idle(gpointer user_data)
{
    update_devices(user_data);
    return G_SOURCE_REMOVE;
}

static void
audio_device_event(const CaptureAudioDeviceEvent *event, gpointer user_data)
{
    AppState *app = user_data;
    if (event != NULL)
        app_log(app, "GStreamer device %s: name=%s class=%s identity=%s properties=%s",
                event->added ? "added" : "removed",
                event->display_name,
                event->device_class != NULL ? event->device_class : "unknown",
                event->identity, event->properties);
    refresh_audio_sources(app);
    g_idle_add(rescan_devices_idle, app);
}

static gboolean
stats_update(gpointer user_data)
{
    AppState *app = user_data;
    if (app->closing)
        return G_SOURCE_REMOVE;

    if (app->pipeline != NULL &&
        capture_pipeline_is_running(app->pipeline)) {
        capture_pipeline_update_stats(app->pipeline);
        app->pipeline_stats = capture_pipeline_get_stats(app->pipeline);
        if (capture_audio_refresh_default_output(app->audio)) {
            CaptureAudioState audio_state = capture_audio_get_state(app->audio);
            if (audio_state.default_output_name != NULL ||
                audio_state.default_output_id != NULL)
                app_log(app, "PulseAudio default playback output: name=%s id=%s",
                        audio_state.default_output_name != NULL
                            ? audio_state.default_output_name : "(unavailable)",
                        audio_state.default_output_id != NULL
                            ? audio_state.default_output_id : "(unavailable)");
        }
    }
    app_log_renderer_backend(app);
    app_refresh_ui(app);
    return G_SOURCE_CONTINUE;
}

static void
app_refresh_ui(AppState *app)
{
    if (app->ui == NULL)
        return;
    gboolean pipeline_running = app->pipeline != NULL &&
        capture_pipeline_is_running(app->pipeline);
    const gchar *pipeline_error = app->pipeline != NULL
        ? capture_pipeline_get_error(app->pipeline) : NULL;
    if (pipeline_error == NULL)
        pipeline_error = app->pipeline_error;
    CaptureUiModel model = {
        .preferences = app->prefs,
        .video_device = app->video_device,
        .video_node = app->video_node,
        .modes = app->modes,
        .current_mode = app->current_mode,
        .audio = capture_audio_get_state(app->audio),
        .pipeline = app->pipeline,
        .pipeline_stats = app->pipeline_stats,
        .pipeline_error = pipeline_error,
        .log_path = app->log_path,
        .renderer_backend = capture_renderer_get_backend_name(app->renderer),
        .pipeline_running = pipeline_running,
        .pipeline_has_audio_source = app->pipeline != NULL &&
            capture_pipeline_has_audio_source(app->pipeline),
        .waiting_for_frames = pipeline_running &&
            capture_pipeline_is_waiting_for_frames(app->pipeline),
        .retry_after_us = app->retry_after_us,
    };
    capture_ui_update(app->ui, &model);
}

static void
refresh_video_sources(AppState *app)
{
    if (app->ui != NULL)
        capture_ui_refresh_video_sources(
            app->ui, app->video_devices, app->video_device, app->video_node,
            app->prefs);
}

static void
refresh_mode_selectors(AppState *app)
{
    if (app->ui != NULL)
        capture_ui_refresh_mode_selectors(app->ui, app->modes, app->current_mode);
}

static void
refresh_audio_sources(AppState *app)
{
    if (app->ui != NULL)
        capture_ui_refresh_audio_sources(
            app->ui, app->audio, app->prefs->audio_selection_id);
}

static gint
find_mode_index(AppState *app, guint32 fourcc, guint width, guint height,
                guint preferred_fps_n, guint preferred_fps_d)
{
    gint first_match = -1;
    for (guint i = 0; app->modes != NULL && i < app->modes->len; i++) {
        CaptureMode *mode = g_ptr_array_index(app->modes, i);
        if (mode->fourcc != fourcc ||
            (width != 0 && mode->width != width) ||
            (height != 0 && mode->height != height))
            continue;
        if (first_match < 0)
            first_match = (gint)i;
        if (preferred_fps_n != 0 && preferred_fps_d != 0 &&
            (guint64)mode->fps_n * preferred_fps_d ==
                (guint64)preferred_fps_n * mode->fps_d)
            return (gint)i;
    }
    return first_match;
}

static void
select_mode_index(AppState *app, guint index)
{
    if (app->modes == NULL || index >= app->modes->len ||
        index == app->current_mode)
        return;
    app->current_mode = index;
    save_preferred_mode(app);
    CaptureMode *mode = g_ptr_array_index(app->modes, index);
    app_log(app, "Selected capture mode %s", mode->label);
    refresh_mode_selectors(app);
    app_refresh_ui(app);
    app_restart_pipeline(app);
}

static void
handle_format_changed(AppState *app, const gchar *format_id)
{
    if (format_id == NULL || app->modes == NULL || app->modes->len == 0)
        return;
    guint32 fourcc = (guint32)g_ascii_strtoull(format_id, NULL, 16);
    CaptureMode *current = g_ptr_array_index(app->modes, app->current_mode);
    gint index = find_mode_index(app, fourcc, current->width, current->height,
                                 current->fps_n, current->fps_d);
    if (index < 0)
        index = find_mode_index(app, fourcc, current->width, current->height, 0, 0);
    if (index < 0)
        index = find_mode_index(app, fourcc, 0, 0, 0, 0);
    if (index >= 0)
        select_mode_index(app, (guint)index);
}

static void
handle_resolution_changed(AppState *app, const gchar *resolution_id)
{
    if (resolution_id == NULL || app->modes == NULL || app->modes->len == 0)
        return;
    CaptureMode *current = g_ptr_array_index(app->modes, app->current_mode);
    guint width = 0, height = 0;
    if (sscanf(resolution_id, "%u:%u", &width, &height) != 2)
        return;
    gint index = find_mode_index(app, current->fourcc, width, height,
                                 current->fps_n, current->fps_d);
    if (index < 0)
        index = find_mode_index(app, current->fourcc, width, height, 0, 0);
    if (index >= 0)
        select_mode_index(app, (guint)index);
}

static void
handle_frame_rate_changed(AppState *app, const gchar *mode_key)
{
    if (mode_key == NULL || app->modes == NULL)
        return;
    for (guint i = 0; i < app->modes->len; i++) {
        CaptureMode *mode = g_ptr_array_index(app->modes, i);
        if (capture_mode_equal_key(mode, mode_key)) {
            select_mode_index(app, i);
            return;
        }
    }
}

static void
handle_source_changed(AppState *app, const gchar *device_id)
{
    if (device_id == NULL)
        return;
    CaptureDevice *device = capture_device_find_by_id(app->video_devices, device_id);
    if (device == NULL) {
        g_free(app->prefs->selected_video_device_id);
        app->prefs->selected_video_device_id = g_strdup(device_id);
        capture_preferences_schedule_save(app->preferences);
        set_video_source(app, NULL, NULL);
        refresh_audio_source(app);
        app_restart_pipeline(app);
        return;
    }
    gboolean same_physical_device = app->video_device != NULL &&
        g_strcmp0(app->video_device->physical_sysfs, device->physical_sysfs) == 0;
    CaptureVideoNode *node = same_physical_device
        ? find_video_node_by_interface(device, app->prefs->selected_node_interface) : NULL;
    if (node == NULL)
        node = first_video_node(device);
    if (device->usb_vid != 0x345f || device->usb_pid != 0x2130)
        g_clear_pointer(&app->prefs->legacy_mode_key, g_free);
    g_free(app->prefs->selected_video_device_id);
    app->prefs->selected_video_device_id = g_strdup(device->stable_id);
    g_free(app->prefs->selected_video_parent_path);
    app->prefs->selected_video_parent_path = g_strdup(device->physical_sysfs);
    g_free(app->prefs->selected_node_interface);
    app->prefs->selected_node_interface = node != NULL
        ? g_strdup(node->interface_sysfs) : NULL;
    capture_preferences_schedule_save(app->preferences);
    set_video_source(app, device, node);
    refresh_audio_source(app);
    refresh_video_sources(app);
    app_restart_pipeline(app);
}

static void
handle_video_node_changed(AppState *app, const gchar *interface_id)
{
    if (interface_id == NULL || app->video_device == NULL)
        return;
    CaptureVideoNode *node =
        find_video_node_by_interface(app->video_device, interface_id);
    if (node == NULL || node == app->video_node)
        return;
    g_free(app->prefs->selected_node_interface);
    app->prefs->selected_node_interface = g_strdup(node->interface_sysfs);
    capture_preferences_schedule_save(app->preferences);
    set_video_source(app, app->video_device, node);
    app_restart_pipeline(app);
}

static void
handle_audio_selection_changed(AppState *app, const gchar *selection)
{
    if (selection == NULL)
        return;
    g_free(app->prefs->audio_selection_id);
    app->prefs->audio_selection_id = g_strdup(selection);
    app->prefs->audio_selection_session_only =
        g_str_has_prefix(selection, "session:");
    capture_preferences_schedule_save(app->preferences);
    gboolean audio_changed = refresh_audio_source(app);
    if (audio_changed && app->prefs->audio_enabled)
        app_restart_pipeline(app);
}

static void
handle_ui_action(const CaptureUiAction *action, gpointer user_data)
{
    AppState *app = user_data;
    switch (action->type) {
    case CAPTURE_UI_ACTION_SOURCE:
        handle_source_changed(app, action->value);
        break;
    case CAPTURE_UI_ACTION_VIDEO_NODE:
        handle_video_node_changed(app, action->value);
        break;
    case CAPTURE_UI_ACTION_FORMAT:
        handle_format_changed(app, action->value);
        break;
    case CAPTURE_UI_ACTION_RESOLUTION:
        handle_resolution_changed(app, action->value);
        break;
    case CAPTURE_UI_ACTION_FRAME_RATE:
        handle_frame_rate_changed(app, action->value);
        break;
    case CAPTURE_UI_ACTION_ADVANCED_SOURCES:
        app->prefs->include_advanced_sources = action->enabled;
        capture_preferences_schedule_save(app->preferences);
        refresh_video_sources(app);
        break;
    case CAPTURE_UI_ACTION_AUDIO_SELECTION:
        handle_audio_selection_changed(app, action->value);
        break;
    case CAPTURE_UI_ACTION_AUDIO_ENABLED:
        app->prefs->audio_enabled = action->enabled;
        capture_preferences_schedule_save(app->preferences);
        app_log(app, "Capture audio %s",
                app->prefs->audio_enabled ? "enabled" : "disabled");
        app_restart_pipeline(app);
        break;
    case CAPTURE_UI_ACTION_SCALING: {
        CaptureRendererScaleMode mode =
            g_strcmp0(action->value, "fill") == 0 ? CAPTURE_RENDERER_FILL
                                                    : CAPTURE_RENDERER_FIT;
        CapturePreferencesScaleMode preference_mode =
            mode == CAPTURE_RENDERER_FILL ? CAPTURE_PREFERENCES_SCALE_FILL
                                          : CAPTURE_PREFERENCES_SCALE_FIT;
        if (preference_mode == app->prefs->scale_mode)
            break;
        app->prefs->scale_mode = preference_mode;
        capture_renderer_set_scale_mode(app->renderer, mode);
        capture_preferences_schedule_save(app->preferences);
        app_log(app, "Video scaling mode set to %s",
                mode == CAPTURE_RENDERER_FILL ? "fill" : "fit");
        break;
    }
    case CAPTURE_UI_ACTION_VOLUME:
        app->prefs->volume = action->number;
        if (app->pipeline != NULL)
            capture_pipeline_set_volume(app->pipeline, app->prefs->volume);
        capture_preferences_schedule_save(app->preferences);
        break;
    case CAPTURE_UI_ACTION_PINNED:
        app->prefs->pinned = action->enabled;
        capture_preferences_schedule_save(app->preferences);
        break;
    case CAPTURE_UI_ACTION_STATS_VISIBLE:
        app->prefs->stats_visible = action->enabled;
        capture_preferences_schedule_save(app->preferences);
        break;
    case CAPTURE_UI_ACTION_DWELL_DELAY:
        app->prefs->panel_dwell_ms = (guint)action->number;
        capture_preferences_schedule_save(app->preferences);
        app_refresh_ui(app);
        break;
    case CAPTURE_UI_ACTION_HIDE_DELAY:
        app->prefs->panel_hide_delay_ms = (guint)action->number;
        capture_preferences_schedule_save(app->preferences);
        app_refresh_ui(app);
        break;
    case CAPTURE_UI_ACTION_VOLUME_RELEASED:
        capture_preferences_schedule_save(app->preferences);
        break;
    case CAPTURE_UI_ACTION_SETTINGS_VISIBILITY:
        app_refresh_ui(app);
        break;
    case CAPTURE_UI_ACTION_QUIT:
        g_application_quit(G_APPLICATION(app->application));
        break;
    }
}


void
capture_viewer_app_activate(GtkApplication *application, gpointer user_data)
{
    AppState *app = user_data;
    app->application = application;
    if (app->ui != NULL) {
        capture_ui_present(app->ui);
        return;
    }

    GError *renderer_error = NULL;
    app->renderer = capture_renderer_new(&renderer_error);
    if (app->renderer == NULL) {
        app_log(app, "Could not create video renderer: %s",
                renderer_error != NULL ? renderer_error->message : "unknown error");
        g_clear_error(&renderer_error);
        g_application_quit(G_APPLICATION(application));
        return;
    }
    capture_renderer_set_scale_mode(
        app->renderer,
        app->prefs->scale_mode == CAPTURE_PREFERENCES_SCALE_FILL
            ? CAPTURE_RENDERER_FILL : CAPTURE_RENDERER_FIT);
    CapturePipelineCallbacks pipeline_callbacks = {
        .log_message = pipeline_log_message,
        .state_changed = pipeline_state_changed,
    };
    app->pipeline =
        capture_pipeline_new(app->renderer, &pipeline_callbacks, app);
    app->pipeline_stats = capture_pipeline_get_stats(app->pipeline);
    CaptureUiCallbacks ui_callbacks = {
        .action = handle_ui_action,
    };
    app->ui = capture_ui_new(application, app->renderer, app->prefs,
                             &ui_callbacks, app);
    capture_ui_present(app->ui);
    app_log_session_context(app);
    app_log_renderer_backend(app);
    app->audio = capture_audio_new(audio_device_event, app);
    if (!capture_audio_start(app->audio))
        app_log(app, "GStreamer device monitor could not start; periodic sysfs scan remains active");

    refresh_audio_sources(app);
    refresh_video_sources(app);
    refresh_mode_selectors(app);
    update_devices(app);
    app->monitor_watch_id = g_timeout_add(DEVICE_RESCAN_MS, update_devices, app);
    app->stats_watch_id = g_timeout_add(333, stats_update, app);
    app_log(app, "Viewer started; S toggles controls, F11 toggles fullscreen, Q quits");
}

static void
app_shutdown_state(AppState *app)
{
    if (app->closing)
        return;
    app->closing = TRUE;
    capture_preferences_save(app->preferences);
    if (app->monitor_watch_id != 0)
        g_source_remove(app->monitor_watch_id);
    if (app->stats_watch_id != 0)
        g_source_remove(app->stats_watch_id);
    capture_audio_stop(app->audio);
    if (app->pipeline != NULL) {
        capture_pipeline_free(app->pipeline);
        app->pipeline = NULL;
    }
    if (app->renderer != NULL) {
        capture_renderer_free(app->renderer);
        app->renderer = NULL;
    }
    capture_ui_free(app->ui);
    app->ui = NULL;
    capture_audio_free(app->audio);
    app->audio = NULL;
    if (app->modes != NULL)
        g_ptr_array_unref(app->modes);
    if (app->video_devices != NULL)
        g_ptr_array_unref(app->video_devices);

    capture_preferences_free(app->preferences);
    g_free(app->pipeline_error);
    g_free(app->renderer_backend_logged);
    app_log(app, "Viewer shut down cleanly");
    g_free(app->log_path);
    g_mutex_clear(&app->log_mutex);
}

void
capture_viewer_app_shutdown(GApplication *application, gpointer user_data)
{
    app_shutdown_state(user_data);
    (void)application;
}


gint
capture_viewer_app_list_modes(void)
{
    GError *error = NULL;
    GPtrArray *devices = capture_devices_enumerate(&error);
    if (devices == NULL) {
        g_printerr("%s\n", error != NULL ? error->message
                                          : "Cannot enumerate capture devices");
        g_clear_error(&error);
        return 1;
    }
    if (devices->len == 0) {
        g_printerr("No V4L2 capture devices found\n");
        g_ptr_array_unref(devices);
        return 1;
    }

    guint advertised_modes = 0;
    for (guint device_index = 0; device_index < devices->len; device_index++) {
        CaptureDevice *device = g_ptr_array_index(devices, device_index);
        g_print("%s\n  ID: %s\n  Physical sysfs: %s\n",
                device->display_name != NULL ? device->display_name : "Video source",
                device->stable_id != NULL ? device->stable_id : "unavailable",
                device->physical_sysfs != NULL ? device->physical_sysfs : "unavailable");
        for (guint node_index = 0; node_index < device->nodes->len; node_index++) {
            CaptureVideoNode *node = g_ptr_array_index(device->nodes, node_index);
            g_print("  Node %s (interface %s, driver %s):\n",
                    node->path,
                    node->interface_sysfs != NULL
                        ? node->interface_sysfs : "unavailable",
                    node->driver != NULL ? node->driver : "unavailable");
            GPtrArray *modes = capture_modes_enumerate(node, &error);
            if (modes == NULL) {
                g_print("    Enumeration failed: %s\n",
                        error != NULL ? error->message : "unknown error");
                g_clear_error(&error);
                continue;
            }
            if (modes->len == 0)
                g_print("    No advertised capture modes\n");
            for (guint mode_index = 0; mode_index < modes->len; mode_index++) {
                CaptureMode *mode = g_ptr_array_index(modes, mode_index);
                gchar *key = capture_mode_key(mode);
                const gchar *reason =
                    capture_pipeline_mode_unusable_reason(node, mode);
                if (reason == NULL) {
                    g_print("    %03u  %s  [%s] — usable\n",
                            mode_index, mode->label, key);
                } else {
                    g_print("    %03u  %s  [%s] — unusable: %s\n",
                            mode_index, mode->label, key, reason);
                }
                advertised_modes++;
                g_free(key);
            }
            g_ptr_array_unref(modes);
        }
    }
    g_ptr_array_unref(devices);
    return advertised_modes > 0 ? 0 : 1;
}

