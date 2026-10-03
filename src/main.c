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
    GtkWidget *mode_diagnostic_label;
    GtkWidget *renderer_label;
    GtkWidget *source_combo;
    GtkWidget *node_combo;
    GtkWidget *node_selector;
    GtkWidget *format_combo;
    GtkWidget *resolution_combo;
    GtkWidget *fps_combo;
    GtkWidget *scale_combo;
    GtkWidget *advanced_sources_check;
    GtkWidget *audio_combo;
    GtkWidget *audio_toggle;
    GtkWidget *volume_scale;
    GtkWidget *status_label;
    GtkWidget *audio_route_label;
    GtkWidget *pipeline_error_label;
    GtkWidget *log_path_label;
    GtkWidget *perf_label;
    GPtrArray *modes;
    GPtrArray *video_devices;
    GHashTable *mode_preferences;
    CaptureDevice *video_device;
    CaptureVideoNode *video_node;
    gchar *selected_video_device_id;
    gchar *selected_video_parent_path;
    gchar *selected_node_interface;
    GstDeviceMonitor *device_monitor;
    GstDevice *audio_device;
    gchar *audio_device_id;
    gchar *audio_display_name;
    gchar *audio_sink_name;
    gchar *audio_sink_id;
    gchar *audio_selection_id;
    gchar *audio_selection_status;
    gchar *legacy_mode_key;
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
    gint64 pipeline_started_us;
    gint64 last_video_frame_us;
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
    gboolean panel_visible;
    gboolean panel_pointer_inside;
    gboolean edge_hotspot_inside;
    gboolean mode_popup_open;
    gboolean interaction_active;
    gboolean pinned;
    gboolean stats_visible;
    gboolean include_advanced_sources;
    gboolean audio_selection_session_only;
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
static void populate_video_devices(AppState *app);
static void populate_mode_selectors(AppState *app);
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
normalize_sysfs_path(const gchar *path)
{
    if (path == NULL || *path == '\0')
        return NULL;
    gchar *absolute = g_str_has_prefix(path, "/devices/")
        ? g_build_filename("/sys", path + 1, NULL) : g_strdup(path);
    gchar *normalized = g_canonicalize_filename(absolute, NULL);
    g_free(absolute);
    return normalized;
}

static gchar *
usb_parent_from_sysfs(const gchar *sysfs_path)
{
    gchar *path = normalize_sysfs_path(sysfs_path);
    while (path != NULL) {
        gchar *vendor = g_build_filename(path, "idVendor", NULL);
        gchar *product = g_build_filename(path, "idProduct", NULL);
        gboolean is_usb_parent =
            g_file_test(vendor, G_FILE_TEST_EXISTS) &&
            g_file_test(product, G_FILE_TEST_EXISTS);
        g_free(vendor);
        g_free(product);
        if (is_usb_parent)
            return path;
        gchar *parent = g_path_get_dirname(path);
        if (g_str_equal(parent, path)) {
            g_free(parent);
            g_free(path);
            return NULL;
        }
        g_free(path);
        path = parent;
    }
    return NULL;
}

static gchar *
usb_interface_from_sysfs(const gchar *sysfs_path, const gchar *usb_parent)
{
    gchar *path = normalize_sysfs_path(sysfs_path);
    if (path == NULL || usb_parent == NULL ||
        !g_str_has_prefix(path, usb_parent) ||
        path[strlen(usb_parent)] != '/') {
        g_free(path);
        return NULL;
    }
    const gchar *relative = path + strlen(usb_parent) + 1;
    const gchar *separator = strchr(relative, '/');
    gsize length = separator != NULL ? (gsize)(separator - relative)
                                     : strlen(relative);
    gchar *interface = g_strndup(relative, length);
    g_free(path);
    if (strchr(interface, ':') == NULL) {
        g_free(interface);
        return NULL;
    }
    return interface;
}

static gchar *
audio_device_identity(GstDevice *device, gboolean *persistent)
{
    if (persistent != NULL)
        *persistent = FALSE;
    GstStructure *properties = gst_device_get_properties(device);
    if (properties == NULL) {
        gchar *name = gst_device_get_display_name(device);
        gchar *identity = g_strdup_printf("session:%s", name);
        g_free(name);
        return identity;
    }
    guint64 vendor = 0, product = 0;
    gboolean have_usb_ids =
        get_usb_id(properties, "device.vendor.id", &vendor) &&
        get_usb_id(properties, "device.product.id", &product);
    const gchar *serial = gst_structure_get_string(properties, "device.serial");
    const gchar *sysfs = gst_structure_get_string(properties, "sysfs.path");
    gchar *usb_parent = usb_parent_from_sysfs(sysfs);
    gchar *usb_interface = usb_parent != NULL
        ? usb_interface_from_sysfs(sysfs, usb_parent) : NULL;
    gchar *normalized_sysfs = usb_parent == NULL
        ? normalize_sysfs_path(sysfs) : NULL;
    gchar *identity = NULL;
    if (usb_parent != NULL && have_usb_ids) {
        identity = serial != NULL
            ? g_strdup_printf("usb:%04" G_GINT64_MODIFIER "x:%04"
                G_GINT64_MODIFIER "x:serial:%s:sysfs:%s%s%s",
                vendor, product, serial, usb_parent,
                usb_interface != NULL ? ":interface:" : "",
                usb_interface != NULL ? usb_interface : "")
            : g_strdup_printf("usb:%04" G_GINT64_MODIFIER "x:%04"
                G_GINT64_MODIFIER "x:sysfs:%s%s%s", vendor, product, usb_parent,
                usb_interface != NULL ? ":interface:" : "",
                usb_interface != NULL ? usb_interface : "");
        if (persistent != NULL)
            *persistent = TRUE;
    } else if (serial != NULL && have_usb_ids) {
        identity = g_strdup_printf("usb:%04" G_GINT64_MODIFIER "x:%04"
            G_GINT64_MODIFIER "x:serial:%s", vendor, product, serial);
        if (persistent != NULL)
            *persistent = TRUE;
    } else if (normalized_sysfs != NULL) {
        identity = g_strdup_printf("sysfs:%s", normalized_sysfs);
        if (persistent != NULL)
            *persistent = TRUE;
    } else {
        const gchar *node_name =
            gst_structure_get_string(properties, "node.name");
        gchar *display_name = node_name != NULL
            ? g_strdup(node_name) : gst_device_get_display_name(device);
        identity = g_strdup_printf("session:%s", display_name);
        g_free(display_name);
    }
    g_free(usb_parent);
    g_free(normalized_sysfs);
    g_free(usb_interface);
    gst_structure_free(properties);
    return identity;
}

static gboolean
is_audio_source_device(GstDevice *device)
{
    gchar *klass = gst_device_get_device_class(device);
    gboolean source = g_strcmp0(klass, "Audio/Source") == 0;
    g_free(klass);
    if (!source)
        return FALSE;
    GstStructure *properties = gst_device_get_properties(device);
    if (properties == NULL)
        return TRUE;
    const gchar *device_class = gst_structure_get_string(properties, "device.class");
    gboolean is_virtual = FALSE;
    gst_structure_get_boolean(properties, "node.virtual", &is_virtual);
    gboolean physical_source =
        g_strcmp0(device_class, "monitor") != 0 && !is_virtual;
    gst_structure_free(properties);
    return physical_source;
}

static gboolean
audio_device_matches_video(GstDevice *audio, const CaptureDevice *video)
{
    if (video == NULL || video->usb_sysfs == NULL)
        return FALSE;
    GstStructure *properties = gst_device_get_properties(audio);
    if (properties == NULL)
        return FALSE;
    guint64 vendor = 0, product = 0;
    gboolean have_usb_ids =
        get_usb_id(properties, "device.vendor.id", &vendor) &&
        get_usb_id(properties, "device.product.id", &product);
    if (have_usb_ids &&
        (vendor != video->usb_vid || product != video->usb_pid)) {
        gst_structure_free(properties);
        return FALSE;
    }
    const gchar *serial = gst_structure_get_string(properties, "device.serial");
    const gchar *sysfs = gst_structure_get_string(properties, "sysfs.path");
    gchar *audio_usb_parent = usb_parent_from_sysfs(sysfs);
    gboolean matches;
    if (audio_usb_parent != NULL) {
        matches = g_strcmp0(audio_usb_parent, video->usb_sysfs) == 0;
    } else {
        matches = have_usb_ids && video->serial != NULL && serial != NULL &&
            g_str_equal(video->serial, serial);
    }
    g_free(audio_usb_parent);
    gst_structure_free(properties);
    return matches;
}

