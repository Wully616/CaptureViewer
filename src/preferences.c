#include "preferences.h"
#include <glib/gstdio.h>

struct _CapturePreferences {
    CapturePreferencesValues values;
    GHashTable *mode_preferences;
    gchar *config_path;
    guint save_watch_id;
};

static gchar *
mode_preference_digest(const gchar *stable_id)
{
    return stable_id != NULL
        ? g_compute_checksum_for_string(G_CHECKSUM_SHA256, stable_id, -1) : NULL;
}

static gboolean
save_preferences_timeout(gpointer user_data)
{
    CapturePreferences *preferences = user_data;
    preferences->save_watch_id = 0;
    capture_preferences_save(preferences);
    return G_SOURCE_REMOVE;
}

CapturePreferences *
capture_preferences_new(void)
{
    CapturePreferences *preferences = g_new0(CapturePreferences, 1);
    preferences->values.audio_enabled = TRUE;
    preferences->values.audio_selection_id = g_strdup("auto");
    preferences->values.volume = 0.8;
    preferences->values.panel_dwell_ms = 150;
    preferences->values.panel_hide_delay_ms = 700;
    preferences->values.scale_mode = CAPTURE_PREFERENCES_SCALE_FIT;
    preferences->mode_preferences =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    preferences->config_path =
        g_build_filename(g_get_user_config_dir(), "captureviewer", "config.ini", NULL);

    gchar *legacy_config_path =
        g_build_filename(g_get_user_config_dir(), "hagibis-viewer", "config.ini", NULL);
    gboolean new_config_exists =
        g_file_test(preferences->config_path, G_FILE_TEST_EXISTS);
    GKeyFile *key_file = g_key_file_new();
    gboolean config_loaded = g_key_file_load_from_file(
        key_file,
        new_config_exists ? preferences->config_path : legacy_config_path,
        G_KEY_FILE_NONE, NULL);
    if (config_loaded && !new_config_exists) {
        gsize length = 0;
        gchar *contents = g_key_file_to_data(key_file, &length, NULL);
        gchar *directory = g_path_get_dirname(preferences->config_path);
        if (g_mkdir_with_parents(directory, 0700) != 0 ||
            !g_file_set_contents(preferences->config_path, contents,
                                 (gssize)length, NULL))
            g_warning("Could not migrate existing preferences to %s",
                      preferences->config_path);
        g_free(directory);
        g_free(contents);
    }
    if (config_loaded) {
        if (g_key_file_has_key(key_file, "capture", "device-id", NULL)) {
            gchar *stored_id =
                g_key_file_get_string(key_file, "capture", "device-id", NULL);
            if (stored_id != NULL && *stored_id != '\0')
                preferences->values.selected_video_device_id = stored_id;
            else
                g_free(stored_id);
        }
        if (g_key_file_has_key(key_file, "capture", "device-parent", NULL))
            preferences->values.selected_video_parent_path =
                g_key_file_get_string(key_file, "capture", "device-parent", NULL);
        if (g_key_file_has_key(key_file, "capture", "node-id", NULL))
            preferences->values.selected_node_interface =
                g_key_file_get_string(key_file, "capture", "node-id", NULL);
        if (g_key_file_has_key(key_file, "capture", "advanced-sources", NULL))
            preferences->values.include_advanced_sources =
                g_key_file_get_boolean(key_file, "capture", "advanced-sources", NULL);
        if (preferences->values.selected_video_device_id == NULL &&
            g_key_file_has_key(key_file, "capture", "mode", NULL)) {
            gchar *legacy_mode =
                g_key_file_get_string(key_file, "capture", "mode", NULL);
            if (legacy_mode != NULL && *legacy_mode != '\0')
                preferences->values.legacy_mode_key = legacy_mode;
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
                        g_hash_table_replace(preferences->mode_preferences,
                                             g_strdup(digests[i]), mode_key);
                }
                g_strfreev(digests);
            }
            g_clear_error(&mode_error);
        }
        if (g_key_file_has_key(key_file, "audio", "enabled", NULL))
            preferences->values.audio_enabled =
                g_key_file_get_boolean(key_file, "audio", "enabled", NULL);
        if (g_key_file_has_key(key_file, "audio", "source-id", NULL)) {
            gchar *stored_audio =
                g_key_file_get_string(key_file, "audio", "source-id", NULL);
            if (stored_audio != NULL && *stored_audio != '\0') {
                g_free(preferences->values.audio_selection_id);
                preferences->values.audio_selection_id = stored_audio;
            } else {
                g_free(stored_audio);
            }
        }
        if (g_key_file_has_key(key_file, "audio", "volume", NULL)) {
            gdouble stored_volume =
                g_key_file_get_double(key_file, "audio", "volume", NULL);
            if (stored_volume >= 0.0 && stored_volume <= 1.0)
                preferences->values.volume = stored_volume;
        }
        if (g_key_file_has_key(key_file, "ui", "panel-pinned", NULL))
            preferences->values.pinned =
                g_key_file_get_boolean(key_file, "ui", "panel-pinned", NULL);
        if (g_key_file_has_key(key_file, "ui", "stats-visible", NULL))
            preferences->values.stats_visible =
                g_key_file_get_boolean(key_file, "ui", "stats-visible", NULL);
        if (g_key_file_has_key(key_file, "ui", "dwell-ms", NULL)) {
            gint dwell = g_key_file_get_integer(key_file, "ui", "dwell-ms", NULL);
            if (dwell >= 100 && dwell <= 250)
                preferences->values.panel_dwell_ms = (guint)dwell;
        }
        if (g_key_file_has_key(key_file, "ui", "hide-delay-ms", NULL)) {
            gint delay =
                g_key_file_get_integer(key_file, "ui", "hide-delay-ms", NULL);
            if (delay >= 500 && delay <= 1000)
                preferences->values.panel_hide_delay_ms = (guint)delay;
        }
        gchar *stored_scale_mode =
            g_key_file_get_string(key_file, "ui", "scaling-mode", NULL);
        preferences->values.scale_mode =
            g_strcmp0(stored_scale_mode, "fill") == 0
                ? CAPTURE_PREFERENCES_SCALE_FILL : CAPTURE_PREFERENCES_SCALE_FIT;
        g_free(stored_scale_mode);
    }
    g_free(legacy_config_path);
    g_key_file_unref(key_file);
    return preferences;
}

