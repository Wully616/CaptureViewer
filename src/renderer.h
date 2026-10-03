#pragma once

#include <gtk/gtk.h>
#include <gst/gst.h>

typedef enum {
    CAPTURE_RENDERER_FIT,
    CAPTURE_RENDERER_FILL,
} CaptureRendererScaleMode;

typedef struct _CaptureRenderer CaptureRenderer;

typedef struct {
    GdkRectangle viewport;
    gdouble u0;
    gdouble v0;
    gdouble u1;
    gdouble v1;
} CaptureRendererLayout;

CaptureRenderer *capture_renderer_new(GError **error);
GtkWidget *capture_renderer_get_widget(CaptureRenderer *renderer);
GstElement *capture_renderer_create_sink(CaptureRenderer *renderer);
void capture_renderer_set_scale_mode(CaptureRenderer *renderer,
                                     CaptureRendererScaleMode mode);
void capture_renderer_pipeline_stopped(CaptureRenderer *renderer);
void capture_renderer_connect_motion_events(
    CaptureRenderer *renderer,
    gboolean (*callback)(GtkWidget *, GdkEventMotion *, gpointer),
    gpointer user_data);
const gchar *capture_renderer_get_backend_name(const CaptureRenderer *renderer);
void capture_renderer_free(CaptureRenderer *renderer);
gboolean capture_renderer_compute_layout(guint source_width,
                                         guint source_height,
                                         guint pixel_aspect_num,
                                         guint pixel_aspect_den,
                                         guint area_width,
                                         guint area_height,
                                         CaptureRendererScaleMode mode,
                                         CaptureRendererLayout *layout);
