#pragma once

#include "capture.h"

#include <gst/gst.h>

typedef struct _CaptureRenderer CaptureRenderer;
typedef struct _CapturePipeline CapturePipeline;

typedef struct {
    guint64 frames_dropped;
    guint64 queue_level;
    gdouble current_fps;
    gdouble average_fps;
    gdouble cpu_percent;
    gint64 audio_source_latency_us;
    gint64 audio_source_buffer_us;
    gint64 latency_min_ns;
    gint64 latency_max_ns;
} CapturePipelineStats;

typedef struct {
    void (*log_message)(const gchar *message, gpointer user_data);
    void (*state_changed)(gpointer user_data);
} CapturePipelineCallbacks;

/* renderer and callback user_data are borrowed for the pipeline's lifetime. */
CapturePipeline *capture_pipeline_new(CaptureRenderer *renderer,
                                     const CapturePipelineCallbacks *callbacks,
                                     gpointer user_data);
void capture_pipeline_free(CapturePipeline *pipeline);

/* Capture structs, audio_device, and strings are borrowed only during start. */
gboolean capture_pipeline_start(CapturePipeline *pipeline,
                                const CaptureDevice *device,
                                const CaptureVideoNode *node,
                                const CaptureMode *mode,
                                GstDevice *audio_device,
                                gboolean audio_enabled,
                                gdouble volume,
                                const gchar *audio_identity,
                                const gchar *audio_status);
void capture_pipeline_stop(CapturePipeline *pipeline);
void capture_pipeline_set_volume(CapturePipeline *pipeline, gdouble volume);
gboolean capture_pipeline_is_running(const CapturePipeline *pipeline);
gboolean capture_pipeline_is_waiting_for_frames(CapturePipeline *pipeline);
gboolean capture_pipeline_has_audio_source(const CapturePipeline *pipeline);
const gchar *capture_pipeline_get_error(const CapturePipeline *pipeline);
gchar *capture_pipeline_dup_video_input_caps(CapturePipeline *pipeline);
void capture_pipeline_clear_error(CapturePipeline *pipeline);

void capture_pipeline_update_stats(CapturePipeline *pipeline);
CapturePipelineStats capture_pipeline_get_stats(CapturePipeline *pipeline);

gboolean capture_pipeline_mode_is_usable(const CaptureVideoNode *node,
                                         const CaptureMode *mode);
const gchar *capture_pipeline_mode_unusable_reason(const CaptureVideoNode *node,
                                                   const CaptureMode *mode);
gchar *capture_pipeline_mode_caps_description(const CaptureMode *mode);
