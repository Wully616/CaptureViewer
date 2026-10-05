#include "renderer.h"

#include <epoxy/gl.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TIMING_SAMPLE_WINDOW 256
#define SOURCE_TIMING_SLOTS 64

typedef enum {
    TIMING_SOURCE_TO_DECODED,
    TIMING_DECODED_TO_APPSINK,
    TIMING_APPSINK_TO_DISPATCH,
    TIMING_DISPATCH_TO_GL,
    TIMING_APPSINK_TO_GL,
    TIMING_SOURCE_TO_GL,
    TIMING_METRIC_COUNT,
} TimingMetric;

enum {
    FRAME_TIMING_SOURCE = 1u << 0,
    FRAME_TIMING_DECODED = 1u << 1,
    FRAME_TIMING_APPSINK = 1u << 2,
    FRAME_TIMING_DISPATCH = 1u << 3,
    FRAME_TIMING_GL = 1u << 4,
};

typedef struct {
    gboolean valid;
    gboolean ambiguous;
    gboolean decoded;
    GstClockTime pts;
    gint64 source_us;
    gint64 decoded_us;
} SourceFrameTiming;

typedef struct {
    guint valid_mask;
    gint64 source_us;
    gint64 decoded_us;
    gint64 appsink_us;
    gint64 dispatch_us;
    gint64 gl_us;
} FrameTiming;

typedef struct {
    gint64 samples_us[TIMING_SAMPLE_WINDOW];
    guint sample_count;
    guint next_sample;
    gint64 sum_us;
} TimingMetricSamples;

struct _CaptureRenderer {
    gint ref_count;
    GtkWidget *stack;
    GtkWidget *gl_area;
    GtkWidget *cairo_area;
    gulong gl_motion_handler_id;
    gulong cairo_motion_handler_id;
    GMainContext *main_context;
    GMutex sample_mutex;
    GMutex timing_mutex;
    GstSample *latest_sample;
    FrameTiming latest_frame_timing;
    GstElement *sink;
    gchar *backend_name;
    guint64 sample_generation;
    guint64 last_timed_generation;
    guint64 uploaded_generation;
    guint source_width;
    guint source_height;
    guint pixel_aspect_num;
    guint pixel_aspect_den;
    GLuint texture;
    GLuint chroma_u_texture;
    GLuint chroma_v_texture;
    GLuint program;
    GLuint vertex_array;
    GLuint vertex_buffer;
    GLint position_location;
    GLint texcoord_location;
    GLint chroma_u_sampler_location;
    GLint chroma_v_sampler_location;
    GLint use_i420_location;
    GLint yuv_offset_location;
    GLint yuv_scale_location;
    GLint yuv_coefficients_location;
    GLint sampler_location;
    guint texture_width;
    guint texture_height;
    GstVideoFormat texture_format;
    CaptureRendererScaleMode scale_mode;
    gboolean dispatch_pending;
    gboolean closing;
    gboolean gl_initialized;
    SourceFrameTiming source_timing[SOURCE_TIMING_SLOTS];
    guint source_timing_next;
    TimingMetricSamples timing_metrics[TIMING_METRIC_COUNT];
};

static CaptureRenderer *capture_renderer_ref(CaptureRenderer *renderer);
static void capture_renderer_unref(CaptureRenderer *renderer);
static gboolean timing_delta_us(guint valid_mask, guint start_flag,
                                gint64 start_us, guint end_flag,
                                gint64 end_us, gint64 *duration_us);
static void timing_metric_add_locked(CaptureRenderer *renderer,
                                     TimingMetric metric, gint64 duration_us);
static void record_frame_timing(CaptureRenderer *renderer,
                                const FrameTiming *timing);
static FrameTiming frame_timing_at_appsink(CaptureRenderer *renderer,
                                           GstBuffer *buffer,
                                           gint64 appsink_us);
static void reset_renderer_timing(CaptureRenderer *renderer);

static GLuint
compile_shader(GLenum type, const gchar *source, GError **error)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    gchar *log = g_malloc0((gsize)MAX(log_length, 1));
    glGetShaderInfoLog(shader, log_length, NULL, log);
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Could not compile renderer shader: %s", log);
    g_free(log);
    glDeleteShader(shader);
    return 0;
}