static GstDevice *
resolve_selected_audio_device(AppState *app, gchar **identity, gchar **status)
{
    *identity = NULL;
    *status = NULL;
    const gchar *selection = app->audio_selection_id != NULL
        ? app->audio_selection_id : "auto";
    if (g_str_equal(selection, "none")) {
        *status = g_strdup("No audio input selected");
        return NULL;
    }
    if (g_str_equal(selection, "auto") &&
        (app->video_device == NULL || app->video_device->usb_sysfs == NULL)) {
        *status = g_strdup("Auto matching waits for a selected USB capture device");
        return NULL;
    }
    if (app->device_monitor == NULL) {
        *status = g_strdup("Audio device monitor is unavailable");
        return NULL;
    }

    GList *devices = gst_device_monitor_get_devices(app->device_monitor);
    GstDevice *found = NULL;
    guint matches = 0;
    for (GList *item = devices; item != NULL; item = item->next) {
        GstDevice *candidate = GST_DEVICE(item->data);
        if (!is_audio_source_device(candidate))
            continue;
        gboolean match;
        gchar *candidate_identity = audio_device_identity(candidate, NULL);
        if (g_str_equal(selection, "auto"))
            match = audio_device_matches_video(candidate, app->video_device);
        else
            match = g_str_equal(selection, candidate_identity);
        g_free(candidate_identity);
        if (match) {
            matches++;
            if (found == NULL)
                found = g_object_ref(candidate);
        }
    }
    g_list_free_full(devices, g_object_unref);
    if (matches == 1) {
        *identity = audio_device_identity(found, NULL);
        *status = g_strdup(g_str_equal(selection, "auto")
            ? "Auto-matched a unique audio input by USB physical identity"
            : "Selected the requested audio input by stable identity");
        return found;
    }
    if (found != NULL)
        gst_object_unref(found);
    if (matches > 1)
        *status = g_strdup_printf(
            "%u audio inputs match this identity; routing is disabled as ambiguous",
            matches);
    else if (g_str_equal(selection, "auto"))
        *status = g_strdup(
            "No audio input uniquely matches the selected capture device");
    else
        *status = g_strdup_printf("Selected audio input is unavailable: %s", selection);
    return NULL;
}

static void
populate_audio_selector(AppState *app)
{
    if (app->audio_combo == NULL)
        return;
    app->updating_controls = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->audio_combo));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->audio_combo),
                              "auto", "Auto — match selected capture device");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->audio_combo),
                              "none", "None");
    gboolean selected_present =
        g_strcmp0(app->audio_selection_id, "auto") == 0 ||
        g_strcmp0(app->audio_selection_id, "none") == 0;
    GHashTable *seen =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *devices = app->device_monitor != NULL
        ? gst_device_monitor_get_devices(app->device_monitor) : NULL;
    for (GList *item = devices; item != NULL; item = item->next) {
        GstDevice *device = GST_DEVICE(item->data);
        if (!is_audio_source_device(device))
            continue;
        gchar *identity = audio_device_identity(device, NULL);
        if (!g_hash_table_contains(seen, identity)) {
            g_hash_table_add(seen, g_strdup(identity));
            gchar *name = gst_device_get_display_name(device);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->audio_combo),
                                      identity, name);
            g_free(name);
        }
        if (g_strcmp0(identity, app->audio_selection_id) == 0)
            selected_present = TRUE;
        g_free(identity);
    }
    g_list_free_full(devices, g_object_unref);
    if (!selected_present && app->audio_selection_id != NULL)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->audio_combo),
                                  app->audio_selection_id,
                                  "Selected audio input unavailable");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->audio_combo),
                                app->audio_selection_id != NULL
                                    ? app->audio_selection_id : "auto");
    g_hash_table_unref(seen);
    app->updating_controls = FALSE;
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

static gchar *
mode_preference_digest(const gchar *stable_id)
{
    return stable_id != NULL
        ? g_compute_checksum_for_string(G_CHECKSUM_SHA256, stable_id, -1) : NULL;
}

static void
migrate_mode_preference(AppState *app, const gchar *old_id, const gchar *new_id)
{
    if (old_id == NULL || new_id == NULL || g_str_equal(old_id, new_id))
        return;
    gchar *old_digest = mode_preference_digest(old_id);
    gchar *new_digest = mode_preference_digest(new_id);
    const gchar *old_mode = g_hash_table_lookup(app->mode_preferences, old_digest);
    if (old_mode != NULL && !g_hash_table_contains(app->mode_preferences, new_digest))
        g_hash_table_insert(app->mode_preferences, g_strdup(new_digest),
                            g_strdup(old_mode));
    g_hash_table_remove(app->mode_preferences, old_digest);
    g_free(old_digest);
    g_free(new_digest);
}

static void
app_save_preferences(AppState *app)
{
    GKeyFile *key_file = g_key_file_new();
    if (app->selected_video_device_id != NULL)
        g_key_file_set_string(key_file, "capture", "device-id",
                              app->selected_video_device_id);
    if (app->selected_video_parent_path != NULL)
        g_key_file_set_string(key_file, "capture", "device-parent",
                              app->selected_video_parent_path);
    if (app->selected_node_interface != NULL)
        g_key_file_set_string(key_file, "capture", "node-id",
                              app->selected_node_interface);
    if (app->legacy_mode_key != NULL && app->selected_video_device_id == NULL)
        g_key_file_set_string(key_file, "capture", "mode", app->legacy_mode_key);
    g_key_file_set_boolean(key_file, "capture", "advanced-sources",
                           app->include_advanced_sources);
    g_key_file_set_boolean(key_file, "audio", "enabled", app->audio_enabled);
    if (!app->audio_selection_session_only && app->audio_selection_id != NULL)
        g_key_file_set_string(key_file, "audio", "source-id",
                              app->audio_selection_id);
    g_key_file_set_double(key_file, "audio", "volume", app->volume);
    GHashTableIter iter;
    gpointer digest, mode_key;
    g_hash_table_iter_init(&iter, app->mode_preferences);
    while (g_hash_table_iter_next(&iter, &digest, &mode_key))
        g_key_file_set_string(key_file, "capture-modes", digest, mode_key);
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
    if (app->video_device == NULL || app->modes == NULL ||
        app->current_mode >= app->modes->len)
        return;
    gchar *digest = mode_preference_digest(app->video_device->stable_id);
    gchar *mode_key = capture_mode_key(g_ptr_array_index(app->modes, app->current_mode));
    g_hash_table_replace(app->mode_preferences, digest, mode_key);
    if (app->video_device->usb_vid == 0x345f &&
        app->video_device->usb_pid == 0x2130)
        g_clear_pointer(&app->legacy_mode_key, g_free);
    app_schedule_preference_save(app);
}

static const gchar *
preferred_mode_for_device(AppState *app)
{
    if (app->video_device == NULL)
        return NULL;
    gchar *digest = mode_preference_digest(app->video_device->stable_id);
    const gchar *mode_key = g_hash_table_lookup(app->mode_preferences, digest);
    g_free(digest);
    if (mode_key == NULL && app->legacy_mode_key != NULL &&
        app->video_device->usb_vid == 0x345f &&
        app->video_device->usb_pid == 0x2130)
        mode_key = app->legacy_mode_key;
    return mode_key;
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

static const gchar *
mode_unusable_reason(const CaptureVideoNode *node, const CaptureMode *mode)
{
    if (node == NULL || mode == NULL)
        return "capture node or mode is unavailable";
    GstCaps *caps = caps_for_mode(mode);
    if (caps == NULL)
        return "format has no GStreamer caps mapping";
    if (!mode->compressed) {
        gst_caps_unref(caps);
        return NULL;
    }
    if (is_mjpeg_mode(mode)) {
        GstElementFactory *jpeg_factory = gst_element_factory_find("jpegdec");
        gst_caps_unref(caps);
        if (jpeg_factory == NULL)
            return "jpegdec decoder plugin is unavailable";
        gst_object_unref(jpeg_factory);
        return NULL;
    }

    GstElementFactory *decodebin_factory = gst_element_factory_find("decodebin");
    if (decodebin_factory == NULL) {
        gst_caps_unref(caps);
        return "decodebin plugin is unavailable";
    }
    gst_object_unref(decodebin_factory);
    GList *decoders = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    GList *compatible = decoders != NULL
        ? gst_element_factory_list_filter(decoders, caps, GST_PAD_SINK, FALSE)
        : NULL;
    gst_caps_unref(caps);
    gboolean has_compatible_decoder = compatible != NULL;
    if (compatible != NULL)
        gst_plugin_feature_list_free(compatible);
    if (decoders != NULL)
        gst_plugin_feature_list_free(decoders);
    return has_compatible_decoder
        ? NULL : "no installed decoder accepts the advertised caps";
}

static gboolean
mode_is_usable(const CaptureVideoNode *node, const CaptureMode *mode)
{
    return mode_unusable_reason(node, mode) == NULL;
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

static GstPadProbeReturn
on_video_frame_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        AppState *app = user_data;
        g_mutex_lock(&app->stats_mutex);
        app->last_video_frame_us = g_get_monotonic_time();
        g_mutex_unlock(&app->stats_mutex);
    }
    (void)pad;
    return GST_PAD_PROBE_OK;
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
    g_object_set(source, "device", app->video_node->path, "io-mode", 2, NULL);
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
    GstPad *frame_pad = gst_element_get_static_pad(convert, "src");
    if (frame_pad != NULL) {
        gst_pad_add_probe(frame_pad, GST_PAD_PROBE_TYPE_BUFFER,
                          on_video_frame_buffer, app, NULL);
        gst_object_unref(frame_pad);
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
                    "Could not link the selected audio input to the default speaker sink");
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
    app->pipeline_started_us = 0;
    g_mutex_lock(&app->stats_mutex);
    app->current_fps = 0.0;
    app->average_fps = 0.0;
    app->frames_dropped = 0;
    app->queue_level = 0;
    app->last_video_frame_us = 0;
    app->audio_source_latency_us = -1;
    app->audio_source_buffer_us = -1;
    app->latency_min_ns = -1;
    app->latency_max_ns = -1;
    g_mutex_unlock(&app->stats_mutex);
}



