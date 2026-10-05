#include "pipeline.h"
#include "renderer.h"

#include <linux/videodev2.h>
#include <stdarg.h>
#include <sys/resource.h>

#define VIDEO_QUEUE_BUFFERS 1
#define AUDIO_QUEUE_BUFFERS 4
#define AUDIO_QUEUE_NS 50000000
#define AUDIO_LATENCY_US 20000
#define AUDIO_BUFFER_US 60000

struct _CapturePipeline {
    CaptureRenderer *renderer;
    CapturePipelineCallbacks callbacks;
    gpointer user_data;

    GstElement *pipeline;
    GstBus *bus;
    GstElement *video_queue;
    GstElement *audio_queue;
    GstElement *video_convert;
    GstElement *fps_sink;
    GstElement *audio_source;
    GstElement *audio_sink;
    guint bus_watch_id;

    gchar *diagnostic_context;
    gchar *diagnostic_expected_caps;
    gchar *audio_identity;
    gchar *audio_status;
    gchar *error;
    gint64 pipeline_started_us;

    GMutex stats_mutex;
    CapturePipelineStats stats;
    gint64 last_video_frame_us;
    gint64 last_cpu_us;
    gint64 last_cpu_time_us;
};
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
pipeline_log(CapturePipeline *pipeline, const gchar *format, ...)
{
    if (pipeline->callbacks.log_message == NULL)
        return;
    va_list args;
    va_start(args, format);
    gchar *message = g_strdup_vprintf(format, args);
    va_end(args);
    pipeline->callbacks.log_message(message, pipeline->user_data);
    g_free(message);
}

static GstCaps *
caps_for_mode(const CaptureMode *mode)
{
    const gchar *caps_name;
    if (mode->compressed) {
        if (mode->fourcc == V4L2_PIX_FMT_MJPEG || mode->fourcc == V4L2_PIX_FMT_JPEG)
            caps_name = "image/jpeg";
        else if (mode->fourcc == V4L2_PIX_FMT_H264)
            caps_name = "video/x-h264";
#ifdef V4L2_PIX_FMT_HEVC
        else if (mode->fourcc == V4L2_PIX_FMT_HEVC)
            caps_name = "video/x-h265";
#endif
#ifdef V4L2_PIX_FMT_VP9
        else if (mode->fourcc == V4L2_PIX_FMT_VP9)
            caps_name = "video/x-vp9";
#endif
        else
            return NULL;
    } else {
        caps_name = "video/x-raw";
    }

    GstCaps *caps = gst_caps_new_simple(caps_name,
        "width", G_TYPE_INT, (gint)mode->width,
        "height", G_TYPE_INT, (gint)mode->height,
        "framerate", GST_TYPE_FRACTION, (gint)mode->fps_n, (gint)mode->fps_d,
        NULL);
    if (mode->fourcc == V4L2_PIX_FMT_MJPEG || mode->fourcc == V4L2_PIX_FMT_JPEG) {
        gst_caps_set_simple(caps, "parsed", G_TYPE_BOOLEAN, TRUE, NULL);
    } else if (!mode->compressed) {
        const gchar *format = NULL;
        switch (mode->fourcc) {
        case V4L2_PIX_FMT_YUYV: format = "YUY2"; break;
        case V4L2_PIX_FMT_UYVY: format = "UYVY"; break;
        case V4L2_PIX_FMT_YVYU: format = "YVYU"; break;
        case V4L2_PIX_FMT_NV12: format = "NV12"; break;
        case V4L2_PIX_FMT_NV21: format = "NV21"; break;
        case V4L2_PIX_FMT_RGB24: format = "RGB"; break;
        case V4L2_PIX_FMT_BGR24: format = "BGR"; break;
        case V4L2_PIX_FMT_GREY: format = "GRAY8"; break;
        default: break;
        }
        if (format == NULL) {
            gst_caps_unref(caps);
            return NULL;
        }
        gst_caps_set_simple(caps, "format", G_TYPE_STRING, format, NULL);
    }
    return caps;
}

gchar *
capture_pipeline_mode_caps_description(const CaptureMode *mode)
{
    GstCaps *caps = mode != NULL ? caps_for_mode(mode) : NULL;
    if (caps == NULL)
        return g_strdup("unavailable");
    gchar *description = gst_caps_to_string(caps);
    gst_caps_unref(caps);
    return description;
}

static gboolean
is_mjpeg_mode(const CaptureMode *mode)
{
    return mode->fourcc == V4L2_PIX_FMT_MJPEG || mode->fourcc == V4L2_PIX_FMT_JPEG;
}