static gboolean
create_gl_resources(CaptureRenderer *renderer, GError **error)
{
    static const gchar *vertex_source =
        "#version 150\n"
        "in vec2 position;\n"
        "in vec2 texcoord;\n"
        "out vec2 frame_texcoord;\n"
        "void main() {\n"
        "    frame_texcoord = texcoord;\n"
        "    gl_Position = vec4(position, 0.0, 1.0);\n"
        "}\n";
    static const gchar *fragment_source =
        "#version 150\n"
        "uniform sampler2D frame_texture;\n"
        "uniform sampler2D frame_u_texture;\n"
        "uniform sampler2D frame_v_texture;\n"
        "uniform int use_i420;\n"
        "uniform vec3 yuv_offset;\n"
        "uniform vec3 yuv_scale;\n"
        "uniform vec4 yuv_coefficients;\n"
        "in vec2 frame_texcoord;\n"
        "out vec4 color;\n"
        "void main() {\n"
        "    if (use_i420 == 0) {\n"
        "        color = texture(frame_texture, frame_texcoord);\n"
        "    } else {\n"
        "        vec3 yuv = vec3(texture(frame_texture, frame_texcoord).r,\n"
        "                        texture(frame_u_texture, frame_texcoord).r,\n"
        "                        texture(frame_v_texture, frame_texcoord).r);\n"
        "        yuv = (yuv - yuv_offset) * yuv_scale;\n"
        "        color = vec4(yuv.x + yuv_coefficients.x * yuv.z,\n"
        "                     yuv.x + yuv_coefficients.y * yuv.y + "
        "yuv_coefficients.z * yuv.z,\n"
        "                     yuv.x + yuv_coefficients.w * yuv.y, 1.0);\n"
        "    }\n"
        "}\n";

    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source, error);
    if (vertex == 0)
        return FALSE;
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source, error);
    if (fragment == 0) {
        glDeleteShader(vertex);
        return FALSE;
    }

    renderer->program = glCreateProgram();
    glAttachShader(renderer->program, vertex);
    glAttachShader(renderer->program, fragment);
    glBindAttribLocation(renderer->program, 0, "position");
    glBindAttribLocation(renderer->program, 1, "texcoord");
    glLinkProgram(renderer->program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(renderer->program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint log_length = 0;
        glGetProgramiv(renderer->program, GL_INFO_LOG_LENGTH, &log_length);
        gchar *log = g_malloc0((gsize)MAX(log_length, 1));
        glGetProgramInfoLog(renderer->program, log_length, NULL, log);
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Could not link renderer shader program: %s", log);
        g_free(log);
        glDeleteProgram(renderer->program);
        renderer->program = 0;
        return FALSE;
    }

    renderer->position_location = glGetAttribLocation(renderer->program, "position");
    renderer->texcoord_location = glGetAttribLocation(renderer->program, "texcoord");
    renderer->sampler_location = glGetUniformLocation(renderer->program, "frame_texture");
    renderer->chroma_u_sampler_location =
        glGetUniformLocation(renderer->program, "frame_u_texture");
    renderer->chroma_v_sampler_location =
        glGetUniformLocation(renderer->program, "frame_v_texture");
    renderer->use_i420_location =
        glGetUniformLocation(renderer->program, "use_i420");
    renderer->yuv_offset_location =
        glGetUniformLocation(renderer->program, "yuv_offset");
    renderer->yuv_scale_location =
        glGetUniformLocation(renderer->program, "yuv_scale");
    renderer->yuv_coefficients_location =
        glGetUniformLocation(renderer->program, "yuv_coefficients");
    glGenVertexArrays(1, &renderer->vertex_array);
    glGenBuffers(1, &renderer->vertex_buffer);
    glGenTextures(1, &renderer->texture);
    glGenTextures(1, &renderer->chroma_u_texture);
    glGenTextures(1, &renderer->chroma_v_texture);
    GLuint textures[] = {
        renderer->texture, renderer->chroma_u_texture, renderer->chroma_v_texture
    };
    for (guint i = 0; i < G_N_ELEMENTS(textures); i++) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    renderer->gl_initialized = TRUE;
    return TRUE;
}

static void
set_backend_name(CaptureRenderer *renderer, const gchar *name)
{
    g_free(renderer->backend_name);
    renderer->backend_name = g_strdup(name);
}

static void
set_sink_format(CaptureRenderer *renderer, GstVideoFormat format)
{
    if (renderer->sink == NULL)
        return;

    GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                        "format", G_TYPE_STRING,
                                        gst_video_format_to_string(format),
                                        NULL);
    g_object_set(renderer->sink, "caps", caps, NULL);
    gst_caps_unref(caps);
}

static void
use_cairo_fallback(CaptureRenderer *renderer, const gchar *reason)
{
    g_warning("CaptureRenderer: GtkGLArea unavailable (%s); using Cairo", reason);
    set_backend_name(renderer, "GtkDrawingArea/Cairo");
    set_sink_format(renderer, GST_VIDEO_FORMAT_RGBA);
    gtk_stack_set_visible_child(GTK_STACK(renderer->stack), renderer->cairo_area);
    gtk_widget_queue_draw(renderer->cairo_area);
}

static void
on_gl_realize(GtkGLArea *area, gpointer user_data)
{
    CaptureRenderer *renderer = user_data;
    gtk_gl_area_make_current(area);
    GError *error = gtk_gl_area_get_error(area);
    if (error != NULL) {
        use_cairo_fallback(renderer, error->message);
        return;
    }

    if (!create_gl_resources(renderer, &error)) {
        use_cairo_fallback(renderer, error != NULL ? error->message : "GL initialization failed");
        g_clear_error(&error);
        return;
    }

    set_sink_format(renderer, GST_VIDEO_FORMAT_I420);
    const gchar *vendor = (const gchar *)glGetString(GL_VENDOR);
    const gchar *name = (const gchar *)glGetString(GL_RENDERER);
    const gchar *version = (const gchar *)glGetString(GL_VERSION);
    gchar *backend = g_strdup_printf("GtkGLArea/OpenGL (vendor=%s, renderer=%s, version=%s)",
                                     vendor != NULL ? vendor : "unknown",
                                     name != NULL ? name : "unknown",
                                     version != NULL ? version : "unknown");
    set_backend_name(renderer, backend);
    g_free(backend);
    g_message("CaptureRenderer GL context: vendor=%s renderer=%s version=%s",
              vendor != NULL ? vendor : "unknown",
              name != NULL ? name : "unknown",
              version != NULL ? version : "unknown");
}

static void
on_gl_unrealize(GtkGLArea *area, gpointer user_data)
{
    CaptureRenderer *renderer = user_data;
    if (!renderer->gl_initialized)
        return;

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) == NULL) {
        glDeleteTextures(1, &renderer->texture);
        glDeleteTextures(1, &renderer->chroma_u_texture);
        glDeleteTextures(1, &renderer->chroma_v_texture);
        glDeleteBuffers(1, &renderer->vertex_buffer);
        glDeleteVertexArrays(1, &renderer->vertex_array);
        glDeleteProgram(renderer->program);
    }
    renderer->texture = 0;
    renderer->chroma_u_texture = 0;
    renderer->chroma_v_texture = 0;
    renderer->vertex_buffer = 0;
    renderer->vertex_array = 0;
    renderer->program = 0;
    renderer->texture_width = 0;
    renderer->texture_height = 0;
    renderer->gl_initialized = FALSE;
}