static gboolean
refresh_audio_source(AppState *app)
{
    gchar *fresh_id = NULL;
    gchar *fresh_status = NULL;
    GstDevice *fresh_audio =
        resolve_selected_audio_device(app, &fresh_id, &fresh_status);
    gboolean audio_changed =
        g_strcmp0(fresh_id, app->audio_device_id) != 0;
    if (g_strcmp0(fresh_status, app->audio_selection_status) != 0) {
        g_free(app->audio_selection_status);
        app->audio_selection_status = g_strdup(fresh_status);
        app_log(app, "Audio input selection: %s",
                fresh_status != NULL ? fresh_status : "status unavailable");
    }
    if (audio_changed) {
        if (app->pipeline != NULL && app->audio_enabled)
            pipeline_stop(app);
        if (app->audio_device != NULL)
            gst_object_unref(app->audio_device);
        app->audio_device = fresh_audio;
        fresh_audio = NULL;
        g_free(app->audio_device_id);
        app->audio_device_id = fresh_id;
        fresh_id = NULL;
        g_free(app->audio_display_name);
        app->audio_display_name = app->audio_device != NULL
            ? gst_device_get_display_name(app->audio_device) : NULL;
        if (app->audio_device != NULL)
            app_log(app, "Selected audio input: %s [%s]",
                    app->audio_display_name != NULL
                        ? app->audio_display_name : "unnamed",
                    app->audio_device_id);
        else
            app_log(app, "No audio input is currently routed");
    }
    if (fresh_audio != NULL)
        gst_object_unref(fresh_audio);
    g_free(fresh_id);
    g_free(fresh_status);
    app_refresh_ui(app);
    return audio_changed;
}
static gchar *
element_current_caps(GstElement *element, const gchar *pad_name)
{
    if (element == NULL)
        return g_strdup("element unavailable");
    GstPad *pad = gst_element_get_static_pad(element, pad_name);
    if (pad == NULL)
        return g_strdup("pad unavailable");
    GstCaps *caps = gst_pad_get_current_caps(pad);
    gst_object_unref(pad);
    if (caps == NULL)
        return g_strdup("not negotiated");
    gchar *text = gst_caps_to_string(caps);
    gst_caps_unref(caps);
    return text;
}

static const gchar *
video_buffer_type_name(enum v4l2_buf_type type)
{
    switch (type) {
    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
        return "VIDEO_CAPTURE";
    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
        return "VIDEO_CAPTURE_MPLANE";
    default:
        return "unknown";
    }
}

static const gchar *
selected_decoder_description(const CaptureMode *mode)
{
    if (mode == NULL)
        return "unavailable";
    if (!mode->compressed)
        return "none (raw capture)";
    return is_mjpeg_mode(mode) ? "jpegdec" : "decodebin (dynamic selection)";
}