const gchar *
capture_pipeline_mode_unusable_reason(const CaptureVideoNode *node,
                                       const CaptureMode *mode)
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

gboolean
capture_pipeline_mode_is_usable(const CaptureVideoNode *node,
                                const CaptureMode *mode)
{
    return capture_pipeline_mode_unusable_reason(node, mode) == NULL;
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

gchar *
capture_pipeline_dup_video_input_caps(CapturePipeline *pipeline)
{
    return element_current_caps(pipeline->video_convert, "sink");
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
capture_diagnostic_context_base(const CaptureDevice *device,
                               const CaptureVideoNode *node,
                               const CaptureMode *mode)
{
    gchar *mode_key = capture_mode_key(mode);
    gchar *context = g_strdup_printf(
        "device-id=%s physical=%s usb-parent=%s serial=%s node=%s card=%s "
        "driver=%s bus-info=%s capabilities=0x%08x buffer-type=%s "
        "mode=%s [%s] decoder=%s",
        device->stable_id != NULL ? device->stable_id : "unavailable",
        device->physical_sysfs != NULL ? device->physical_sysfs : "unavailable",
        device->usb_sysfs != NULL ? device->usb_sysfs : "non-USB",
        device->serial != NULL ? device->serial : "unavailable",
        node->path != NULL ? node->path : "unavailable",
        node->card_name != NULL ? node->card_name : "unavailable",
        node->driver != NULL ? node->driver : "unavailable",
        node->bus_info != NULL ? node->bus_info : "unavailable",
        node->capabilities, video_buffer_type_name(node->buffer_type),
        mode->label, mode_key, selected_decoder_description(mode));
    g_free(mode_key);
    return context;
}

static gchar *
capture_diagnostic_context(CapturePipeline *pipeline)
{
    gchar *negotiated = capture_pipeline_dup_video_input_caps(pipeline);
    const gchar *renderer = pipeline->renderer != NULL
        ? capture_renderer_get_backend_name(pipeline->renderer) : "unavailable";
    gchar *context = g_strdup_printf(
        "%s renderer=%s expected-caps=%s negotiated-videoconvert-sink=%s "
        "audio-source-id=%s audio-status=%s",
        pipeline->diagnostic_context != NULL
            ? pipeline->diagnostic_context : "capture metadata unavailable",
        renderer,
        pipeline->diagnostic_expected_caps != NULL
            ? pipeline->diagnostic_expected_caps : "unavailable",
        negotiated,
        pipeline->audio_identity != NULL ? pipeline->audio_identity : "unavailable",
        pipeline->audio_status != NULL ? pipeline->audio_status : "unavailable");
    g_free(negotiated);
    return context;
}


static void
on_decode_pad_added(GstElement *decoder, GstPad *pad, gpointer user_data)
{
    CapturePipeline *pipeline = user_data;
    GstPad *sinkpad = gst_element_get_static_pad(pipeline->video_convert, "sink");
    if (sinkpad == NULL) {
        pipeline_log(pipeline, "decodebin produced a pad but video converter has no sink pad");
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
                pipeline_log(pipeline, "Could not link decoded video pad: %s",
                             gst_pad_link_get_name(linked));
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
        CapturePipeline *pipeline = user_data;
        g_mutex_lock(&pipeline->stats_mutex);
        pipeline->last_video_frame_us = g_get_monotonic_time();
        g_mutex_unlock(&pipeline->stats_mutex);
    }
    (void)pad;
    return GST_PAD_PROBE_OK;
}


static GstPadProbeReturn
on_capture_source_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        CapturePipeline *pipeline = user_data;
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        capture_renderer_record_source_buffer(pipeline->renderer, buffer);
    }
    (void)pad;
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
on_decoded_video_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        CapturePipeline *pipeline = user_data;
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        capture_renderer_record_decoded_buffer(pipeline->renderer, buffer);
    }
    (void)pad;
    return GST_PAD_PROBE_OK;
}
static gboolean
setup_video_branch(CapturePipeline *pipeline, const CaptureVideoNode *node,
                   const CaptureMode *mode, GError **error)
{
    GstElement *source = gst_element_factory_make("v4l2src", "capture-source");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "capture-mode");
    GstElement *queue = gst_element_factory_make("queue", "latest-frame-queue");
    GstElement *convert = gst_element_factory_make("videoconvert", "video-convert");
    GstElement *fps = gst_element_factory_make("fpsdisplaysink", "display-metrics");
    GstElement *sink = capture_renderer_create_sink(pipeline->renderer);
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
    g_object_set(source, "device", node->path, "io-mode", 2, NULL);
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
        return FALSE;
    }

    gst_bin_add_many(GST_BIN(pipeline->pipeline), source, capsfilter, queue, convert, NULL);
    if (decoder != NULL)
        gst_bin_add(GST_BIN(pipeline->pipeline), decoder);
    gst_bin_add(GST_BIN(pipeline->pipeline), fps);

    pipeline->video_queue = queue;
    pipeline->video_convert = convert;
    pipeline->fps_sink = fps;

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
            g_signal_connect(decoder, "pad-added", G_CALLBACK(on_decode_pad_added), pipeline);
    }
    if (!linked) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
                    "Could not link video elements for %s", mode->label);
        return FALSE;
    }
    GstPad *source_pad = gst_element_get_static_pad(source, "src");
    if (source_pad != NULL) {
        gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_BUFFER,
                          on_capture_source_buffer, pipeline, NULL);
        gst_object_unref(source_pad);
    }
    GstPad *decoded_pad = gst_element_get_static_pad(convert, "sink");
    if (decoded_pad != NULL) {
        gst_pad_add_probe(decoded_pad, GST_PAD_PROBE_TYPE_BUFFER,
                          on_decoded_video_buffer, pipeline, NULL);
        gst_object_unref(decoded_pad);
    }
    GstPad *frame_pad = gst_element_get_static_pad(convert, "src");
    if (frame_pad != NULL) {
        gst_pad_add_probe(frame_pad, GST_PAD_PROBE_TYPE_BUFFER,
                          on_video_frame_buffer, pipeline, NULL);
        gst_object_unref(frame_pad);
    }

    g_signal_connect(fps, "fps-measurements", G_CALLBACK(on_fps_measurements), pipeline);
    return TRUE;
}