gboolean
capture_renderer_compute_layout(guint source_width,
                                guint source_height,
                                guint pixel_aspect_num,
                                guint pixel_aspect_den,
                                guint area_width,
                                guint area_height,
                                CaptureRendererScaleMode mode,
                                CaptureRendererLayout *layout)
{
    if (layout == NULL)
        return FALSE;
    *layout = (CaptureRendererLayout){0};
    if (source_width == 0 || source_height == 0 ||
        pixel_aspect_num == 0 || pixel_aspect_den == 0 ||
        area_width == 0 || area_height == 0 ||
        area_width > G_MAXINT || area_height > G_MAXINT ||
        (mode != CAPTURE_RENDERER_FIT && mode != CAPTURE_RENDERER_FILL))
        return FALSE;

    long double source_aspect =
        ((long double)source_width * pixel_aspect_num) /
        ((long double)source_height * pixel_aspect_den);
    long double area_aspect = (long double)area_width / area_height;
    if (!isfinite((double)source_aspect) || source_aspect <= 0.0L ||
        !isfinite((double)area_aspect) || area_aspect <= 0.0L)
        return FALSE;

    if (mode == CAPTURE_RENDERER_FIT) {
        guint viewport_width;
        guint viewport_height;
        if (source_aspect > area_aspect) {
            viewport_width = area_width;
            viewport_height = (guint)floorl((long double)area_width / source_aspect + 0.5L);
            viewport_height = CLAMP(viewport_height, 1, area_height);
        } else {
            viewport_height = area_height;
            viewport_width = (guint)floorl((long double)area_height * source_aspect + 0.5L);
            viewport_width = CLAMP(viewport_width, 1, area_width);
        }
        layout->viewport.x = (gint)((area_width - viewport_width) / 2);
        layout->viewport.y = (gint)((area_height - viewport_height) / 2);
        layout->viewport.width = (gint)viewport_width;
        layout->viewport.height = (gint)viewport_height;
        layout->u1 = 1.0;
        layout->v1 = 1.0;
        return TRUE;
    }

    layout->viewport = (GdkRectangle){0, 0, (gint)area_width, (gint)area_height};
    layout->u1 = 1.0;
    layout->v1 = 1.0;
    if (source_aspect > area_aspect) {
        gdouble visible_fraction = (gdouble)(area_aspect / source_aspect);
        layout->u0 = (1.0 - visible_fraction) / 2.0;
        layout->u1 = 1.0 - layout->u0;
    } else if (source_aspect < area_aspect) {
        gdouble visible_fraction = (gdouble)(source_aspect / area_aspect);
        layout->v0 = (1.0 - visible_fraction) / 2.0;
        layout->v1 = 1.0 - layout->v0;
    }
    return TRUE;
}

static GstSample *
get_latest_sample(CaptureRenderer *renderer, guint64 *generation,
                  FrameTiming *timing)
{
    g_mutex_lock(&renderer->sample_mutex);
    GstSample *sample = renderer->latest_sample != NULL
        ? gst_sample_ref(renderer->latest_sample) : NULL;
    *generation = renderer->sample_generation;
    if (timing != NULL)
        *timing = renderer->latest_frame_timing;
    g_mutex_unlock(&renderer->sample_mutex);
    return sample;
}

static gboolean
map_video_sample(GstSample *sample, GstVideoInfo *info, GstVideoFrame *frame)
{
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (caps == NULL || buffer == NULL || !gst_video_info_from_caps(info, caps))
        return FALSE;
    GstVideoFormat format = GST_VIDEO_INFO_FORMAT(info);
    if (format != GST_VIDEO_FORMAT_RGBA && format != GST_VIDEO_FORMAT_I420)
        return FALSE;
    return gst_video_frame_map(frame, info, buffer, GST_MAP_READ);
}

static gboolean
upload_i420_frame(CaptureRenderer *renderer, const GstVideoInfo *info,
                  const GstVideoFrame *frame)
{
    guint width = GST_VIDEO_INFO_WIDTH(info);
    guint height = GST_VIDEO_INFO_HEIGHT(info);
    guint chroma_width = GST_VIDEO_FRAME_COMP_WIDTH(frame, 1);
    guint chroma_height = GST_VIDEO_FRAME_COMP_HEIGHT(frame, 1);
    gint y_stride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
    gint u_stride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 1);
    gint v_stride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 2);
    if (y_stride < (gint)width || u_stride < (gint)chroma_width ||
        v_stride < (gint)chroma_width)
        return FALSE;

    gboolean reallocate = width != renderer->texture_width ||
        height != renderer->texture_height ||
        renderer->texture_format != GST_VIDEO_FORMAT_I420;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, renderer->texture);
    if (reallocate)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)width,
                     (GLsizei)height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width,
                    (GLsizei)height, GL_RED, GL_UNSIGNED_BYTE,
                    GST_VIDEO_FRAME_PLANE_DATA(frame, 0));

    glBindTexture(GL_TEXTURE_2D, renderer->chroma_u_texture);
    if (reallocate)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)chroma_width,
                     (GLsizei)chroma_height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, u_stride);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)chroma_width,
                    (GLsizei)chroma_height, GL_RED, GL_UNSIGNED_BYTE,
                    GST_VIDEO_FRAME_PLANE_DATA(frame, 1));

    glBindTexture(GL_TEXTURE_2D, renderer->chroma_v_texture);
    if (reallocate)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)chroma_width,
                     (GLsizei)chroma_height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, v_stride);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)chroma_width,
                    (GLsizei)chroma_height, GL_RED, GL_UNSIGNED_BYTE,
                    GST_VIDEO_FRAME_PLANE_DATA(frame, 2));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    renderer->texture_width = width;
    renderer->texture_height = height;
    renderer->texture_format = GST_VIDEO_FORMAT_I420;
    return TRUE;
}

static gboolean
layout_for_area(CaptureRenderer *renderer, GtkWidget *area,
                guint width, guint height, CaptureRendererLayout *layout)
{
    g_mutex_lock(&renderer->sample_mutex);
    guint source_width = renderer->source_width;
    guint source_height = renderer->source_height;
    guint par_num = renderer->pixel_aspect_num;
    guint par_den = renderer->pixel_aspect_den;
    CaptureRendererScaleMode mode = renderer->scale_mode;
    g_mutex_unlock(&renderer->sample_mutex);
    (void)area;
    return capture_renderer_compute_layout(source_width, source_height,
                                           par_num, par_den, width, height,
                                           mode, layout);
}