static gchar *
capture_diagnostic_context(AppState *app)
{
    CaptureMode *mode = app->modes != NULL &&
        app->current_mode < app->modes->len
        ? g_ptr_array_index(app->modes, app->current_mode) : NULL;
    CaptureVideoNode *node = app->video_node;
    gchar *mode_key = mode != NULL ? capture_mode_key(mode)
                                   : g_strdup("unavailable");
    GstCaps *expected_caps = mode != NULL ? caps_for_mode(mode) : NULL;
    gchar *expected_text = expected_caps != NULL
        ? gst_caps_to_string(expected_caps) : g_strdup("unavailable");
    if (expected_caps != NULL)
        gst_caps_unref(expected_caps);
    gchar *negotiated = element_current_caps(app->video_convert, "sink");
    const gchar *renderer = app->renderer != NULL
        ? capture_renderer_get_backend_name(app->renderer) : "unavailable";
    gchar *context = g_strdup_printf(
        "device-id=%s physical=%s usb-parent=%s serial=%s node=%s card=%s "
        "driver=%s bus-info=%s capabilities=0x%08x buffer-type=%s "
        "mode=%s [%s] decoder=%s renderer=%s expected-caps=%s "
        "negotiated-videoconvert-sink=%s audio-source-id=%s audio-status=%s",
        app->video_device != NULL && app->video_device->stable_id != NULL
            ? app->video_device->stable_id : "unavailable",
        app->video_device != NULL && app->video_device->physical_sysfs != NULL
            ? app->video_device->physical_sysfs : "unavailable",
        app->video_device != NULL && app->video_device->usb_sysfs != NULL
            ? app->video_device->usb_sysfs : "non-USB",
        app->video_device != NULL && app->video_device->serial != NULL
            ? app->video_device->serial : "unavailable",
        node != NULL && node->path != NULL ? node->path : "unavailable",
        node != NULL && node->card_name != NULL ? node->card_name : "unavailable",
        node != NULL && node->driver != NULL ? node->driver : "unavailable",
        node != NULL && node->bus_info != NULL ? node->bus_info : "unavailable",
        node != NULL ? node->capabilities : 0,
        node != NULL ? video_buffer_type_name(node->buffer_type) : "unavailable",
        mode != NULL ? mode->label : "unavailable", mode_key,
        selected_decoder_description(mode), renderer, expected_text, negotiated,
        app->audio_device_id != NULL ? app->audio_device_id : "unavailable",
        app->audio_selection_status != NULL
            ? app->audio_selection_status : "unavailable");
    g_free(mode_key);
    g_free(expected_text);
    g_free(negotiated);
    return context;
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
        gchar *context = capture_diagnostic_context(app);
        const gchar *domain = error != NULL ? g_quark_to_string(error->domain) : NULL;
        app_log(app,
                "GStreamer ERROR domain=%s code=%d element=%s type=%s message=%s "
                "debug=%s; %s",
                domain != NULL ? domain : "unknown",
                error != NULL ? error->code : -1,
                GST_OBJECT_NAME(message->src),
                G_OBJECT_TYPE_NAME(message->src),
                error != NULL ? error->message : "unknown error",
                debug != NULL ? debug : "unavailable", context);
        g_free(app->pipeline_error);
        app->pipeline_error = g_strdup_printf(
            "%s (domain=%s, code=%d, element=%s)\n%s\nDebug: %s",
            error != NULL ? error->message : "GStreamer pipeline error",
            domain != NULL ? domain : "unknown",
            error != NULL ? error->code : -1,
            GST_OBJECT_NAME(message->src), context,
            debug != NULL ? debug : "unavailable");
        g_free(context);
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
        gchar *context = capture_diagnostic_context(app);
        app_log(app, "GStreamer WARNING element=%s message=%s debug=%s; %s",
                GST_OBJECT_NAME(message->src),
                error != NULL ? error->message : "unknown warning",
                debug != NULL ? debug : "unavailable", context);
        g_free(context);
        if (error != NULL)
            g_error_free(error);
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
video_waiting_for_frames(AppState *app)
{
    if (app->pipeline == NULL || app->pipeline_started_us == 0)
        return FALSE;
    g_mutex_lock(&app->stats_mutex);
    gint64 last_frame_us = app->last_video_frame_us;
    g_mutex_unlock(&app->stats_mutex);
    gint64 last_activity_us = last_frame_us > app->pipeline_started_us
        ? last_frame_us : app->pipeline_started_us;
    return g_get_monotonic_time() - last_activity_us >= 2 * G_USEC_PER_SEC;
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
        gchar *context = capture_diagnostic_context(app);
        const gchar *domain = error != NULL ? g_quark_to_string(error->domain) : NULL;
        app_log(app, "Pipeline setup failed: %s; domain=%s code=%d; %s",
                error != NULL ? error->message : "unknown error",
                domain != NULL ? domain : "unknown",
                error != NULL ? error->code : -1, context);
        g_free(app->pipeline_error);
        app->pipeline_error = g_strdup_printf(
            "%s (domain=%s, code=%d)\n%s",
            error != NULL ? error->message : "Pipeline setup failed",
            domain != NULL ? domain : "unknown",
            error != NULL ? error->code : -1, context);
        g_free(context);
        if (error != NULL)
            g_error_free(error);
        pipeline_stop(app);
        app_refresh_ui(app);
        return FALSE;
    }

    app->pipeline_bus = gst_element_get_bus(app->pipeline);
    app->bus_watch_id = gst_bus_add_watch(app->pipeline_bus, pipeline_bus_message, app);
    GstStateChangeReturn state = gst_element_set_state(app->pipeline, GST_STATE_PLAYING);
    if (state == GST_STATE_CHANGE_FAILURE) {
        GstState current = GST_STATE_VOID_PENDING;
        GstState pending = GST_STATE_VOID_PENDING;
        GstStateChangeReturn observed =
            gst_element_get_state(app->pipeline, &current, &pending, 0);
        gchar *context = capture_diagnostic_context(app);
        app_log(app,
                "GStreamer refused PLAYING for %s (state=%s pending=%s "
                "query-result=%d); %s",
                mode->label, gst_element_state_get_name(current),
                gst_element_state_get_name(pending), observed, context);
        g_free(app->pipeline_error);
        app->pipeline_error = g_strdup_printf(
            "GStreamer refused PLAYING (state=%s, pending=%s, query-result=%d)\n%s",
            gst_element_state_get_name(current), gst_element_state_get_name(pending),
            observed, context);
        g_free(context);
        app->retry_after_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        pipeline_stop(app);
        app_refresh_ui(app);
        return FALSE;
    }
    app->pipeline_started_us = g_get_monotonic_time();

    app_log_renderer_backend(app);
    g_free(app->pipeline_error);
    app->pipeline_error = NULL;
    app_log(app, "Started %s on %s; audio %s%s", mode->label,
            app->video_node->path,
            app->audio_enabled && app->audio_device != NULL ? "enabled" : "disabled",
            app->audio_enabled && app->audio_device == NULL ? " (no audio source matched)" : "");
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
            if (mode_is_usable(node, g_ptr_array_index(modes, mode_index))) {
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
                if (!mode_is_usable(node, item)) {
                    const gchar *reason = mode_unusable_reason(node, item);
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
    populate_mode_selectors(app);
    app_refresh_ui(app);
}

static void
replace_video_devices(AppState *app, GPtrArray *fresh_devices)
{
    const gchar *desired_id = app->selected_video_device_id;
    const gchar *desired_parent = app->selected_video_parent_path;
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
        g_clear_pointer(&app->legacy_mode_key, g_free);

    CaptureVideoNode *fresh_node = fresh_device != NULL
        ? find_video_node_by_interface(fresh_device, app->selected_node_interface) : NULL;
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
            migrate_mode_preference(app, desired_id, fresh_device->stable_id);
        gboolean selection_changed =
            g_strcmp0(app->selected_video_device_id, fresh_device->stable_id) != 0 ||
            g_strcmp0(app->selected_video_parent_path,
                      fresh_device->physical_sysfs) != 0;
        g_free(app->selected_video_device_id);
        app->selected_video_device_id = g_strdup(fresh_device->stable_id);
        g_free(app->selected_video_parent_path);
        app->selected_video_parent_path = g_strdup(fresh_device->physical_sysfs);
        if (fresh_node != NULL) {
            g_free(app->selected_node_interface);
            app->selected_node_interface = g_strdup(fresh_node->interface_sysfs);
        }
        if (selection_changed)
            app_schedule_preference_save(app);
    }

    GPtrArray *old_devices = app->video_devices;
    app->video_devices = fresh_devices;
    set_video_source(app, fresh_device, fresh_node);
    populate_video_devices(app);
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
        app->pipeline == NULL && g_get_monotonic_time() >= app->retry_after_us)
        pipeline_start(app);
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
            gchar *device_class = gst_device_get_device_class(device);
            gchar *identity = audio_device_identity(device, NULL);
            GstStructure *properties = gst_device_get_properties(device);
            gchar *property_text = properties != NULL
                ? gst_structure_to_string(properties) : g_strdup("unavailable");
            app_log(app, "GStreamer device %s: name=%s class=%s identity=%s properties=%s",
                    GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_ADDED ? "added" : "removed",
                    display_name, device_class != NULL ? device_class : "unknown",
                    identity, property_text);
            if (properties != NULL)
                gst_structure_free(properties);
            g_free(display_name);
            g_free(device_class);
            g_free(identity);
            g_free(property_text);
            gst_object_unref(device);
        }
        populate_audio_selector(app);
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
    gboolean waiting_for_frames = video_waiting_for_frames(app);
    gboolean show_diagnostics =
        app->settings != NULL && gtk_widget_get_visible(app->settings);
    CaptureMode *mode = app->modes != NULL &&
        app->current_mode < app->modes->len
        ? g_ptr_array_index(app->modes, app->current_mode) : NULL;
    const gchar *mode_name = mode != NULL ? mode->label : "No capture mode";
    if (show_diagnostics) {

    gchar *device_text;
    if (app->video_device != NULL && app->video_node != NULL) {
        CaptureDevice *device = app->video_device;
        CaptureVideoNode *node = app->video_node;
        gchar *usb_ids = device->usb_sysfs != NULL
            ? g_strdup_printf("%04x:%04x", device->usb_vid, device->usb_pid)
            : g_strdup("not USB");
        device_text = g_strdup_printf(
            "Name: %s\nStable ID: %s\nPhysical sysfs: %s\nUSB parent: %s\n"
            "USB IDs: %s · serial: %s\nNode: %s\nV4L2 card: %s\n"
            "Driver: %s · bus: %s\nInterface sysfs: %s\n"
            "Capabilities: 0x%08x · buffer type: %s\nUsable modes: %u",
            device->display_name != NULL ? device->display_name : "Video capture device",
            device->stable_id != NULL ? device->stable_id : "unavailable",
            device->physical_sysfs != NULL ? device->physical_sysfs : "unavailable",
            device->usb_sysfs != NULL ? device->usb_sysfs : "not USB",
            usb_ids, device->serial != NULL ? device->serial : "unavailable",
            node->path != NULL ? node->path : "unavailable",
            node->card_name != NULL ? node->card_name : "unavailable",
            node->driver != NULL ? node->driver : "unavailable",
            node->bus_info != NULL ? node->bus_info : "unavailable",
            node->interface_sysfs != NULL ? node->interface_sysfs : "unavailable",
            node->capabilities, video_buffer_type_name(node->buffer_type),
            app->modes != NULL ? app->modes->len : 0);
        g_free(usb_ids);
    } else if (app->video_device != NULL) {
        device_text = g_strdup_printf(
            "Name: %s\nStable ID: %s\nPhysical sysfs: %s\n"
            "Connected, but no usable video interface is selected",
            app->video_device->display_name != NULL
                ? app->video_device->display_name : "Video capture device",
            app->video_device->stable_id != NULL
                ? app->video_device->stable_id : "unavailable",
            app->video_device->physical_sysfs != NULL
                ? app->video_device->physical_sysfs : "unavailable");
    } else if (app->selected_video_device_id != NULL) {
        device_text = g_strdup_printf(
            "Saved video source unavailable: %s\nWaiting for reconnection or another source",
            app->selected_video_device_id);
    } else {
        device_text = g_strdup(
            "No video source selected. Connect a USB capture device or choose a source.");
    }
    if (app->device_label != NULL)
        gtk_label_set_text(GTK_LABEL(app->device_label), device_text);

    gchar *mode_text;
    if (mode != NULL) {
        gchar *mode_key = capture_mode_key(mode);
        GstCaps *expected_caps = caps_for_mode(mode);
        gchar *expected_text = expected_caps != NULL
            ? gst_caps_to_string(expected_caps) : g_strdup("unavailable");
        if (expected_caps != NULL)
            gst_caps_unref(expected_caps);
        gchar *negotiated_text = element_current_caps(app->video_convert, "sink");
        mode_text = g_strdup_printf(
            "Mode: %s\nExact mode ID: %s\nExpected capture caps: %s\n"
            "Negotiated videoconvert input caps: %s",
            mode->label, mode_key, expected_text, negotiated_text);
        g_free(mode_key);
        g_free(expected_text);
        g_free(negotiated_text);
    } else {
        mode_text = g_strdup("No usable capture mode is selected");
    }
    if (app->mode_diagnostic_label != NULL)
        gtk_label_set_text(GTK_LABEL(app->mode_diagnostic_label), mode_text);

    const gchar *renderer = app->renderer != NULL
        ? capture_renderer_get_backend_name(app->renderer) : "unavailable";
    gchar *renderer_text = g_strdup_printf("Renderer: %s\nDecoder path: %s",
        renderer, selected_decoder_description(mode));
    if (app->renderer_label != NULL)
        gtk_label_set_text(GTK_LABEL(app->renderer_label), renderer_text);
    g_free(device_text);
    g_free(mode_text);
    g_free(renderer_text);
    }

    if (app->audio_toggle != NULL) {
        gtk_widget_set_sensitive(app->audio_toggle,
                                 g_strcmp0(app->audio_selection_id, "none") != 0);
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->audio_toggle)) !=
            app->audio_enabled) {
            app->updating_controls = TRUE;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->audio_toggle),
                                         app->audio_enabled);
            app->updating_controls = FALSE;
        }
    }
    if (app->volume_scale != NULL)
        gtk_widget_set_sensitive(app->volume_scale,
                                 app->audio_enabled && app->audio_device != NULL);

    gchar *overlay_text = NULL;
    gchar *perf = NULL;
    gchar *latency_text = NULL;
    gchar *audio_stats = NULL;
    g_mutex_lock(&app->stats_mutex);
    if (show_diagnostics) {
        latency_text = app->latency_min_ns >= 0
            ? (app->latency_max_ns >= 0
                ? g_strdup_printf("Pipeline-reported latency %.1f–%.1f ms (not end-to-end)",
                    app->latency_min_ns / 1000000.0,
                    app->latency_max_ns / 1000000.0)
                : g_strdup_printf("Pipeline-reported latency ≥ %.1f ms (not end-to-end)",
                    app->latency_min_ns / 1000000.0))
            : g_strdup("Pipeline-reported latency unavailable (not end-to-end)");
        perf = g_strdup_printf("FPS %.1f avg %.1f · dropped %" G_GUINT64_FORMAT
            " · queue %" G_GUINT64_FORMAT " · CPU %.1f%% · %s",
            app->current_fps, app->average_fps, app->frames_dropped,
            app->queue_level, app->cpu_percent, latency_text);
        audio_stats =
            app->audio_device != NULL && app->audio_enabled && app->audio_source != NULL
            ? (app->audio_source_latency_us >= 0 &&
               app->audio_source_buffer_us >= 0
                ? g_strdup_printf("Source timing: %.1f ms latency / %.1f ms buffer",
                    app->audio_source_latency_us / 1000.0,
                    app->audio_source_buffer_us / 1000.0)
                : g_strdup("Source timing: unavailable"))
            : g_strdup("Source timing: unavailable");
    }
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
            mode_name, app->current_fps, app->average_fps,
            app->frames_dropped, app->cpu_percent, overlay_latency);
        g_free(overlay_latency);
    }
    g_mutex_unlock(&app->stats_mutex);
    if (show_diagnostics) {
        if (app->perf_label != NULL)
            gtk_label_set_text(GTK_LABEL(app->perf_label), perf);

        gchar *sink_display =
            app->audio_sink_name != NULL && *app->audio_sink_name != '\0'
            ? (app->audio_sink_id != NULL && *app->audio_sink_id != '\0'
                ? g_strdup_printf("%s (%s)", app->audio_sink_name, app->audio_sink_id)
                : g_strdup(app->audio_sink_name))
            : (app->audio_sink_id != NULL && *app->audio_sink_id != '\0'
                ? g_strdup(app->audio_sink_id) : NULL);
        const gchar *policy =
            app->audio_selection_id == NULL ||
            g_str_equal(app->audio_selection_id, "auto") ? "Auto" :
            g_str_equal(app->audio_selection_id, "none") ? "None" : "Explicit source";
        const gchar *input_name = app->audio_display_name != NULL
            ? app->audio_display_name : "No audio input";
        const gchar *input_identity = app->audio_device_id != NULL
            ? app->audio_device_id : "unavailable";
        gchar *route_text;
        if (!app->audio_enabled)
            route_text = g_strdup("Audio is disabled");
        else if (app->audio_device == NULL)
            route_text = g_strdup(app->audio_selection_status != NULL
                ? app->audio_selection_status : "No selected audio input is available");
        else if (app->audio_source == NULL)
            route_text = g_strdup(
                "Input detected; playback inactive because capture pipeline is stopped");
        else if (sink_display != NULL)
            route_text = g_strdup_printf("PulseAudio default output: %s", sink_display);
        else
            route_text = g_strdup("PulseAudio default output unavailable");
        gchar *audio_route = g_strdup_printf(
            "Policy: %s\nInput: %s\nInput identity: %s\nStatus: %s\n%s\n%s",
            policy, input_name, input_identity,
            app->audio_selection_status != NULL ? app->audio_selection_status
                                                : "No audio status available",
            route_text, audio_stats);
        if (app->audio_route_label != NULL)
            gtk_label_set_text(GTK_LABEL(app->audio_route_label), audio_route);

        const gchar *status_base;
        if (app->pipeline != NULL && waiting_for_frames)
            status_base = "Pipeline active; waiting for video frames (signal status unavailable)";
        else if (app->pipeline != NULL)
            status_base = "Capture pipeline active";
        else if (app->pipeline_error != NULL)
            status_base = "Capture pipeline stopped after an error";
        else if (app->video_device == NULL && app->selected_video_device_id != NULL)
            status_base = "Saved video source is unavailable";
        else if (app->video_device == NULL)
            status_base = "No video capture source selected";
        else if (app->modes == NULL || app->modes->len == 0)
            status_base = "No usable video mode selected";
        else
            status_base = "Capture pipeline is not running";
        gint64 remaining_us = app->retry_after_us - g_get_monotonic_time();
        gchar *status_text = remaining_us > 0
            ? g_strdup_printf("%s; retry scheduled in %" G_GINT64_FORMAT " ms",
                              status_base, (remaining_us + 999) / 1000)
            : g_strdup(status_base);
        if (app->status_label != NULL)
            gtk_label_set_text(GTK_LABEL(app->status_label), status_text);
        if (app->pipeline_error_label != NULL)
            gtk_label_set_text(GTK_LABEL(app->pipeline_error_label),
                app->pipeline_error != NULL
                    ? app->pipeline_error : "No pipeline error recorded");
        if (app->log_path_label != NULL)
            gtk_label_set_text(GTK_LABEL(app->log_path_label),
                app->log_path != NULL ? app->log_path : "Log path unavailable");

        g_free(sink_display);
        g_free(route_text);
        g_free(audio_route);
        g_free(status_text);
    }
    g_free(perf);
    g_free(audio_stats);
    g_free(latency_text);
    if (app->stats_overlay_label != NULL)
        gtk_label_set_text(GTK_LABEL(app->stats_overlay_label), overlay_text);

    if (app->status_overlay_label != NULL && app->status_overlay != NULL) {
        const gchar *message = NULL;
        if (app->video_device == NULL && app->selected_video_device_id != NULL)
            message = "Saved source unavailable\nReconnect it or choose another source.";
        else if (app->video_device == NULL)
            message = "Waiting for a USB video capture source";
        else if (app->pipeline_error != NULL)
            message = "Capture error\nOpen Advanced for the error, debug trace, device, and caps.";
        else if (app->modes == NULL || app->modes->len == 0)
            message = "No usable capture mode\nOpen Advanced for decoder and caps details.";
        else if (waiting_for_frames)
            message = "Waiting for video frames\nCapture signal status is unavailable.";
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

    g_free(overlay_text);
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
    populate_mode_selectors(app);
    app_refresh_ui(app);
    app_restart_pipeline(app);
}