void
capture_preferences_free(CapturePreferences *preferences)
{
    if (preferences == NULL)
        return;
    if (preferences->save_watch_id != 0)
        g_source_remove(preferences->save_watch_id);
    g_free(preferences->values.selected_video_device_id);
    g_free(preferences->values.selected_video_parent_path);
    g_free(preferences->values.selected_node_interface);
    g_free(preferences->values.legacy_mode_key);
    g_free(preferences->values.audio_selection_id);
    g_hash_table_unref(preferences->mode_preferences);
    g_free(preferences->config_path);
    g_free(preferences);
}

CapturePreferencesValues *
capture_preferences_get_values(CapturePreferences *preferences)
{
    return &preferences->values;
}

void
capture_preferences_save(CapturePreferences *preferences)
{
    if (preferences->save_watch_id != 0) {
        g_source_remove(preferences->save_watch_id);
        preferences->save_watch_id = 0;
    }

    const CapturePreferencesValues *values = &preferences->values;
    GKeyFile *key_file = g_key_file_new();
    if (values->selected_video_device_id != NULL)
        g_key_file_set_string(key_file, "capture", "device-id",
                              values->selected_video_device_id);
    if (values->selected_video_parent_path != NULL)
        g_key_file_set_string(key_file, "capture", "device-parent",
                              values->selected_video_parent_path);
    if (values->selected_node_interface != NULL)
        g_key_file_set_string(key_file, "capture", "node-id",
                              values->selected_node_interface);
    if (values->legacy_mode_key != NULL && values->selected_video_device_id == NULL)
        g_key_file_set_string(key_file, "capture", "mode", values->legacy_mode_key);
    g_key_file_set_boolean(key_file, "capture", "advanced-sources",
                           values->include_advanced_sources);
    g_key_file_set_boolean(key_file, "audio", "enabled", values->audio_enabled);
    if (!values->audio_selection_session_only && values->audio_selection_id != NULL)
        g_key_file_set_string(key_file, "audio", "source-id",
                              values->audio_selection_id);
    g_key_file_set_double(key_file, "audio", "volume", values->volume);

    GHashTableIter iter;
    gpointer digest, mode_key;
    g_hash_table_iter_init(&iter, preferences->mode_preferences);
    while (g_hash_table_iter_next(&iter, &digest, &mode_key))
        g_key_file_set_string(key_file, "capture-modes", digest, mode_key);

    g_key_file_set_boolean(key_file, "ui", "panel-pinned", values->pinned);
    g_key_file_set_boolean(key_file, "ui", "stats-visible", values->stats_visible);
    g_key_file_set_integer(key_file, "ui", "dwell-ms", values->panel_dwell_ms);
    g_key_file_set_integer(key_file, "ui", "hide-delay-ms", values->panel_hide_delay_ms);
    g_key_file_set_string(key_file, "ui", "scaling-mode",
        values->scale_mode == CAPTURE_PREFERENCES_SCALE_FILL ? "fill" : "fit");

    gsize length = 0;
    gchar *contents = g_key_file_to_data(key_file, &length, NULL);
    gchar *directory = g_path_get_dirname(preferences->config_path);
    if (g_mkdir_with_parents(directory, 0700) == 0)
        g_file_set_contents(preferences->config_path, contents, (gssize)length, NULL);
    g_free(directory);
    g_free(contents);
    g_key_file_unref(key_file);
}