static gboolean
on_gl_render(GtkGLArea *area, GdkGLContext *context, gpointer user_data)
{
    CaptureRenderer *renderer = user_data;
    gint64 render_callback_us = g_get_monotonic_time();
    gint allocated_width = gtk_widget_get_allocated_width(GTK_WIDGET(area));
    gint allocated_height = gtk_widget_get_allocated_height(GTK_WIDGET(area));
    gint scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    if (allocated_width <= 0 || allocated_height <= 0)
        return TRUE;

    gtk_gl_area_make_current(area);
    GError *error = gtk_gl_area_get_error(area);
    if (error != NULL) {
        use_cairo_fallback(renderer, error->message);
        return TRUE;
    }
    if (!renderer->gl_initialized) {
        use_cairo_fallback(renderer, "GL resources are not initialized");
        return TRUE;
    }

    glViewport(0, 0, allocated_width * scale, allocated_height * scale);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    guint64 generation = 0;
    FrameTiming frame_timing = {0};
    GstSample *sample = get_latest_sample(renderer, &generation,
                                          &frame_timing);
    if (sample == NULL)
        return TRUE;
    if (generation != renderer->last_timed_generation) {
        renderer->last_timed_generation = generation;
        if ((frame_timing.valid_mask & FRAME_TIMING_APPSINK) != 0 &&
            render_callback_us >= frame_timing.appsink_us) {
            frame_timing.gl_us = render_callback_us;
            frame_timing.valid_mask |= FRAME_TIMING_GL;
            record_frame_timing(renderer, &frame_timing);
        }
    }

    GstVideoInfo info;
    GstVideoFrame frame;
    if (!map_video_sample(sample, &info, &frame)) {
        gst_sample_unref(sample);
        return TRUE;
    }

    guint width = GST_VIDEO_INFO_WIDTH(&info);
    guint height = GST_VIDEO_INFO_HEIGHT(&info);
    GstVideoFormat format = GST_VIDEO_INFO_FORMAT(&info);
    if (generation != renderer->uploaded_generation ||
        width != renderer->texture_width || height != renderer->texture_height ||
        format != renderer->texture_format) {
        if (format == GST_VIDEO_FORMAT_I420) {
            if (upload_i420_frame(renderer, &info, &frame))
                renderer->uploaded_generation = generation;
        } else {
            gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
            const guint8 *pixels = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
            if (stride > 0 && stride % 4 == 0) {
                glBindTexture(GL_TEXTURE_2D, renderer->texture);
                if (width != renderer->texture_width ||
                    height != renderer->texture_height ||
                    renderer->texture_format != GST_VIDEO_FORMAT_RGBA) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width,
                                 (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 NULL);
                    renderer->texture_width = width;
                    renderer->texture_height = height;
                    renderer->texture_format = GST_VIDEO_FORMAT_RGBA;
                }
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width,
                                (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE,
                                pixels);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                renderer->uploaded_generation = generation;
            }
        }
    }

    CaptureRendererLayout layout;
    if (layout_for_area(renderer, GTK_WIDGET(area), (guint)allocated_width,
                        (guint)allocated_height, &layout)) {
        gfloat x0 = 2.0f * (gfloat)layout.viewport.x / allocated_width - 1.0f;
        gfloat x1 = 2.0f * (gfloat)(layout.viewport.x + layout.viewport.width) /
                    allocated_width - 1.0f;
        gfloat y0 = 1.0f - 2.0f * (gfloat)layout.viewport.y / allocated_height;
        gfloat y1 = 1.0f - 2.0f * (gfloat)(layout.viewport.y + layout.viewport.height) /
                    allocated_height;
        GLfloat vertices[] = {
            x0, y1, (GLfloat)layout.u0, (GLfloat)layout.v1,
            x1, y1, (GLfloat)layout.u1, (GLfloat)layout.v1,
            x0, y0, (GLfloat)layout.u0, (GLfloat)layout.v0,
            x1, y0, (GLfloat)layout.u1, (GLfloat)layout.v0,
        };
        glUseProgram(renderer->program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, renderer->texture);
        glUniform1i(renderer->sampler_location, 0);
        glUniform1i(renderer->chroma_u_sampler_location, 1);
        glUniform1i(renderer->chroma_v_sampler_location, 2);
        glUniform1i(renderer->use_i420_location,
                    format == GST_VIDEO_FORMAT_I420);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, renderer->chroma_u_texture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, renderer->chroma_v_texture);
        glActiveTexture(GL_TEXTURE0);
        if (format == GST_VIDEO_FORMAT_I420) {
            gdouble kr = 0.299;
            gdouble kb = 0.114;
            if (!gst_video_color_matrix_get_Kr_Kb(info.colorimetry.matrix,
                                                  &kr, &kb)) {
                kr = 0.299;
                kb = 0.114;
            }
            gdouble kg = 1.0 - kr - kb;
            GLfloat y_offset = 16.0f / 255.0f;
            GLfloat uv_offset = 128.0f / 255.0f;
            GLfloat y_scale = 255.0f / 219.0f;
            GLfloat uv_scale = 255.0f / 224.0f;
            if (info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255) {
                y_offset = 0.0f;
                uv_offset = 0.5f;
                y_scale = 1.0f;
                uv_scale = 1.0f;
            }
            glUniform3f(renderer->yuv_offset_location, y_offset,
                        uv_offset, uv_offset);
            glUniform3f(renderer->yuv_scale_location, y_scale,
                        uv_scale, uv_scale);
            glUniform4f(renderer->yuv_coefficients_location,
                        (GLfloat)(2.0 * (1.0 - kr)),
                        (GLfloat)(-2.0 * kb * (1.0 - kb) / kg),
                        (GLfloat)(-2.0 * kr * (1.0 - kr) / kg),
                        (GLfloat)(2.0 * (1.0 - kb)));
        }
        glBindVertexArray(renderer->vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, renderer->vertex_buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
        glEnableVertexAttribArray((GLuint)renderer->position_location);
        glVertexAttribPointer((GLuint)renderer->position_location, 2, GL_FLOAT,
                              GL_FALSE, 4 * sizeof(GLfloat), (void *)0);
        glEnableVertexAttribArray((GLuint)renderer->texcoord_location);
        glVertexAttribPointer((GLuint)renderer->texcoord_location, 2, GL_FLOAT,
                              GL_FALSE, 4 * sizeof(GLfloat),
                              (void *)(2 * sizeof(GLfloat)));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    gst_video_frame_unmap(&frame);
    gst_sample_unref(sample);
    (void)context;
    return TRUE;
}