static void
format_changed(GtkComboBox *combo, gpointer user_data)
{
    AppState *app = user_data;
    const gchar *format_id = gtk_combo_box_get_active_id(combo);
    if (app->updating_controls || format_id == NULL || app->modes == NULL ||
        app->modes->len == 0)
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
resolution_changed(GtkComboBox *combo, gpointer user_data)
{
    AppState *app = user_data;
    const gchar *resolution_id = gtk_combo_box_get_active_id(combo);
    if (app->updating_controls || resolution_id == NULL || app->modes == NULL ||
        app->modes->len == 0)
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
fps_changed(GtkComboBox *combo, gpointer user_data)
{
    AppState *app = user_data;
    const gchar *mode_key = gtk_combo_box_get_active_id(combo);
    if (app->updating_controls || mode_key == NULL || app->modes == NULL)
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
video_source_changed(GtkComboBox *combo, gpointer user_data)
{
    AppState *app = user_data;
    const gchar *device_id = gtk_combo_box_get_active_id(combo);
    if (app->updating_controls || device_id == NULL)
        return;
    CaptureDevice *device =
        capture_device_find_by_id(app->video_devices, device_id);
    if (device == NULL) {
        g_free(app->selected_video_device_id);
        app->selected_video_device_id = g_strdup(device_id);
        app_schedule_preference_save(app);
        set_video_source(app, NULL, NULL);
        refresh_audio_source(app);
        app_restart_pipeline(app);
        return;
    }
    gboolean same_physical_device = app->video_device != NULL &&
        g_strcmp0(app->video_device->physical_sysfs, device->physical_sysfs) == 0;
    CaptureVideoNode *node = same_physical_device
        ? find_video_node_by_interface(device, app->selected_node_interface) : NULL;
    if (node == NULL)
        node = first_video_node(device);
    if (device->usb_vid != 0x345f || device->usb_pid != 0x2130)
        g_clear_pointer(&app->legacy_mode_key, g_free);
    g_free(app->selected_video_device_id);
    app->selected_video_device_id = g_strdup(device->stable_id);
    g_free(app->selected_video_parent_path);
    app->selected_video_parent_path = g_strdup(device->physical_sysfs);
    g_free(app->selected_node_interface);
    app->selected_node_interface = node != NULL
        ? g_strdup(node->interface_sysfs) : NULL;
    app_schedule_preference_save(app);
    set_video_source(app, device, node);
    refresh_audio_source(app);
    populate_video_devices(app);
    app_restart_pipeline(app);
}

static void
video_node_changed(GtkComboBox *combo, gpointer user_data)
{
    AppState *app = user_data;
    const gchar *interface_id = gtk_combo_box_get_active_id(combo);
    if (app->updating_controls || interface_id == NULL || app->video_device == NULL)
        return;
    CaptureVideoNode *node =
        find_video_node_by_interface(app->video_device, interface_id);
    if (node == NULL || node == app->video_node)
        return;
    g_free(app->selected_node_interface);
    app->selected_node_interface = g_strdup(node->interface_sysfs);
    app_schedule_preference_save(app);
    set_video_source(app, app->video_device, node);
    app_restart_pipeline(app);
}

static void
advanced_sources_toggled(GtkToggleButton *button, gpointer user_data)
{
    AppState *app = user_data;
    if (app->updating_controls)
        return;
    app->include_advanced_sources = gtk_toggle_button_get_active(button);
    app_schedule_preference_save(app);
    populate_video_devices(app);
}

static void
audio_selection_changed(GtkComboBox *combo, gpointer user_data)
{
    AppState *app = user_data;
    const gchar *selection = gtk_combo_box_get_active_id(combo);
    if (app->updating_controls || selection == NULL)
        return;
    g_free(app->audio_selection_id);
    app->audio_selection_id = g_strdup(selection);
    app->audio_selection_session_only = g_str_has_prefix(selection, "session:");
    app_schedule_preference_save(app);
    gboolean audio_changed = refresh_audio_source(app);
    if (audio_changed && app->audio_enabled)
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
    app_log(app, "Capture audio %s", app->audio_enabled ? "enabled" : "disabled");
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
        app_refresh_ui(app);
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
        if (app->mode_popup_open) {
            GtkWidget *selectors[] = {
                app->source_combo, app->node_combo, app->format_combo,
                app->resolution_combo, app->fps_combo
            };
            for (guint i = 0; i < G_N_ELEMENTS(selectors); i++) {
                if (selectors[i] != NULL)
                    gtk_combo_box_popdown(GTK_COMBO_BOX(selectors[i]));
            }
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

static GtkWidget *
diagnostic_value_label(void)
{
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_hexpand(label, TRUE);
    return label;
}

static void
attach_diagnostic_row(GtkGrid *grid, const gchar *title, GtkWidget *value, gint row)
{
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_valign(label, GTK_ALIGN_START);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_grid_attach(grid, value, 1, row, 2, 1);
}

static void
create_settings(AppState *app)
{
    app->settings = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(app->settings), "Advanced Capture Diagnostics");
    gtk_window_set_transient_for(GTK_WINDOW(app->settings), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(app->settings), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(app->settings), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(app->settings), 820, 740);
    g_signal_connect(app->settings, "delete-event", G_CALLBACK(close_settings), app);
    g_signal_connect(app->settings, "key-press-event", G_CALLBACK(key_press), app);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(app->settings));
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 24);
    gtk_widget_set_size_request(grid, 720, -1);
    gtk_container_add(GTK_CONTAINER(scroll), grid);

    app->device_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Capture device", app->device_label, 0);
    app->mode_diagnostic_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Capture mode", app->mode_diagnostic_label, 1);
    app->renderer_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Renderer / decoder", app->renderer_label, 2);
    app->perf_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Capture statistics", app->perf_label, 3);
    app->status_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Pipeline status", app->status_label, 4);
    app->audio_route_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Audio route", app->audio_route_label, 5);
    app->pipeline_error_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Latest pipeline error",
                          app->pipeline_error_label, 6);
    app->log_path_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Log file", app->log_path_label, 7);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Show controls after"), 0, 8, 1, 1);
    app->dwell_spin = gtk_spin_button_new_with_range(100, 250, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->dwell_spin), app->panel_dwell_ms);
    gtk_grid_attach(GTK_GRID(grid), app->dwell_spin, 1, 8, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("ms pointer dwell"), 2, 8, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Hide controls after"), 0, 9, 1, 1);
    app->hide_delay_spin = gtk_spin_button_new_with_range(500, 1000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->hide_delay_spin), app->panel_hide_delay_ms);
    gtk_grid_attach(GTK_GRID(grid), app->hide_delay_spin, 1, 9, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("ms outside panel"), 2, 9, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Video scaling"), 0, 10, 1, 1);
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
    gtk_grid_attach(GTK_GRID(grid), app->scale_combo, 1, 10, 2, 1);
    g_signal_connect(app->scale_combo, "changed",
                     G_CALLBACK(scaling_mode_changed), app);

    app->advanced_sources_check =
        gtk_check_button_new_with_label("Show internal/virtual video sources");
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(app->advanced_sources_check), app->include_advanced_sources);
    gtk_grid_attach(GTK_GRID(grid), app->advanced_sources_check, 0, 11, 3, 1);
    g_signal_connect(app->advanced_sources_check, "toggled",
                     G_CALLBACK(advanced_sources_toggled), app);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Audio input"), 0, 12, 1, 1);
    app->audio_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(app->audio_combo, 420, 48);
    gtk_grid_attach(GTK_GRID(grid), app->audio_combo, 1, 12, 2, 1);
    g_signal_connect(app->audio_combo, "changed",
                     G_CALLBACK(audio_selection_changed), app);
    populate_audio_selector(app);
    g_signal_connect(app->dwell_spin, "value-changed", G_CALLBACK(dwell_changed), app);
    g_signal_connect(app->hide_delay_spin, "value-changed", G_CALLBACK(hide_delay_changed), app);

    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(close, 120, 48);
    gtk_box_pack_end(GTK_BOX(content), close, FALSE, FALSE, 12);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(hide_settings), app);
    app_refresh_ui(app);
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
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    GtkWidget *panel_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(panel_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(panel_scroll),
                                        GTK_SHADOW_NONE);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(panel_scroll), 220);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(panel_scroll), TRUE);
    gtk_container_add(GTK_CONTAINER(panel_scroll), outer);
    gtk_container_add(GTK_CONTAINER(panel), panel_scroll);
    GtkWidget *selectors = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(selectors), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(selectors), FALSE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(selectors), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(selectors), 5);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(selectors), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(selectors), 8);
    gtk_widget_set_hexpand(selectors, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), selectors, FALSE, TRUE, 0);
    GtkWidget *controls = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(controls), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(controls), FALSE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(controls), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(controls), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(controls), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(controls), 8);
    gtk_widget_set_hexpand(controls, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), controls, FALSE, TRUE, 0);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "eventbox { background-color: rgba(12, 15, 20, 0.88); border-radius: 12px; } "
        "button, combobox, checkbutton { min-height: 44px; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(panel),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *selector_label = gtk_label_new("Source");
    gtk_widget_set_halign(selector_label, GTK_ALIGN_START);
    GtkWidget *selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    app->source_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(app->source_combo, 230, 48);
    gtk_box_pack_start(GTK_BOX(selector), selector_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), app->source_combo, FALSE, FALSE, 0);
    gtk_flow_box_insert(GTK_FLOW_BOX(selectors), selector, -1);
    g_signal_connect(app->source_combo, "changed",
                     G_CALLBACK(video_source_changed), app);
    g_signal_connect(app->source_combo, "notify::popup-shown",
                     G_CALLBACK(mode_popup_notify), app);

    selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    selector_label = gtk_label_new("Video interface");
    gtk_widget_set_halign(selector_label, GTK_ALIGN_START);
    app->node_selector = selector;
    app->node_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(app->node_combo, 190, 48);
    gtk_box_pack_start(GTK_BOX(selector), selector_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), app->node_combo, FALSE, FALSE, 0);
    gtk_flow_box_insert(GTK_FLOW_BOX(selectors), selector, -1);
    g_signal_connect(app->node_combo, "changed",
                     G_CALLBACK(video_node_changed), app);
    g_signal_connect(app->node_combo, "notify::popup-shown",
                     G_CALLBACK(mode_popup_notify), app);

    selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    selector_label = gtk_label_new("Format");
    gtk_widget_set_halign(selector_label, GTK_ALIGN_START);
    app->format_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(app->format_combo, 150, 48);
    gtk_box_pack_start(GTK_BOX(selector), selector_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), app->format_combo, FALSE, FALSE, 0);
    gtk_flow_box_insert(GTK_FLOW_BOX(selectors), selector, -1);
    g_signal_connect(app->format_combo, "changed",
                     G_CALLBACK(format_changed), app);
    g_signal_connect(app->format_combo, "notify::popup-shown",
                     G_CALLBACK(mode_popup_notify), app);

    selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    selector_label = gtk_label_new("Resolution");
    gtk_widget_set_halign(selector_label, GTK_ALIGN_START);
    app->resolution_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(app->resolution_combo, 170, 48);
    gtk_box_pack_start(GTK_BOX(selector), selector_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), app->resolution_combo, FALSE, FALSE, 0);
    gtk_flow_box_insert(GTK_FLOW_BOX(selectors), selector, -1);
    g_signal_connect(app->resolution_combo, "changed",
                     G_CALLBACK(resolution_changed), app);
    g_signal_connect(app->resolution_combo, "notify::popup-shown",
                     G_CALLBACK(mode_popup_notify), app);

    selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    selector_label = gtk_label_new("Frame rate");
    gtk_widget_set_halign(selector_label, GTK_ALIGN_START);
    app->fps_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(app->fps_combo, 130, 48);
    gtk_box_pack_start(GTK_BOX(selector), selector_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), app->fps_combo, FALSE, FALSE, 0);
    gtk_flow_box_insert(GTK_FLOW_BOX(selectors), selector, -1);
    g_signal_connect(app->fps_combo, "changed", G_CALLBACK(fps_changed), app);
    g_signal_connect(app->fps_combo, "notify::popup-shown",
                     G_CALLBACK(mode_popup_notify), app);

    GtkWidget *pin = gtk_toggle_button_new_with_label("Pin");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pin), app->pinned);
    gtk_button_set_label(GTK_BUTTON(pin), app->pinned ? "Pinned" : "Pin");
    gtk_widget_set_size_request(pin, 72, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), pin, -1);
    g_signal_connect(pin, "toggled", G_CALLBACK(pin_toggled), app);
    app->audio_toggle = gtk_check_button_new_with_label("Audio");
    gtk_widget_set_size_request(app->audio_toggle, 140, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), app->audio_toggle, -1);
    g_signal_connect(app->audio_toggle, "toggled", G_CALLBACK(audio_toggled), app);
    GtkWidget *volume_label = gtk_label_new("Volume");
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), volume_label, -1);
    app->volume_scale =
        gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
    gtk_widget_set_size_request(app->volume_scale, 140, 48);
    gtk_range_set_value(GTK_RANGE(app->volume_scale), app->volume);
    gtk_scale_set_draw_value(GTK_SCALE(app->volume_scale), TRUE);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), app->volume_scale, -1);
    gtk_widget_add_events(app->volume_scale,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(app->volume_scale, "button-press-event",
                     G_CALLBACK(slider_press), app);
    g_signal_connect(app->volume_scale, "button-release-event",
                     G_CALLBACK(slider_release), app);
    g_signal_connect(app->volume_scale, "value-changed",
                     G_CALLBACK(volume_changed), app);
    GtkWidget *stats = gtk_toggle_button_new_with_label("Stats");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(stats), app->stats_visible);
    gtk_widget_set_size_request(stats, 88, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), stats, -1);
    g_signal_connect(stats, "toggled", G_CALLBACK(stats_toggled), app);
    GtkWidget *advanced = gtk_button_new_with_label("Advanced");
    gtk_widget_set_size_request(advanced, 112, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), advanced, -1);
    g_signal_connect_swapped(advanced, "clicked", G_CALLBACK(show_settings), app);
    GtkWidget *full = gtk_button_new_with_label("Fullscreen");
    gtk_widget_set_size_request(full, 120, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), full, -1);
    g_signal_connect_swapped(full, "clicked", G_CALLBACK(toggle_fullscreen), app);
    GtkWidget *quit = gtk_button_new_with_label("Quit");
    gtk_widget_set_size_request(quit, 76, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), quit, -1);
    g_signal_connect_swapped(quit, "clicked", G_CALLBACK(g_application_quit),
                             app->application);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->root_overlay), panel);
    gtk_widget_set_halign(panel, GTK_ALIGN_FILL);
    gtk_widget_set_valign(panel, GTK_ALIGN_START);
    app->stats_overlay_box = gtk_event_box_new();
    gtk_widget_set_halign(app->stats_overlay_box, GTK_ALIGN_START);
    gtk_widget_set_valign(app->stats_overlay_box, GTK_ALIGN_END);
    gtk_widget_set_hexpand(app->stats_overlay_box, FALSE);
    gtk_widget_set_vexpand(app->stats_overlay_box, FALSE);
    gtk_widget_set_margin_start(app->stats_overlay_box, 22);
    gtk_widget_set_margin_end(app->stats_overlay_box, 22);
    gtk_widget_set_margin_bottom(app->stats_overlay_box, 22);
    GtkWidget *stats_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(stats_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(stats_scroll),
                                        GTK_SHADOW_NONE);
    gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(stats_scroll),
                                              680);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(stats_scroll),
                                               180);
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(stats_scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(stats_scroll), TRUE);
    app->stats_overlay_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->stats_overlay_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(app->stats_overlay_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(app->stats_overlay_label), 80);
    gtk_widget_set_hexpand(app->stats_overlay_label, TRUE);
    gtk_widget_set_vexpand(app->stats_overlay_label, FALSE);
    gtk_container_add(GTK_CONTAINER(stats_scroll), app->stats_overlay_label);
    gtk_container_add(GTK_CONTAINER(app->stats_overlay_box), stats_scroll);
    gtk_widget_show_all(stats_scroll);
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
populate_video_devices(AppState *app)
{
    if (app->source_combo == NULL)
        return;
    app->updating_controls = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->source_combo));

    gboolean selected_present = FALSE;
    for (guint i = 0; app->video_devices != NULL && i < app->video_devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(app->video_devices, i);
        if (g_strcmp0(device->stable_id, app->selected_video_device_id) == 0)
            selected_present = TRUE;
    }
    if (app->selected_video_device_id != NULL && !selected_present)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->source_combo),
                                  app->selected_video_device_id,
                                  "Saved source unavailable");
    for (guint i = 0; app->video_devices != NULL && i < app->video_devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(app->video_devices, i);
        gboolean selected = g_strcmp0(device->stable_id,
                                      app->selected_video_device_id) == 0;
        if (device->usb_sysfs == NULL && !app->include_advanced_sources && !selected)
            continue;
        gchar *label = device->usb_sysfs != NULL
            ? g_strdup(device->display_name)
            : g_strdup_printf("%s (%s · Internal/virtual)",
                device->display_name != NULL ? device->display_name : "Video source",
                device->nodes->len > 0
                    ? ((CaptureVideoNode *)g_ptr_array_index(device->nodes, 0))->driver
                    : "non-USB");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->source_combo),
                                  device->stable_id, label);
        g_free(label);
    }
    if (app->selected_video_device_id != NULL)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->source_combo),
                                    app->selected_video_device_id);
    else
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->source_combo), -1);

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->node_combo));
    GHashTable *seen_interfaces =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint interface_count = 0;
    for (guint i = 0; app->video_device != NULL &&
         i < app->video_device->nodes->len; i++) {
        CaptureVideoNode *node = g_ptr_array_index(app->video_device->nodes, i);
        if (node->interface_sysfs == NULL ||
            g_hash_table_contains(seen_interfaces, node->interface_sysfs))
            continue;
        g_hash_table_add(seen_interfaces, g_strdup(node->interface_sysfs));
        gchar *label = g_strdup_printf("%s%s%s",
            node->card_name != NULL ? node->card_name : "Video interface",
            node->driver != NULL ? " · " : "",
            node->driver != NULL ? node->driver : "");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->node_combo),
                                  node->interface_sysfs, label);
        g_free(label);
        interface_count++;
    }
    const gchar *node_id = app->video_node != NULL
        ? app->video_node->interface_sysfs : app->selected_node_interface;
    if (node_id != NULL)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->node_combo), node_id);
    else if (interface_count > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->node_combo), 0);
    gtk_widget_set_visible(app->node_selector, interface_count > 1);
    gtk_widget_set_sensitive(app->node_combo, interface_count > 1);
    g_hash_table_unref(seen_interfaces);
    app->updating_controls = FALSE;
}

