#include "audio.h"

#include <glib/gstdio.h>
#include <string.h>

struct _CaptureAudio {
    GstDeviceMonitor *monitor;
    guint monitor_watch_id;
    gboolean monitor_started;
    CaptureAudioDeviceEventFunc event_callback;
    gpointer user_data;
    GstDevice *source;
    gchar *source_id;
    gchar *source_display_name;
    gchar *source_status;
    gchar *default_output_name;
    gchar *default_output_id;
};

static gboolean find_default_audio_output(CaptureAudio *audio,
                                          gchar **display_name,
                                          gchar **device_id);

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
audio_device_identity(GstDevice *device)
{
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
    } else if (serial != NULL && have_usb_ids) {
        identity = g_strdup_printf("usb:%04" G_GINT64_MODIFIER "x:%04"
            G_GINT64_MODIFIER "x:serial:%s", vendor, product, serial);
    } else if (normalized_sysfs != NULL) {
        identity = g_strdup_printf("sysfs:%s", normalized_sysfs);
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

static gboolean
capture_audio_device_monitor_message(GstBus *bus,
                                     GstMessage *message,
                                     gpointer user_data)
{
    CaptureAudio *audio = user_data;
    GstMessageType type = GST_MESSAGE_TYPE(message);
    if (type == GST_MESSAGE_DEVICE_ADDED || type == GST_MESSAGE_DEVICE_REMOVED) {
        GstDevice *device = NULL;
        if (type == GST_MESSAGE_DEVICE_ADDED)
            gst_message_parse_device_added(message, &device);
        else
            gst_message_parse_device_removed(message, &device);
        CaptureAudioDeviceEvent event = {.added = type == GST_MESSAGE_DEVICE_ADDED};
        gchar *display_name = NULL;
        gchar *device_class = NULL;
        gchar *identity = NULL;
        gchar *property_text = NULL;
        if (device != NULL) {
            display_name = gst_device_get_display_name(device);
            device_class = gst_device_get_device_class(device);
            identity = audio_device_identity(device);
            GstStructure *properties = gst_device_get_properties(device);
            property_text = properties != NULL
                ? gst_structure_to_string(properties) : g_strdup("unavailable");
            if (properties != NULL)
                gst_structure_free(properties);
            event.display_name = display_name;
            event.device_class = device_class;
            event.identity = identity;
            event.properties = property_text;
        }
        if (audio->event_callback != NULL)
            audio->event_callback(device != NULL ? &event : NULL, audio->user_data);
        g_free(display_name);
        g_free(device_class);
        g_free(identity);
        g_free(property_text);
        if (device != NULL)
            gst_object_unref(device);
    }
    (void)bus;
    return G_SOURCE_CONTINUE;
}

CaptureAudio *
capture_audio_new(CaptureAudioDeviceEventFunc event_callback, gpointer user_data)
{
    CaptureAudio *audio = g_new0(CaptureAudio, 1);
    audio->event_callback = event_callback;
    audio->user_data = user_data;
    return audio;
}

gboolean
capture_audio_start(CaptureAudio *audio)
{
    if (audio->monitor != NULL)
        return audio->monitor_started;
    audio->monitor = gst_device_monitor_new();
    GstCaps *audio_caps = gst_caps_new_empty_simple("audio/x-raw");
    gst_device_monitor_add_filter(audio->monitor, "Audio/Source", audio_caps);
    gst_device_monitor_add_filter(audio->monitor, "Audio/Sink", audio_caps);
    gst_caps_unref(audio_caps);
    audio->monitor_started = gst_device_monitor_start(audio->monitor);
    if (!audio->monitor_started)
        return FALSE;
    GstBus *bus = gst_device_monitor_get_bus(audio->monitor);
    audio->monitor_watch_id = gst_bus_add_watch(
        bus, capture_audio_device_monitor_message, audio);
    g_source_set_name_by_id(audio->monitor_watch_id, "captureviewer-device-monitor");
    gst_object_unref(bus);
    return TRUE;
}

void
capture_audio_stop(CaptureAudio *audio)
{
    if (audio == NULL)
        return;
    if (audio->monitor_watch_id != 0) {
        g_source_remove(audio->monitor_watch_id);
        audio->monitor_watch_id = 0;
    }
    if (audio->monitor_started) {
        gst_device_monitor_stop(audio->monitor);
        audio->monitor_started = FALSE;
    }
}

void
capture_audio_free(CaptureAudio *audio)
{
    if (audio == NULL)
        return;
    capture_audio_stop(audio);
    if (audio->monitor != NULL)
        gst_object_unref(audio->monitor);
    if (audio->source != NULL)
        gst_object_unref(audio->source);
    g_free(audio->source_id);
    g_free(audio->source_display_name);
    g_free(audio->source_status);
    g_free(audio->default_output_name);
    g_free(audio->default_output_id);
    g_free(audio);
}

void
capture_audio_foreach_source(CaptureAudio *audio,
                             CaptureAudioSourceFunc callback,
                             gpointer user_data)
{
    if (audio == NULL || audio->monitor == NULL || callback == NULL)
        return;
    GList *devices = gst_device_monitor_get_devices(audio->monitor);
    for (GList *item = devices; item != NULL; item = item->next) {
        GstDevice *device = GST_DEVICE(item->data);
        if (!is_audio_source_device(device))
            continue;
        gchar *identity = audio_device_identity(device);
        gchar *display_name = gst_device_get_display_name(device);
        callback(identity, display_name, user_data);
        g_free(identity);
        g_free(display_name);
    }
    g_list_free_full(devices, g_object_unref);
}

static GstDevice *
resolve_audio_source(CaptureAudio *audio,
                     const CaptureDevice *video_device,
                     const gchar *selection,
                     gchar **identity,
                     gchar **status)
{
    *identity = NULL;
    *status = NULL;
    if (selection == NULL)
        selection = "auto";
    if (g_str_equal(selection, "none")) {
        *status = g_strdup("No audio input selected");
        return NULL;
    }
    if (g_str_equal(selection, "auto") &&
        (video_device == NULL || video_device->usb_sysfs == NULL)) {
        *status = g_strdup("Auto matching waits for a selected USB capture device");
        return NULL;
    }
    if (audio == NULL || audio->monitor == NULL) {
        *status = g_strdup("Audio device monitor is unavailable");
        return NULL;
    }

    GList *devices = gst_device_monitor_get_devices(audio->monitor);
    GstDevice *found = NULL;
    guint matches = 0;
    for (GList *item = devices; item != NULL; item = item->next) {
        GstDevice *candidate = GST_DEVICE(item->data);
        if (!is_audio_source_device(candidate))
            continue;
        gboolean match;
        gchar *candidate_identity = audio_device_identity(candidate);
        if (g_str_equal(selection, "auto"))
            match = audio_device_matches_video(candidate, video_device);
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
        *identity = audio_device_identity(found);
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

CaptureAudioUpdate
capture_audio_update_source(CaptureAudio *audio,
                            const CaptureDevice *video_device,
                            const gchar *selection)
{
    CaptureAudioUpdate update = {0};
    if (audio == NULL)
        return update;

    gchar *identity = NULL;
    gchar *status = NULL;
    GstDevice *source = resolve_audio_source(
        audio, video_device, selection, &identity, &status);
    update.source_changed = g_strcmp0(identity, audio->source_id) != 0;
    update.status_changed = g_strcmp0(status, audio->source_status) != 0;
    if (update.status_changed) {
        g_free(audio->source_status);
        audio->source_status = status;
        status = NULL;
    }
    if (update.source_changed) {
        if (audio->source != NULL)
            gst_object_unref(audio->source);
        audio->source = source;
        source = NULL;
        g_free(audio->source_id);
        audio->source_id = identity;
        identity = NULL;
        g_free(audio->source_display_name);
        audio->source_display_name = audio->source != NULL
            ? gst_device_get_display_name(audio->source) : NULL;
    }
    if (source != NULL)
        gst_object_unref(source);
    g_free(identity);
    g_free(status);
    return update;
}

CaptureAudioState
capture_audio_get_state(const CaptureAudio *audio)
{
    CaptureAudioState state = {0};
    if (audio != NULL) {
        state.source = audio->source;
        state.source_id = audio->source_id;
        state.source_display_name = audio->source_display_name;
        state.source_status = audio->source_status;
        state.default_output_name = audio->default_output_name;
        state.default_output_id = audio->default_output_id;
    }
    return state;
}

gboolean
capture_audio_refresh_default_output(CaptureAudio *audio)
{
    if (audio == NULL)
        return FALSE;
    gchar *display_name = NULL;
    gchar *device_id = NULL;
    find_default_audio_output(audio, &display_name, &device_id);
    if (g_strcmp0(audio->default_output_name, display_name) == 0 &&
        g_strcmp0(audio->default_output_id, device_id) == 0) {
        g_free(display_name);
        g_free(device_id);
        return FALSE;
    }
    g_free(audio->default_output_name);
    g_free(audio->default_output_id);
    audio->default_output_name = display_name;
    audio->default_output_id = device_id;
    return TRUE;
}

void
capture_audio_clear_default_output(CaptureAudio *audio)
{
    if (audio == NULL)
        return;
    g_clear_pointer(&audio->default_output_name, g_free);
    g_clear_pointer(&audio->default_output_id, g_free);
}

static gboolean
find_default_audio_output(CaptureAudio *audio,
                          gchar **display_name,
                          gchar **device_id)
{
    *display_name = NULL;
    *device_id = NULL;
    if (audio == NULL || audio->monitor == NULL)
        return FALSE;

    GList *devices = gst_device_monitor_get_devices(audio->monitor);
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