static gboolean
on_cairo_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    CaptureRenderer *renderer = user_data;
    gint width = gtk_widget_get_allocated_width(widget);
    gint height = gtk_widget_get_allocated_height(widget);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    if (width <= 0 || height <= 0)
        return TRUE;

    guint64 generation = 0;
    GstSample *sample = get_latest_sample(renderer, &generation, NULL);
    if (sample == NULL)
        return TRUE;

    GstVideoInfo info;
    GstVideoFrame frame;
    if (map_video_sample(sample, &info, &frame)) {
        if (GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_RGBA) {
            gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
            const guint8 *pixels = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
            CaptureRendererLayout layout;
            if (stride > 0 &&
                layout_for_area(renderer, widget, (guint)width,
                                (guint)height, &layout)) {
                GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(
                    pixels, GDK_COLORSPACE_RGB, TRUE, 8,
                    (gint)GST_VIDEO_INFO_WIDTH(&info),
                    (gint)GST_VIDEO_INFO_HEIGHT(&info), stride, NULL, NULL);
                if (pixbuf != NULL) {
                    gdouble image_width = layout.viewport.width /
                                          (layout.u1 - layout.u0);
                    gdouble image_height = layout.viewport.height /
                                           (layout.v1 - layout.v0);
                    gdouble x = layout.viewport.x - layout.u0 * image_width;
                    gdouble y = layout.viewport.y - layout.v0 * image_height;
                    cairo_save(cr);
                    cairo_rectangle(cr, layout.viewport.x, layout.viewport.y,
                                    layout.viewport.width, layout.viewport.height);
                    cairo_clip(cr);
                    cairo_translate(cr, x, y);
                    cairo_scale(cr,
                                image_width / GST_VIDEO_INFO_WIDTH(&info),
                                image_height / GST_VIDEO_INFO_HEIGHT(&info));
                    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                    g_object_unref(pixbuf);
                }
            }
        }
        gst_video_frame_unmap(&frame);
    }
    gst_sample_unref(sample);
    (void)generation;
    return TRUE;
}

static gboolean
renderer_dispatch(gpointer user_data)
{
    CaptureRenderer *renderer = user_data;
    g_mutex_lock(&renderer->sample_mutex);
    renderer->dispatch_pending = FALSE;
    gboolean closing = renderer->closing;
    if (!closing && renderer->latest_sample != NULL &&
        (renderer->latest_frame_timing.valid_mask & FRAME_TIMING_APPSINK) != 0) {
        gint64 dispatch_us = g_get_monotonic_time();
        if (dispatch_us >= renderer->latest_frame_timing.appsink_us) {
            renderer->latest_frame_timing.dispatch_us = dispatch_us;
            renderer->latest_frame_timing.valid_mask |= FRAME_TIMING_DISPATCH;
        }
    }
    g_mutex_unlock(&renderer->sample_mutex);
    if (!closing) {
        gtk_gl_area_queue_render(GTK_GL_AREA(renderer->gl_area));
        gtk_widget_queue_draw(renderer->cairo_area);
    }
    capture_renderer_unref(renderer);
    return G_SOURCE_REMOVE;
}

static GstFlowReturn
on_new_sample(GstAppSink *sink, gpointer user_data)
{
    CaptureRenderer *renderer = user_data;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == NULL)
        return GST_FLOW_EOS;
    gint64 appsink_us = g_get_monotonic_time();

    GstVideoInfo info;
    gboolean valid_caps = gst_sample_get_caps(sample) != NULL &&
        gst_video_info_from_caps(&info, gst_sample_get_caps(sample));
    if (valid_caps) {
        GstVideoFormat format = GST_VIDEO_INFO_FORMAT(&info);
        valid_caps = format == GST_VIDEO_FORMAT_RGBA ||
                     format == GST_VIDEO_FORMAT_I420;
    }
    FrameTiming sample_timing = {0};
    if (valid_caps)
        sample_timing = frame_timing_at_appsink(
            renderer, gst_sample_get_buffer(sample), appsink_us);
    GstSample *old_sample = NULL;
    gboolean schedule_dispatch = FALSE;
    g_mutex_lock(&renderer->sample_mutex);
    if (renderer->closing) {
        g_mutex_unlock(&renderer->sample_mutex);
        gst_sample_unref(sample);
        return GST_FLOW_FLUSHING;
    }
    if (valid_caps) {
        renderer->source_width = GST_VIDEO_INFO_WIDTH(&info);
        renderer->source_height = GST_VIDEO_INFO_HEIGHT(&info);
        renderer->pixel_aspect_num = GST_VIDEO_INFO_PAR_N(&info);
        renderer->pixel_aspect_den = GST_VIDEO_INFO_PAR_D(&info);
        if (renderer->pixel_aspect_num == 0 || renderer->pixel_aspect_den == 0) {
            renderer->pixel_aspect_num = 1;
            renderer->pixel_aspect_den = 1;
        }
    }
    old_sample = renderer->latest_sample;
    renderer->latest_sample = sample;
    renderer->latest_frame_timing = sample_timing;
    renderer->sample_generation++;
    if (!renderer->dispatch_pending) {
        renderer->dispatch_pending = TRUE;
        schedule_dispatch = TRUE;
        capture_renderer_ref(renderer);
    }
    g_mutex_unlock(&renderer->sample_mutex);
    if (old_sample != NULL)
        gst_sample_unref(old_sample);
    if (schedule_dispatch)
        g_main_context_invoke_full(renderer->main_context, G_PRIORITY_DEFAULT,
                                   renderer_dispatch, renderer, NULL);
    return GST_FLOW_OK;
}