static gboolean
setup_audio_branch(CapturePipeline *pipeline, GstDevice *audio_device,
                   gboolean audio_enabled, gdouble volume, GError **error)
{
    if (!audio_enabled || audio_device == NULL)
        return TRUE;

    GstElement *source = gst_device_create_element(audio_device, "capture-audio-source");
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
        "sync", FALSE,
        "volume", volume,
        "client-name", "CaptureViewer",
        NULL);
    gst_bin_add_many(GST_BIN(pipeline->pipeline), source, queue, convert, resample, sink, NULL);
    if (!gst_element_link_many(source, queue, convert, resample, sink, NULL)) {
        g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
                    "Could not link the selected audio input to the default speaker sink");
        return FALSE;
    }
    pipeline->audio_source = source;
    pipeline->audio_sink = sink;
    pipeline->audio_queue = queue;
    return TRUE;
}

static void
pipeline_stop(CapturePipeline *pipeline)
{
    if (pipeline->bus_watch_id != 0) {
        g_source_remove(pipeline->bus_watch_id);
        pipeline->bus_watch_id = 0;
    }
    if (pipeline->pipeline != NULL) {
        GstState state = GST_STATE_VOID_PENDING;
        gst_element_set_state(pipeline->pipeline, GST_STATE_NULL);
        GstStateChangeReturn state_result =
            gst_element_get_state(pipeline->pipeline, &state, NULL, GST_CLOCK_TIME_NONE);
        if (pipeline->renderer != NULL && state_result != GST_STATE_CHANGE_FAILURE &&
            state == GST_STATE_NULL) {
            capture_renderer_pipeline_stopped(pipeline->renderer);
        } else if (pipeline->renderer != NULL) {
            pipeline_log(pipeline, "Renderer teardown deferred: GStreamer pipeline did not reach NULL");
        }
        gst_object_unref(pipeline->pipeline);
    } else if (pipeline->renderer != NULL) {
        capture_renderer_pipeline_stopped(pipeline->renderer);
    }
    if (pipeline->bus != NULL)
        gst_object_unref(pipeline->bus);
    pipeline->pipeline = NULL;
    pipeline->bus = NULL;
    pipeline->video_queue = NULL;
    pipeline->video_convert = NULL;
    pipeline->fps_sink = NULL;
    pipeline->audio_source = NULL;
    pipeline->audio_sink = NULL;
    pipeline->audio_queue = NULL;
    pipeline->pipeline_started_us = 0;
    g_clear_pointer(&pipeline->diagnostic_context, g_free);
    g_clear_pointer(&pipeline->diagnostic_expected_caps, g_free);
    g_clear_pointer(&pipeline->audio_identity, g_free);
    g_clear_pointer(&pipeline->audio_status, g_free);
    g_mutex_lock(&pipeline->stats_mutex);
    pipeline->stats.current_fps = 0.0;
    pipeline->stats.average_fps = 0.0;
    pipeline->stats.frames_dropped = 0;
    pipeline->stats.queue_level = 0;
    pipeline->last_video_frame_us = 0;
    pipeline->stats.audio_source_latency_us = -1;
    pipeline->stats.audio_source_buffer_us = -1;
    g_mutex_unlock(&pipeline->stats_mutex);
}