static void
populate_mode_selectors(AppState *app)
{
    if (app->format_combo == NULL)
        return;
    app->updating_controls = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->format_combo));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->resolution_combo));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->fps_combo));
    gboolean have_mode = app->modes != NULL && app->modes->len > 0 &&
        app->current_mode < app->modes->len;
    if (have_mode) {
        CaptureMode *current = g_ptr_array_index(app->modes, app->current_mode);
        GHashTable *formats = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GHashTable *resolutions =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GHashTable *rates = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        gchar *format_id = g_strdup_printf("%08x", current->fourcc);
        gchar *resolution_id =
            g_strdup_printf("%u:%u", current->width, current->height);
        gchar *current_key = capture_mode_key(current);
        for (guint i = 0; i < app->modes->len; i++) {
            CaptureMode *mode = g_ptr_array_index(app->modes, i);
            gchar *candidate_format = g_strdup_printf("%08x", mode->fourcc);
            if (!g_hash_table_contains(formats, candidate_format)) {
                g_hash_table_add(formats, g_strdup(candidate_format));
                const gchar *format_name = mode->format_name != NULL
                    ? mode->format_name : candidate_format;
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->format_combo),
                                          candidate_format, format_name);
            }
            g_free(candidate_format);
            if (mode->fourcc != current->fourcc)
                continue;
            gchar *candidate_resolution =
                g_strdup_printf("%u:%u", mode->width, mode->height);
            if (!g_hash_table_contains(resolutions, candidate_resolution)) {
                g_hash_table_add(resolutions, g_strdup(candidate_resolution));
                gchar *resolution_label =
                    g_strdup_printf("%u × %u", mode->width, mode->height);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->resolution_combo),
                                          candidate_resolution, resolution_label);
                g_free(resolution_label);
            }
            g_free(candidate_resolution);
            if (mode->width != current->width || mode->height != current->height)
                continue;
            gchar *rate_key = capture_mode_key(mode);
            if (!g_hash_table_contains(rates, rate_key)) {
                g_hash_table_add(rates, g_strdup(rate_key));
                gchar *rate_label = mode->fps_d == 1
                    ? g_strdup_printf("%u fps", mode->fps_n)
                    : g_strdup_printf("%u/%u fps", mode->fps_n, mode->fps_d);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->fps_combo),
                                          rate_key, rate_label);
                g_free(rate_label);
            }
            g_free(rate_key);
        }
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->format_combo), format_id);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->resolution_combo),
                                    resolution_id);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->fps_combo), current_key);
        g_free(format_id);
        g_free(resolution_id);
        g_free(current_key);
        g_hash_table_unref(formats);
        g_hash_table_unref(resolutions);
        g_hash_table_unref(rates);
    }
    gtk_widget_set_sensitive(app->format_combo, have_mode);
    gtk_widget_set_sensitive(app->resolution_combo, have_mode);
    gtk_widget_set_sensitive(app->fps_combo, have_mode);
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
    populate_audio_selector(app);

    populate_video_devices(app);
    populate_mode_selectors(app);
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
    if (app->video_devices != NULL)
        g_ptr_array_unref(app->video_devices);
    g_free(app->audio_device_id);
    g_free(app->audio_selection_id);
    g_free(app->audio_selection_status);
    g_free(app->audio_display_name);
    g_free(app->audio_sink_name);
    g_free(app->audio_sink_id);
    g_free(app->selected_video_device_id);
    g_free(app->selected_video_parent_path);
    g_free(app->selected_node_interface);
    g_free(app->legacy_mode_key);
    g_hash_table_unref(app->mode_preferences);
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
                const gchar *reason = mode_is_usable(node, mode)
                    ? NULL : mode_unusable_reason(node, mode);
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