static CaptureRenderer *
capture_renderer_ref(CaptureRenderer *renderer)
{
    g_atomic_int_inc(&renderer->ref_count);
    return renderer;
}

static void
capture_renderer_unref(CaptureRenderer *renderer)
{
    if (!g_atomic_int_dec_and_test(&renderer->ref_count))
        return;
    if (renderer->latest_sample != NULL)
        gst_sample_unref(renderer->latest_sample);
    if (renderer->sink != NULL)
        gst_object_unref(renderer->sink);
    if (renderer->stack != NULL)
        g_object_unref(renderer->stack);
    if (renderer->main_context != NULL)
        g_main_context_unref(renderer->main_context);
    g_free(renderer->backend_name);
    g_mutex_clear(&renderer->sample_mutex);
    g_mutex_clear(&renderer->timing_mutex);
    g_free(renderer);
}

CaptureRenderer *
capture_renderer_new(GError **error)
{
    CaptureRenderer *renderer = g_new0(CaptureRenderer, 1);
    renderer->ref_count = 1;
    renderer->main_context = g_main_context_ref_thread_default();
    g_mutex_init(&renderer->sample_mutex);
    g_mutex_init(&renderer->timing_mutex);
    renderer->scale_mode = CAPTURE_RENDERER_FIT;
    renderer->pixel_aspect_num = 1;
    renderer->pixel_aspect_den = 1;
    set_backend_name(renderer, "GtkGLArea/OpenGL (initializing)");

    renderer->stack = gtk_stack_new();
    if (renderer->stack == NULL) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Could not create renderer widget stack");
        capture_renderer_unref(renderer);
        return NULL;
    }
    g_object_ref_sink(renderer->stack);
    gtk_stack_set_transition_type(GTK_STACK(renderer->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_hexpand(renderer->stack, TRUE);
    gtk_widget_set_vexpand(renderer->stack, TRUE);

    renderer->gl_area = gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(renderer->gl_area), 3, 2);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(renderer->gl_area), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(renderer->gl_area), FALSE);
    gtk_widget_set_hexpand(renderer->gl_area, TRUE);
    gtk_widget_set_vexpand(renderer->gl_area, TRUE);
    gtk_widget_set_app_paintable(renderer->gl_area, TRUE);
    gtk_container_add(GTK_CONTAINER(renderer->stack), renderer->gl_area);
    g_signal_connect(renderer->gl_area, "realize", G_CALLBACK(on_gl_realize), renderer);
    g_signal_connect(renderer->gl_area, "unrealize", G_CALLBACK(on_gl_unrealize), renderer);
    g_signal_connect(renderer->gl_area, "render", G_CALLBACK(on_gl_render), renderer);

    renderer->cairo_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(renderer->cairo_area, TRUE);
    gtk_widget_set_vexpand(renderer->cairo_area, TRUE);
    gtk_widget_set_app_paintable(renderer->cairo_area, TRUE);
    gtk_container_add(GTK_CONTAINER(renderer->stack), renderer->cairo_area);
    g_signal_connect(renderer->cairo_area, "draw", G_CALLBACK(on_cairo_draw), renderer);
    gtk_stack_set_visible_child(GTK_STACK(renderer->stack), renderer->gl_area);
    return renderer;
}

GtkWidget *
capture_renderer_get_widget(CaptureRenderer *renderer)
{
    return renderer->stack;
}

void
capture_renderer_connect_motion_events(
    CaptureRenderer *renderer,
    gboolean (*callback)(GtkWidget *, GdkEventMotion *, gpointer),
    gpointer user_data)
{
    if (renderer == NULL || callback == NULL)
        return;
    if (renderer->gl_motion_handler_id != 0)
        g_signal_handler_disconnect(renderer->gl_area,
                                    renderer->gl_motion_handler_id);
    if (renderer->cairo_motion_handler_id != 0)
        g_signal_handler_disconnect(renderer->cairo_area,
                                    renderer->cairo_motion_handler_id);
    gtk_widget_add_events(renderer->gl_area, GDK_POINTER_MOTION_MASK);
    gtk_widget_add_events(renderer->cairo_area, GDK_POINTER_MOTION_MASK);
    renderer->gl_motion_handler_id =
        g_signal_connect(renderer->gl_area, "motion-notify-event",
                         G_CALLBACK(callback), user_data);
    renderer->cairo_motion_handler_id =
        g_signal_connect(renderer->cairo_area, "motion-notify-event",
                         G_CALLBACK(callback), user_data);
}


GstElement *
capture_renderer_create_sink(CaptureRenderer *renderer)
{
    if (renderer->sink != NULL)
        return NULL;
    GstElement *sink = gst_element_factory_make("appsink", "gtk-video-appsink");
    if (sink == NULL)
        return NULL;

    GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                        "format", G_TYPE_STRING,
                                        renderer->gl_initialized ? "I420" : "RGBA",
                                        NULL);
    g_object_set(sink,
                 "caps", caps,
                 "max-buffers", 1u,
                 "drop", TRUE,
                 "sync", FALSE,
                 "enable-last-sample", FALSE,
                 NULL);
    gst_caps_unref(caps);

    GstAppSinkCallbacks callbacks = {0};
    callbacks.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks,
                               capture_renderer_ref(renderer),
                               (GDestroyNotify)capture_renderer_unref);
    renderer->sink = gst_object_ref(sink);
    return sink;
}

void
capture_renderer_set_scale_mode(CaptureRenderer *renderer,
                                CaptureRendererScaleMode mode)
{
    if (renderer == NULL || (mode != CAPTURE_RENDERER_FIT &&
                             mode != CAPTURE_RENDERER_FILL))
        return;
    g_mutex_lock(&renderer->sample_mutex);
    renderer->scale_mode = mode;
    g_mutex_unlock(&renderer->sample_mutex);
    gtk_gl_area_queue_render(GTK_GL_AREA(renderer->gl_area));
    gtk_widget_queue_draw(renderer->cairo_area);
}