void
capture_preferences_schedule_save(CapturePreferences *preferences)
{
    if (preferences->save_watch_id != 0)
        g_source_remove(preferences->save_watch_id);
    preferences->save_watch_id =
        g_timeout_add(250, save_preferences_timeout, preferences);
}

const gchar *
capture_preferences_mode_for_device(CapturePreferences *preferences,
                                      const gchar *stable_id,
                                      guint16 usb_vid,
                                      guint16 usb_pid)
{
    gchar *digest = mode_preference_digest(stable_id);
    const gchar *mode_key = g_hash_table_lookup(preferences->mode_preferences, digest);
    g_free(digest);
    if (mode_key == NULL && preferences->values.legacy_mode_key != NULL &&
        usb_vid == 0x345f && usb_pid == 0x2130)
        mode_key = preferences->values.legacy_mode_key;
    return mode_key;
}

void
capture_preferences_set_mode_for_device(CapturePreferences *preferences,
                                        const gchar *stable_id,
                                        guint16 usb_vid,
                                        guint16 usb_pid,
                                        gchar *mode_key)
{
    gchar *digest = mode_preference_digest(stable_id);
    g_hash_table_replace(preferences->mode_preferences, digest, mode_key);
    if (usb_vid == 0x345f && usb_pid == 0x2130)
        g_clear_pointer(&preferences->values.legacy_mode_key, g_free);
    capture_preferences_schedule_save(preferences);
}

void
capture_preferences_migrate_device_id(CapturePreferences *preferences,
                                      const gchar *old_id,
                                      const gchar *new_id)
{
    if (old_id == NULL || new_id == NULL || g_str_equal(old_id, new_id))
        return;
    gchar *old_digest = mode_preference_digest(old_id);
    gchar *new_digest = mode_preference_digest(new_id);
    const gchar *old_mode =
        g_hash_table_lookup(preferences->mode_preferences, old_digest);
    if (old_mode != NULL &&
        !g_hash_table_contains(preferences->mode_preferences, new_digest))
        g_hash_table_insert(preferences->mode_preferences, g_strdup(new_digest),
                            g_strdup(old_mode));
    g_hash_table_remove(preferences->mode_preferences, old_digest);
    g_free(old_digest);
    g_free(new_digest);
}