int
main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    if (argc > 1 && g_str_equal(argv[1], "--list-modes"))
        return list_modes();
    AppState app = {0};
    app.mode_preferences = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_mutex_init(&app.stats_mutex);
    g_mutex_init(&app.log_mutex);
    app.audio_enabled = TRUE;
    app.audio_selection_id = g_strdup("auto");
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
        if (g_key_file_has_key(key_file, "capture", "device-id", NULL)) {
            gchar *stored_id =
                g_key_file_get_string(key_file, "capture", "device-id", NULL);
            if (stored_id != NULL && *stored_id != '\0')
                app.selected_video_device_id = stored_id;
            else
                g_free(stored_id);
        }
        if (g_key_file_has_key(key_file, "capture", "device-parent", NULL))
            app.selected_video_parent_path =
                g_key_file_get_string(key_file, "capture", "device-parent", NULL);
        if (g_key_file_has_key(key_file, "capture", "node-id", NULL))
            app.selected_node_interface =
                g_key_file_get_string(key_file, "capture", "node-id", NULL);
        if (g_key_file_has_key(key_file, "capture", "advanced-sources", NULL))
            app.include_advanced_sources =
                g_key_file_get_boolean(key_file, "capture", "advanced-sources", NULL);
        if (app.selected_video_device_id == NULL &&
            g_key_file_has_key(key_file, "capture", "mode", NULL)) {
            gchar *legacy_mode =
                g_key_file_get_string(key_file, "capture", "mode", NULL);
            if (legacy_mode != NULL && *legacy_mode != '\0')
                app.legacy_mode_key = legacy_mode;
            else
                g_free(legacy_mode);
        }
        if (g_key_file_has_group(key_file, "capture-modes")) {
            GError *mode_error = NULL;
            gsize mode_count = 0;
            gchar **digests =
                g_key_file_get_keys(key_file, "capture-modes", &mode_count,
                                    &mode_error);
            if (digests != NULL) {
                for (gsize i = 0; i < mode_count; i++) {
                    gchar *mode_key = g_key_file_get_string(
                        key_file, "capture-modes", digests[i], NULL);
                    if (mode_key != NULL)
                        g_hash_table_replace(app.mode_preferences,
                                             g_strdup(digests[i]), mode_key);
                }
                g_strfreev(digests);
            }
            g_clear_error(&mode_error);
        }
        if (g_key_file_has_key(key_file, "audio", "enabled", NULL))
            app.audio_enabled = g_key_file_get_boolean(key_file, "audio", "enabled", NULL);
        if (g_key_file_has_key(key_file, "audio", "source-id", NULL)) {
            gchar *stored_audio =
                g_key_file_get_string(key_file, "audio", "source-id", NULL);
            if (stored_audio != NULL && *stored_audio != '\0') {
                g_free(app.audio_selection_id);
                app.audio_selection_id = stored_audio;
            } else {
                g_free(stored_audio);
            }
        }
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