static gboolean pipeline_bus_message(GstBus *bus, GstMessage *message, gpointer user_data);

CapturePipeline *
capture_pipeline_new(CaptureRenderer *renderer,
                    const CapturePipelineCallbacks *callbacks,
                    gpointer user_data)
{
    CapturePipeline *pipeline = g_new0(CapturePipeline, 1);
    pipeline->renderer = renderer;
    if (callbacks != NULL)
        pipeline->callbacks = *callbacks;
    pipeline->user_data = user_data;
    pipeline->stats.audio_source_latency_us = -1;
    pipeline->stats.audio_source_buffer_us = -1;
    g_mutex_init(&pipeline->stats_mutex);
    return pipeline;
}

void
capture_pipeline_stop(CapturePipeline *pipeline)
{
    if (pipeline != NULL)
        pipeline_stop(pipeline);
}

void
capture_pipeline_free(CapturePipeline *pipeline)
{
    if (pipeline == NULL)
        return;
    pipeline_stop(pipeline);
    g_free(pipeline->error);
    g_mutex_clear(&pipeline->stats_mutex);
    g_free(pipeline);
}

gboolean
capture_pipeline_start(CapturePipeline *pipeline,
                       const CaptureDevice *device,
                       const CaptureVideoNode *node,
                       const CaptureMode *mode,
                       GstDevice *audio_device,
                       gboolean audio_enabled,
                       gdouble volume,
                       const gchar *audio_identity,
                       const gchar *audio_status)
{
    if (device == NULL || node == NULL || mode == NULL)
        return FALSE;


    g_clear_pointer(&pipeline->diagnostic_context, g_free);
    g_clear_pointer(&pipeline->diagnostic_expected_caps, g_free);
    g_clear_pointer(&pipeline->audio_identity, g_free);
    g_clear_pointer(&pipeline->audio_status, g_free);
    pipeline->diagnostic_context =
        capture_diagnostic_context_base(device, node, mode);
    pipeline->diagnostic_expected_caps =
        capture_pipeline_mode_caps_description(mode);
    pipeline->audio_identity = g_strdup(audio_identity);
    pipeline->audio_status = g_strdup(audio_status);
    pipeline->pipeline = gst_pipeline_new("captureviewer-pipeline");
    if (pipeline->pipeline == NULL) {
        g_free(pipeline->error);
        pipeline->error = g_strdup("Could not allocate GStreamer pipeline");
        return FALSE;
    }

    GError *error = NULL;
    if (!setup_video_branch(pipeline, node, mode, &error) ||
        !setup_audio_branch(pipeline, audio_device, audio_enabled, volume, &error)) {
        gchar *context = capture_diagnostic_context(pipeline);
        const gchar *domain = error != NULL ? g_quark_to_string(error->domain) : NULL;
        pipeline_log(pipeline, "Pipeline setup failed: %s; domain=%s code=%d; %s",
                    error != NULL ? error->message : "unknown error",
                    domain != NULL ? domain : "unknown",
                    error != NULL ? error->code : -1, context);
        g_free(pipeline->error);
        pipeline->error = g_strdup_printf(
            "%s (domain=%s, code=%d)\n%s",
            error != NULL ? error->message : "Pipeline setup failed",
            domain != NULL ? domain : "unknown",
            error != NULL ? error->code : -1, context);
        g_free(context);
        if (error != NULL)
            g_error_free(error);
        pipeline_stop(pipeline);
        return FALSE;
    }

    pipeline->bus = gst_element_get_bus(pipeline->pipeline);
    pipeline->bus_watch_id = gst_bus_add_watch(pipeline->bus, pipeline_bus_message, pipeline);
    GstStateChangeReturn state = gst_element_set_state(pipeline->pipeline, GST_STATE_PLAYING);
    if (state == GST_STATE_CHANGE_FAILURE) {
        GstState current = GST_STATE_VOID_PENDING;
        GstState pending = GST_STATE_VOID_PENDING;
        GstStateChangeReturn observed =
            gst_element_get_state(pipeline->pipeline, &current, &pending, 0);
        gchar *context = capture_diagnostic_context(pipeline);
        pipeline_log(pipeline,
                    "GStreamer refused PLAYING for %s (state=%s pending=%s "
                    "query-result=%d); %s",
                    mode->label, gst_element_state_get_name(current),
                    gst_element_state_get_name(pending), observed, context);
        g_free(pipeline->error);
        pipeline->error = g_strdup_printf(
            "GStreamer refused PLAYING (state=%s, pending=%s, query-result=%d)\n%s",
            gst_element_state_get_name(current), gst_element_state_get_name(pending),
            observed, context);
        g_free(context);
        pipeline_stop(pipeline);
        if (pipeline->callbacks.state_changed != NULL)
            pipeline->callbacks.state_changed(pipeline->user_data);
        return FALSE;
    }
    pipeline->pipeline_started_us = g_get_monotonic_time();

    g_clear_pointer(&pipeline->error, g_free);
    pipeline_log(pipeline, "Started %s on %s; audio %s%s", mode->label,
                 node->path, audio_enabled && audio_device != NULL ? "enabled" : "disabled",
                 audio_enabled && audio_device == NULL ? " (no audio source matched)" : "");
    return TRUE;
}