void
capture_renderer_pipeline_stopped(CaptureRenderer *renderer)
{
    if (renderer == NULL)
        return;
    GstElement *sink = renderer->sink;
    renderer->sink = NULL;
    if (sink != NULL) {
        GstAppSinkCallbacks callbacks = {0};
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, NULL, NULL);
        gst_object_unref(sink);
    }
    g_mutex_lock(&renderer->sample_mutex);
    GstSample *sample = renderer->latest_sample;
    renderer->latest_sample = NULL;
    renderer->source_width = 0;
    renderer->source_height = 0;
    renderer->pixel_aspect_num = 1;
    renderer->pixel_aspect_den = 1;
    renderer->latest_frame_timing = (FrameTiming){0};
    renderer->sample_generation++;
    renderer->last_timed_generation = renderer->sample_generation;
    g_mutex_unlock(&renderer->sample_mutex);
    if (sample != NULL)
        gst_sample_unref(sample);
    reset_renderer_timing(renderer);
    gtk_gl_area_queue_render(GTK_GL_AREA(renderer->gl_area));
    gtk_widget_queue_draw(renderer->cairo_area);
}

const gchar *
capture_renderer_get_backend_name(const CaptureRenderer *renderer)
{
    return renderer != NULL ? renderer->backend_name : "unavailable";
}

void
capture_renderer_free(CaptureRenderer *renderer)
{
    if (renderer == NULL)
        return;
    g_mutex_lock(&renderer->sample_mutex);
    renderer->closing = TRUE;
    g_mutex_unlock(&renderer->sample_mutex);
    if (renderer->gl_motion_handler_id != 0)
        g_signal_handler_disconnect(renderer->gl_area,
                                    renderer->gl_motion_handler_id);
    if (renderer->cairo_motion_handler_id != 0)
        g_signal_handler_disconnect(renderer->cairo_area,
                                    renderer->cairo_motion_handler_id);
    renderer->gl_motion_handler_id = 0;
    renderer->cairo_motion_handler_id = 0;
    g_signal_handlers_disconnect_by_data(renderer->gl_area, renderer);
    g_signal_handlers_disconnect_by_data(renderer->cairo_area, renderer);
    if (renderer->gl_initialized && gtk_widget_get_realized(renderer->gl_area))
        on_gl_unrealize(GTK_GL_AREA(renderer->gl_area), renderer);
    capture_renderer_unref(renderer);
}

static void
reset_renderer_timing(CaptureRenderer *renderer)
{
    g_mutex_lock(&renderer->timing_mutex);
    memset(renderer->source_timing, 0, sizeof(renderer->source_timing));
    memset(renderer->timing_metrics, 0, sizeof(renderer->timing_metrics));
    renderer->source_timing_next = 0;
    g_mutex_unlock(&renderer->timing_mutex);
}

static gboolean
timing_delta_us(guint valid_mask, guint start_flag, gint64 start_us,
                guint end_flag, gint64 end_us, gint64 *duration_us)
{
    if ((valid_mask & start_flag) == 0 || (valid_mask & end_flag) == 0 ||
        end_us < start_us)
        return FALSE;
    *duration_us = end_us - start_us;
    return TRUE;
}

static void
timing_metric_add_locked(CaptureRenderer *renderer, TimingMetric metric,
                         gint64 duration_us)
{
    TimingMetricSamples *samples = &renderer->timing_metrics[metric];
    if (samples->sample_count == TIMING_SAMPLE_WINDOW)
        samples->sum_us -= samples->samples_us[samples->next_sample];
    else
        samples->sample_count++;
    samples->samples_us[samples->next_sample] = duration_us;
    samples->sum_us += duration_us;
    samples->next_sample = (samples->next_sample + 1) % TIMING_SAMPLE_WINDOW;
}

static void
record_frame_timing(CaptureRenderer *renderer, const FrameTiming *timing)
{
    gint64 duration_us;
    g_mutex_lock(&renderer->timing_mutex);
    if (timing_delta_us(timing->valid_mask, FRAME_TIMING_SOURCE,
                        timing->source_us, FRAME_TIMING_DECODED,
                        timing->decoded_us, &duration_us))
        timing_metric_add_locked(renderer, TIMING_SOURCE_TO_DECODED,
                                 duration_us);
    if (timing_delta_us(timing->valid_mask, FRAME_TIMING_DECODED,
                        timing->decoded_us, FRAME_TIMING_APPSINK,
                        timing->appsink_us, &duration_us))
        timing_metric_add_locked(renderer, TIMING_DECODED_TO_APPSINK,
                                 duration_us);
    if (timing_delta_us(timing->valid_mask, FRAME_TIMING_APPSINK,
                        timing->appsink_us, FRAME_TIMING_DISPATCH,
                        timing->dispatch_us, &duration_us))
        timing_metric_add_locked(renderer, TIMING_APPSINK_TO_DISPATCH,
                                 duration_us);
    if (timing_delta_us(timing->valid_mask, FRAME_TIMING_DISPATCH,
                        timing->dispatch_us, FRAME_TIMING_GL,
                        timing->gl_us, &duration_us))
        timing_metric_add_locked(renderer, TIMING_DISPATCH_TO_GL, duration_us);
    if (timing_delta_us(timing->valid_mask, FRAME_TIMING_APPSINK,
                        timing->appsink_us, FRAME_TIMING_GL,
                        timing->gl_us, &duration_us))
        timing_metric_add_locked(renderer, TIMING_APPSINK_TO_GL, duration_us);
    if (timing_delta_us(timing->valid_mask, FRAME_TIMING_SOURCE,
                        timing->source_us, FRAME_TIMING_GL,
                        timing->gl_us, &duration_us))
        timing_metric_add_locked(renderer, TIMING_SOURCE_TO_GL, duration_us);
    g_mutex_unlock(&renderer->timing_mutex);
}

