#pragma once

#include "capture.h"

#include <gst/gst.h>

typedef struct _CaptureAudio CaptureAudio;

typedef struct {
    gboolean added;
    const gchar *display_name;
    const gchar *device_class;
    const gchar *identity;
    const gchar *properties;
} CaptureAudioDeviceEvent;

typedef struct {
    gboolean source_changed;
    gboolean status_changed;
} CaptureAudioUpdate;

/* Pointers and strings in this view are borrowed from CaptureAudio. */
typedef struct {
    GstDevice *source;
    const gchar *source_id;
    const gchar *source_display_name;
    const gchar *source_status;
    const gchar *default_output_name;
    const gchar *default_output_id;
} CaptureAudioState;

/* Event fields are borrowed; event is NULL if a device message could not be parsed. */
typedef void (*CaptureAudioDeviceEventFunc)(
    const CaptureAudioDeviceEvent *event,
    gpointer user_data);
typedef void (*CaptureAudioSourceFunc)(
    const gchar *identity,
    const gchar *display_name,
    gpointer user_data);

CaptureAudio *capture_audio_new(CaptureAudioDeviceEventFunc event_callback,
                                gpointer user_data);
gboolean capture_audio_start(CaptureAudio *audio);
void capture_audio_stop(CaptureAudio *audio);
void capture_audio_free(CaptureAudio *audio);

/* Callback strings are borrowed for the duration of the call. */
void capture_audio_foreach_source(CaptureAudio *audio,
                                 CaptureAudioSourceFunc callback,
                                 gpointer user_data);

CaptureAudioUpdate capture_audio_update_source(CaptureAudio *audio,
                                               const CaptureDevice *video_device,
                                               const gchar *selection);
CaptureAudioState capture_audio_get_state(const CaptureAudio *audio);

gboolean capture_audio_refresh_default_output(CaptureAudio *audio);
void capture_audio_clear_default_output(CaptureAudio *audio);