void
capture_pipeline_set_volume(CapturePipeline *pipeline, gdouble volume)
{
    if (pipeline->audio_sink != NULL)
        g_object_set(pipeline->audio_sink, "volume", volume, NULL);
}

gboolean
capture_pipeline_is_running(const CapturePipeline *pipeline)
{
    return pipeline->pipeline != NULL;
}

gboolean
capture_pipeline_is_waiting_for_frames(CapturePipeline *pipeline)
{
    if (pipeline->pipeline == NULL || pipeline->pipeline_started_us == 0)
        return FALSE;
    g_mutex_lock(&pipeline->stats_mutex);
    gint64 last_frame_us = pipeline->last_video_frame_us;
    g_mutex_unlock(&pipeline->stats_mutex);
    gint64 last_activity_us = last_frame_us > pipeline->pipeline_started_us
        ? last_frame_us : pipeline->pipeline_started_us;
    return g_get_monotonic_time() - last_activity_us >= 2 * G_USEC_PER_SEC;
}

void
capture_pipeline_clear_error(CapturePipeline *pipeline)
{
    g_clear_pointer(&pipeline->error, g_free);
}

gboolean
capture_pipeline_has_audio_source(const CapturePipeline *pipeline)
{
    return pipeline->audio_source != NULL;
}

const gchar *
capture_pipeline_get_error(const CapturePipeline *pipeline)
{
    return pipeline->error;
}

void
capture_pipeline_update_stats(CapturePipeline *pipeline)
{
    if (pipeline->pipeline == NULL)
        return;
    guint dropped = 0, queued = 0;
    if (pipeline->fps_sink != NULL)
        g_object_get(pipeline->fps_sink, "frames-dropped", &dropped, NULL);
    if (pipeline->video_queue != NULL)
        g_object_get(pipeline->video_queue, "current-level-buffers", &queued, NULL);
    gint64 source_latency = -1, source_buffer = -1;
    if (pipeline->audio_source != NULL) {
        g_object_get(pipeline->audio_source, "actual-latency-time", &source_latency,
                     "actual-buffer-time", &source_buffer, NULL);
    }
    gint64 cpu_time = process_cpu_time_us();
    gint64 wall_time = g_get_monotonic_time();
    g_mutex_lock(&pipeline->stats_mutex);
    if (pipeline->last_cpu_time_us > 0 && wall_time > pipeline->last_cpu_time_us) {
        pipeline->stats.cpu_percent =
            (gdouble)(cpu_time - pipeline->last_cpu_us) * 100.0 /
            (wall_time - pipeline->last_cpu_time_us);
    }
    pipeline->last_cpu_us = cpu_time;
    pipeline->last_cpu_time_us = wall_time;
    pipeline->stats.frames_dropped = dropped;
    pipeline->stats.queue_level = queued;
    pipeline->stats.audio_source_latency_us = source_latency;
    pipeline->stats.audio_source_buffer_us = source_buffer;
    g_mutex_unlock(&pipeline->stats_mutex);
}