static FrameTiming
frame_timing_at_appsink(CaptureRenderer *renderer, GstBuffer *buffer,
                        gint64 appsink_us)
{
    FrameTiming timing = {
        .valid_mask = FRAME_TIMING_APPSINK,
        .appsink_us = appsink_us,
    };
    if (buffer == NULL)
        return timing;
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return timing;

    g_mutex_lock(&renderer->timing_mutex);
    SourceFrameTiming *match = NULL;
    guint matches = 0;
    for (guint i = 0; i < SOURCE_TIMING_SLOTS; i++) {
        SourceFrameTiming *candidate = &renderer->source_timing[i];
        if (candidate->valid && candidate->pts == pts) {
            match = candidate;
            matches++;
        }
    }
    if (matches == 1 && !match->ambiguous && match->decoded &&
        match->source_us <= match->decoded_us &&
        match->decoded_us <= appsink_us) {
        timing.source_us = match->source_us;
        timing.decoded_us = match->decoded_us;
        timing.valid_mask |= FRAME_TIMING_SOURCE | FRAME_TIMING_DECODED;
        match->valid = FALSE;
    }
    g_mutex_unlock(&renderer->timing_mutex);
    return timing;
}

void
capture_renderer_record_source_buffer(CaptureRenderer *renderer,
                                      GstBuffer *buffer)
{
    if (renderer == NULL || buffer == NULL)
        return;
    gint64 source_us = g_get_monotonic_time();
    GstClockTime pts = GST_BUFFER_PTS(buffer);

    g_mutex_lock(&renderer->timing_mutex);
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT))
        memset(renderer->source_timing, 0, sizeof(renderer->source_timing));
    if (!GST_CLOCK_TIME_IS_VALID(pts)) {
        g_mutex_unlock(&renderer->timing_mutex);
        return;
    }
    gboolean ambiguous = FALSE;
    for (guint i = 0; i < SOURCE_TIMING_SLOTS; i++) {
        SourceFrameTiming *candidate = &renderer->source_timing[i];
        if (candidate->valid && candidate->pts == pts) {
            candidate->ambiguous = TRUE;
            ambiguous = TRUE;
        }
    }
    SourceFrameTiming *entry =
        &renderer->source_timing[renderer->source_timing_next];
    renderer->source_timing_next =
        (renderer->source_timing_next + 1) % SOURCE_TIMING_SLOTS;
    *entry = (SourceFrameTiming){
        .valid = TRUE,
        .ambiguous = ambiguous,
        .pts = pts,
        .source_us = source_us,
    };
    g_mutex_unlock(&renderer->timing_mutex);
}

void
capture_renderer_record_decoded_buffer(CaptureRenderer *renderer,
                                       GstBuffer *buffer)
{
    if (renderer == NULL || buffer == NULL)
        return;
    gint64 decoded_us = g_get_monotonic_time();
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return;

    g_mutex_lock(&renderer->timing_mutex);
    SourceFrameTiming *match = NULL;
    guint matches = 0;
    for (guint i = 0; i < SOURCE_TIMING_SLOTS; i++) {
        SourceFrameTiming *candidate = &renderer->source_timing[i];
        if (candidate->valid && candidate->pts == pts) {
            match = candidate;
            matches++;
        }
    }
    if (matches == 1 && !match->ambiguous && !match->decoded) {
        match->decoded_us = decoded_us;
        match->decoded = TRUE;
    } else if (matches > 1) {
        for (guint i = 0; i < SOURCE_TIMING_SLOTS; i++) {
            SourceFrameTiming *candidate = &renderer->source_timing[i];
            if (candidate->valid && candidate->pts == pts)
                candidate->ambiguous = TRUE;
        }
    }
    g_mutex_unlock(&renderer->timing_mutex);
}

static gint
compare_timing_sample(const void *first, const void *second)
{
    gint64 left = *(const gint64 *)first;
    gint64 right = *(const gint64 *)second;
    return (left > right) - (left < right);
}

CaptureRendererTimingStats
capture_renderer_get_timing_stats(CaptureRenderer *renderer)
{
    CaptureRendererTimingStats result = {0};
    if (renderer == NULL)
        return result;

    gint64 ordered[TIMING_METRIC_COUNT][TIMING_SAMPLE_WINDOW];
    guint sample_counts[TIMING_METRIC_COUNT];
    gint64 sums_us[TIMING_METRIC_COUNT];
    CaptureRendererTimingSummary *summaries[TIMING_METRIC_COUNT] = {
        &result.source_to_decoded,
        &result.decoded_to_appsink,
        &result.appsink_to_dispatch,
        &result.dispatch_to_gl,
        &result.appsink_to_gl,
        &result.source_to_gl,
    };

    g_mutex_lock(&renderer->timing_mutex);
    for (guint i = 0; i < TIMING_METRIC_COUNT; i++) {
        const TimingMetricSamples *samples = &renderer->timing_metrics[i];
        sample_counts[i] = samples->sample_count;
        sums_us[i] = samples->sum_us;
        if (sample_counts[i] > 0)
            memcpy(ordered[i], samples->samples_us,
                   sample_counts[i] * sizeof(ordered[i][0]));
    }
    g_mutex_unlock(&renderer->timing_mutex);

    for (guint i = 0; i < TIMING_METRIC_COUNT; i++) {
        guint count = sample_counts[i];
        if (count == 0)
            continue;
        qsort(ordered[i], count, sizeof(ordered[i][0]),
              compare_timing_sample);
        guint p50_rank = (count * 50 + 99) / 100;
        guint p95_rank = (count * 95 + 99) / 100;
        summaries[i]->sample_count = count;
        summaries[i]->average_ms = (gdouble)sums_us[i] / count / 1000.0;
        summaries[i]->p50_ms = (gdouble)ordered[i][p50_rank - 1] / 1000.0;
        summaries[i]->p95_ms = (gdouble)ordered[i][p95_rank - 1] / 1000.0;
    }
    return result;
}
