#pragma once

#include <glib.h>

typedef enum {
    CAPTURE_PREFERENCES_SCALE_FIT,
    CAPTURE_PREFERENCES_SCALE_FILL,
} CapturePreferencesScaleMode;

typedef struct {
    gchar *selected_video_device_id;
    gchar *selected_video_parent_path;
    gchar *selected_node_interface;
    gchar *legacy_mode_key;
    gchar *audio_selection_id;
    gboolean audio_enabled;
    gboolean audio_selection_session_only;
    gboolean include_advanced_sources;
    gboolean uvc_setup_dismissed;
    gdouble volume;
    gboolean pinned;
    gboolean stats_visible;
    guint panel_dwell_ms;
    guint panel_hide_delay_ms;
    CapturePreferencesScaleMode scale_mode;
} CapturePreferencesValues;

typedef struct _CapturePreferences CapturePreferences;

CapturePreferences *capture_preferences_new(void);
void capture_preferences_free(CapturePreferences *preferences);

/* Borrowed mutable view; its string fields remain owned by preferences. */
CapturePreferencesValues *capture_preferences_get_values(
    CapturePreferences *preferences);

void capture_preferences_save(CapturePreferences *preferences);
void capture_preferences_schedule_save(CapturePreferences *preferences);

/* Returned mode keys remain owned by preferences. */
const gchar *capture_preferences_mode_for_device(
    CapturePreferences *preferences,
    const gchar *stable_id,
    guint16 usb_vid,
    guint16 usb_pid);
/* Takes ownership of mode_key. */
void capture_preferences_set_mode_for_device(
    CapturePreferences *preferences,
    const gchar *stable_id,
    guint16 usb_vid,
    guint16 usb_pid,
    gchar *mode_key);
void capture_preferences_migrate_device_id(CapturePreferences *preferences,
                                           const gchar *old_id,
                                           const gchar *new_id);