CapturePipelineStats
capture_pipeline_get_stats(CapturePipeline *pipeline)
{
    CapturePipelineStats stats;
    g_mutex_lock(&pipeline->stats_mutex);
    stats = pipeline->stats;
    g_mutex_unlock(&pipeline->stats_mutex);
    return stats;
}

static void
on_fps_measurements(GstElement *element, gdouble fps, gdouble droprate,
                    gdouble average, gpointer user_data)
{
    CapturePipeline *pipeline = user_data;
    g_mutex_lock(&pipeline->stats_mutex);
    pipeline->stats.current_fps = fps;
    pipeline->stats.average_fps = average;
    g_mutex_unlock(&pipeline->stats_mutex);
    (void)element;
    (void)droprate;
}

static gboolean
pipeline_bus_message(GstBus *bus, GstMessage *message, gpointer user_data)
{
    CapturePipeline *pipeline = user_data;
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        gchar *context = capture_diagnostic_context(pipeline);
        const gchar *domain = error != NULL ? g_quark_to_string(error->domain) : NULL;
        pipeline_log(pipeline,
                    "GStreamer ERROR domain=%s code=%d element=%s type=%s message=%s "
                    "debug=%s; %s",
                    domain != NULL ? domain : "unknown",
                    error != NULL ? error->code : -1,
                    GST_OBJECT_NAME(message->src),
                    G_OBJECT_TYPE_NAME(message->src),
                    error != NULL ? error->message : "unknown error",
                    debug != NULL ? debug : "unavailable", context);
        g_free(pipeline->error);
        pipeline->error = g_strdup_printf(
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
        pipeline->bus_watch_id = 0;
        pipeline_stop(pipeline);
        if (pipeline->callbacks.state_changed != NULL)
            pipeline->callbacks.state_changed(pipeline->user_data);
        return G_SOURCE_REMOVE;
    }
    case GST_MESSAGE_WARNING: {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_warning(message, &error, &debug);
        gchar *context = capture_diagnostic_context(pipeline);
        gchar *audio_detail = NULL;
        if (pipeline->audio_source != NULL &&
            GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline->audio_source)) {
            guint64 queue_time = 0;
            guint queue_buffers = 0;
            gint64 source_latency = -1;
            gint64 source_buffer = -1;
            if (pipeline->audio_queue != NULL)
                g_object_get(pipeline->audio_queue,
                             "current-level-time", &queue_time,
                             "current-level-buffers", &queue_buffers, NULL);
            if (pipeline->audio_source != NULL &&
                g_object_class_find_property(
                    G_OBJECT_GET_CLASS(pipeline->audio_source),
                    "actual-latency-time") != NULL)
                g_object_get(pipeline->audio_source,
                             "actual-latency-time", &source_latency,
                             "actual-buffer-time", &source_buffer, NULL);
            gchar *source_caps = element_current_caps(pipeline->audio_source, "src");
            gchar *sink_caps = element_current_caps(pipeline->audio_sink, "sink");
            audio_detail = g_strdup_printf(
                "; audio-source-caps=%s audio-sink-caps=%s "
                "audio-source-actual-latency=%" G_GINT64_FORMAT "us "
                "audio-source-actual-buffer=%" G_GINT64_FORMAT "us "
                "audio-queue=%u buffers/%" G_GUINT64_FORMAT "ns",
                source_caps, sink_caps, source_latency, source_buffer,
                queue_buffers, queue_time);
            g_free(source_caps);
            g_free(sink_caps);
        }
        pipeline_log(pipeline, "GStreamer WARNING element=%s message=%s debug=%s; %s%s",
                    GST_OBJECT_NAME(message->src),
                    error != NULL ? error->message : "unknown warning",
                    debug != NULL ? debug : "unavailable", context,
                    audio_detail != NULL ? audio_detail : "");
        g_free(audio_detail);
        g_free(context);
        if (error != NULL)
            g_error_free(error);
        g_free(debug);
        break;
    }
    case GST_MESSAGE_LATENCY:
        gst_bin_recalculate_latency(GST_BIN(pipeline->pipeline));
        break;
    case GST_MESSAGE_EOS:
        pipeline_log(pipeline, "Unexpected end-of-stream from live capture pipeline");
        pipeline->bus_watch_id = 0;
        pipeline_stop(pipeline);
        if (pipeline->callbacks.state_changed != NULL)
            pipeline->callbacks.state_changed(pipeline->user_data);
        return G_SOURCE_REMOVE;
    default:
        break;
    }
    (void)bus;
    return G_SOURCE_CONTINUE;
}
