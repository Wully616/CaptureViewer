#pragma once

#include "audio.h"
#include "capture.h"
#include "pipeline.h"
#include "preferences.h"
#include "renderer.h"

#include <gtk/gtk.h>

typedef struct _CaptureUi CaptureUi;

typedef enum {
    CAPTURE_UI_ACTION_SOURCE,
    CAPTURE_UI_ACTION_VIDEO_NODE,
    CAPTURE_UI_ACTION_FORMAT,
    CAPTURE_UI_ACTION_RESOLUTION,
    CAPTURE_UI_ACTION_FRAME_RATE,
    CAPTURE_UI_ACTION_ADVANCED_SOURCES,
    CAPTURE_UI_ACTION_AUDIO_SELECTION,
    CAPTURE_UI_ACTION_AUDIO_ENABLED,
    CAPTURE_UI_ACTION_SCALING,
    CAPTURE_UI_ACTION_VOLUME,
    CAPTURE_UI_ACTION_PINNED,
    CAPTURE_UI_ACTION_STATS_VISIBLE,
    CAPTURE_UI_ACTION_DWELL_DELAY,
    CAPTURE_UI_ACTION_HIDE_DELAY,
    CAPTURE_UI_ACTION_VOLUME_RELEASED,
    CAPTURE_UI_ACTION_SETTINGS_VISIBILITY,
    CAPTURE_UI_ACTION_QUIT,
} CaptureUiActionType;

typedef struct {
    CaptureUiActionType type;
    const gchar *value;
    gdouble number;
    gboolean enabled;
} CaptureUiAction;

typedef void (*CaptureUiActionFunc)(const CaptureUiAction *action,
                                    gpointer user_data);

typedef struct {
    CaptureUiActionFunc action;
} CaptureUiCallbacks;

typedef struct {
    const CapturePreferencesValues *preferences;
    CaptureDevice *video_device;
    CaptureVideoNode *video_node;
    GPtrArray *modes;
    guint current_mode;
    CaptureAudioState audio;
    CapturePipeline *pipeline;
    CapturePipelineStats pipeline_stats;
    const gchar *pipeline_error;
    const gchar *log_path;
    const gchar *renderer_backend;
    gboolean pipeline_running;
    gboolean pipeline_has_audio_source;
    gboolean waiting_for_frames;
    gint64 retry_after_us;
} CaptureUiModel;

CaptureUi *capture_ui_new(GtkApplication *application,
                          CaptureRenderer *renderer,
                          const CapturePreferencesValues *preferences,
                          const CaptureUiCallbacks *callbacks,
                          gpointer user_data);
void capture_ui_free(CaptureUi *ui);
GtkWidget *capture_ui_get_window(CaptureUi *ui);
void capture_ui_present(CaptureUi *ui);
void capture_ui_toggle_fullscreen(CaptureUi *ui);
void capture_ui_set_fullscreen(CaptureUi *ui, gboolean fullscreen);

void capture_ui_refresh_audio_sources(CaptureUi *ui,
                                      CaptureAudio *audio,
                                      const gchar *selected_id);
void capture_ui_refresh_video_sources(CaptureUi *ui,
                                      GPtrArray *devices,
                                      CaptureDevice *selected_device,
                                      CaptureVideoNode *selected_node,
                                      const CapturePreferencesValues *preferences);
void capture_ui_refresh_mode_selectors(CaptureUi *ui,
                                       GPtrArray *modes,
                                       guint current_mode);
void capture_ui_update(CaptureUi *ui, const CaptureUiModel *model);
