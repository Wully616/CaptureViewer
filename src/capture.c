#define _GNU_SOURCE
#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <dirent.h>

#define HAGIBIS_VENDOR 0x345f
#define HAGIBIS_PRODUCT 0x2130
#define MAX_SIZE_EXPANSION 10000
#define MAX_INTERVAL_EXPANSION 100000

static gchar *
read_sysfs_attribute(const gchar *base, const gchar *name)
{
    gchar *path = g_build_filename(base, name, NULL);
    gchar *value = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &value, &length, NULL)) {
        g_free(path);
        return NULL;
    }
    g_free(path);
    g_strstrip(value);
    return value;
}

static gboolean
read_usb_ids(const gchar *start, gchar **usb_path, gchar **manufacturer,
             gchar **product, gchar **serial)
{
    gchar *path = g_strdup(start);
    gboolean found = FALSE;

    while (path != NULL) {
        gchar *vendor = read_sysfs_attribute(path, "idVendor");
        gchar *product_id = read_sysfs_attribute(path, "idProduct");
        if (vendor != NULL && product_id != NULL &&
            g_ascii_strtoull(vendor, NULL, 16) == HAGIBIS_VENDOR &&
            g_ascii_strtoull(product_id, NULL, 16) == HAGIBIS_PRODUCT) {
            if (usb_path != NULL)
                *usb_path = g_strdup(path);
            if (manufacturer != NULL)
                *manufacturer = read_sysfs_attribute(path, "manufacturer");
            if (product != NULL)
                *product = read_sysfs_attribute(path, "product");
            if (serial != NULL)
                *serial = read_sysfs_attribute(path, "serial");
            found = TRUE;
        }
        g_free(vendor);
        g_free(product_id);
        if (found)
            break;

        gchar *parent = g_path_get_dirname(path);
        if (g_str_equal(parent, path)) {
            g_free(parent);
            break;
        }
        g_free(path);
        path = parent;
    }
    g_free(path);
    return found;
}

gboolean
capture_hagibis_usb_detected(void)
{
    DIR *dir = opendir("/sys/bus/usb/devices");
    if (dir == NULL)
        return FALSE;

    gboolean detected = FALSE;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        gchar *path = g_build_filename("/sys/bus/usb/devices", entry->d_name, NULL);
        gchar *vendor = read_sysfs_attribute(path, "idVendor");
        gchar *product = read_sysfs_attribute(path, "idProduct");
        detected = vendor != NULL && product != NULL &&
            g_ascii_strtoull(vendor, NULL, 16) == HAGIBIS_VENDOR &&
            g_ascii_strtoull(product, NULL, 16) == HAGIBIS_PRODUCT;
        g_free(vendor);
        g_free(product);
        g_free(path);
        if (detected)
            break;
    }
    closedir(dir);
    return detected;
}

static gboolean
query_capture_type(const gchar *path, enum v4l2_buf_type *type)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return FALSE;

    struct v4l2_capability cap = {0};
    gboolean found = FALSE;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        guint32 caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
                     ? cap.device_caps : cap.capabilities;
        if (caps & V4L2_CAP_VIDEO_CAPTURE) {
            *type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            found = TRUE;
        } else if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
            *type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            found = TRUE;
        }
    }
    close(fd);
    return found;
}

CaptureDevice *
capture_device_find_hagibis(GError **error)
{
    DIR *dir = opendir("/sys/class/video4linux");
    if (dir == NULL) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot scan /sys/class/video4linux: %s", g_strerror(errno));
        return NULL;
    }

    CaptureDevice *result = NULL;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!g_str_has_prefix(entry->d_name, "video") ||
            !g_ascii_isdigit(entry->d_name[5]))
            continue;

        gchar *class_path = g_build_filename("/sys/class/video4linux", entry->d_name,
                                             "device", NULL);
        gchar resolved[PATH_MAX];
        if (realpath(class_path, resolved) == NULL) {
            g_free(class_path);
            continue;
        }
        g_free(class_path);

        gchar *usb_path = NULL;
        gchar *manufacturer = NULL;
        gchar *product = NULL;
        gchar *serial = NULL;
        if (!read_usb_ids(resolved, &usb_path, &manufacturer, &product, &serial))
            continue;

        gchar *dev_path = g_build_filename("/dev", entry->d_name, NULL);
        enum v4l2_buf_type type;
        if (!query_capture_type(dev_path, &type)) {
            g_free(usb_path);
            g_free(manufacturer);
            g_free(product);
            g_free(serial);
            g_free(dev_path);
            continue;
        }

        int fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        struct v4l2_capability cap = {0};
        if (fd < 0 || ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
            if (fd >= 0)
                close(fd);
            g_free(usb_path);
            g_free(manufacturer);
            g_free(product);
            g_free(serial);
            g_free(dev_path);
            continue;
        }
        close(fd);

        result = g_new0(CaptureDevice, 1);
        result->path = dev_path;
        result->interface_sysfs = g_strdup(resolved);
        result->usb_sysfs = usb_path;
        result->manufacturer = manufacturer;
        result->product = product;
        result->serial = serial;
        result->card_name = g_strdup((const gchar *)cap.card);
        result->buffer_type = type;
        break;
    }
    closedir(dir);

    if (result == NULL)
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                    "Hagibis USB capture device %04x:%04x is not present",
                    HAGIBIS_VENDOR, HAGIBIS_PRODUCT);
    return result;
}

void
capture_device_free(CaptureDevice *device)
{
    if (device == NULL)
        return;
    g_free(device->path);
    g_free(device->interface_sysfs);
    g_free(device->usb_sysfs);
    g_free(device->manufacturer);
    g_free(device->product);
    g_free(device->serial);
    g_free(device->card_name);
    g_free(device);
}

void
capture_mode_free(CaptureMode *mode)
{
    if (mode == NULL)
        return;
    g_free(mode->format_name);
    g_free(mode->label);
    g_free(mode);
}

static guint64
gcd_u64(guint64 a, guint64 b)
{
    while (b != 0) {
        guint64 t = a % b;
        a = b;
        b = t;
    }
    return a == 0 ? 1 : a;
}

static gchar *
fourcc_name(guint32 fourcc)
{
    switch (fourcc) {
    case V4L2_PIX_FMT_MJPEG: return g_strdup("MJPEG");
    case V4L2_PIX_FMT_JPEG: return g_strdup("JPEG");
    case V4L2_PIX_FMT_YUYV: return g_strdup("YUYV");
    case V4L2_PIX_FMT_UYVY: return g_strdup("UYVY");
    case V4L2_PIX_FMT_YVYU: return g_strdup("YVYU");
    case V4L2_PIX_FMT_NV12: return g_strdup("NV12");
    case V4L2_PIX_FMT_NV21: return g_strdup("NV21");
    case V4L2_PIX_FMT_RGB24: return g_strdup("RGB24");
    case V4L2_PIX_FMT_BGR24: return g_strdup("BGR24");
    case V4L2_PIX_FMT_GREY: return g_strdup("GREY");
    case V4L2_PIX_FMT_H264: return g_strdup("H.264");
#ifdef V4L2_PIX_FMT_HEVC
    case V4L2_PIX_FMT_HEVC: return g_strdup("HEVC");
#endif
#ifdef V4L2_PIX_FMT_HEVC_SLICE
    case V4L2_PIX_FMT_HEVC_SLICE: return g_strdup("HEVC slice");
#endif
#ifdef V4L2_PIX_FMT_VP9
    case V4L2_PIX_FMT_VP9: return g_strdup("VP9");
#endif
    default: {
        gchar text[5] = {
            (gchar)(fourcc & 0xff), (gchar)((fourcc >> 8) & 0xff),
            (gchar)((fourcc >> 16) & 0xff), (gchar)((fourcc >> 24) & 0xff), '\0'
        };
        for (guint i = 0; i < 4; i++)
            if (!g_ascii_isprint(text[i]))
                text[i] = '?';
        return g_strdup(text);
    }
    }
}

static void
mode_add(GPtrArray *modes, guint32 fourcc, guint width, guint height,
         guint fps_n, guint fps_d, gboolean compressed)
{
    if (width == 0 || height == 0 || fps_n == 0 || fps_d == 0)
        return;

    guint64 divisor = gcd_u64(fps_n, fps_d);
    fps_n /= (guint)divisor;
    fps_d /= (guint)divisor;
    for (guint i = 0; i < modes->len; i++) {
        CaptureMode *old = g_ptr_array_index(modes, i);
        if (old->fourcc == fourcc && old->width == width && old->height == height &&
            old->fps_n == fps_n && old->fps_d == fps_d)
            return;
    }

    CaptureMode *mode = g_new0(CaptureMode, 1);
    mode->fourcc = fourcc;
    mode->width = width;
    mode->height = height;
    mode->fps_n = fps_n;
    mode->fps_d = fps_d;
    mode->compressed = compressed;
    mode->format_name = fourcc_name(fourcc);
    gdouble fps = (gdouble)fps_n / fps_d;
    mode->label = g_strdup_printf("%s · %u×%u · %.3g Hz",
                                  mode->format_name, width, height, fps);
    g_ptr_array_add(modes, mode);
}

static gint
compare_modes(gconstpointer a, gconstpointer b)
{
    const CaptureMode *left = *(CaptureMode * const *)a;
    const CaptureMode *right = *(CaptureMode * const *)b;
    guint64 left_pixels = (guint64)left->width * left->height;
    guint64 right_pixels = (guint64)right->width * right->height;
    if (left_pixels != right_pixels)
        return left_pixels > right_pixels ? -1 : 1;
    if (left->width != right->width)
        return left->width > right->width ? -1 : 1;
    if (left->height != right->height)
        return left->height > right->height ? -1 : 1;
    guint64 left_rate = (guint64)left->fps_n * right->fps_d;
    guint64 right_rate = (guint64)right->fps_n * left->fps_d;
    if (left_rate != right_rate)
        return left_rate > right_rate ? -1 : 1;
    if (left->compressed != right->compressed)
        return left->compressed ? -1 : 1;
    if (left->fourcc != right->fourcc)
        return left->fourcc < right->fourcc ? -1 : 1;
    return 0;
}

static gint
compare_fract(const struct v4l2_fract *a, const struct v4l2_fract *b)
{
    __uint128_t left = (__uint128_t)a->numerator * b->denominator;
    __uint128_t right = (__uint128_t)b->numerator * a->denominator;
    return left < right ? -1 : left > right ? 1 : 0;
}

static __uint128_t
gcd_u128(__uint128_t a, __uint128_t b)
{
    while (b != 0) {
        __uint128_t t = a % b;
        a = b;
        b = t;
    }
    return a == 0 ? 1 : a;
}

static gboolean
fract_at_step(const struct v4l2_fract *base, const struct v4l2_fract *step,
              guint64 index, struct v4l2_fract *result)
{
    if (base->denominator == 0 || step->denominator == 0)
        return FALSE;
    __uint128_t numerator = (__uint128_t)base->numerator * step->denominator +
        (__uint128_t)index * step->numerator * base->denominator;
    __uint128_t denominator = (__uint128_t)base->denominator * step->denominator;
    __uint128_t divisor = gcd_u128(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    if (numerator > G_MAXUINT32 || denominator > G_MAXUINT32)
        return FALSE;
    result->numerator = (guint32)numerator;
    result->denominator = (guint32)denominator;
    return TRUE;
}

static gboolean
fract_is_step(const struct v4l2_fract *value, const struct v4l2_fract *minimum,
              const struct v4l2_fract *step)
{
    if (step->numerator == 0 || value->denominator == 0 ||
        minimum->denominator == 0 || step->denominator == 0)
        return FALSE;
    __int128 delta = (__int128)value->numerator * minimum->denominator -
                     (__int128)minimum->numerator * value->denominator;
    if (delta < 0)
        return FALSE;
    __int128 numerator = delta * step->denominator;
    __int128 denominator = (__int128)value->denominator * minimum->denominator *
                           step->numerator;
    return denominator > 0 && numerator % denominator == 0;
}

static void
add_interval(GPtrArray *modes, guint32 fourcc, guint width, guint height,
             gboolean compressed, const struct v4l2_fract *interval)
{
    if (interval->numerator == 0 || interval->denominator == 0)
        return;
    mode_add(modes, fourcc, width, height, interval->denominator,
             interval->numerator, compressed);
}

static gboolean
interval_in_range(const struct v4l2_fract *value,
                  const struct v4l2_fract *minimum,
                  const struct v4l2_fract *maximum)
{
    return compare_fract(value, minimum) >= 0 && compare_fract(value, maximum) <= 0;
}

static void
enumerate_intervals(int fd, enum v4l2_buf_type type, guint32 fourcc,
                    guint width, guint height, gboolean compressed,
                    GPtrArray *modes)
{
    struct v4l2_frmivalenum interval = {0};
    interval.pixel_format = fourcc;
    interval.width = width;
    interval.height = height;
    interval.type = type;
    gboolean found = FALSE;

    for (guint index = 0; index < 4096; index++) {
        interval.index = index;
        if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) < 0) {
            if (errno != EINVAL && !found)
                g_warning("VIDIOC_ENUM_FRAMEINTERVALS %ux%u failed: %s",
                          width, height, g_strerror(errno));
            break;
        }
        found = TRUE;
        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            add_interval(modes, fourcc, width, height, compressed,
                         &interval.discrete);
            continue;
        }

        const struct v4l2_fract *minimum;
        const struct v4l2_fract *maximum;
        const struct v4l2_fract *step = NULL;
        if (interval.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
            minimum = &interval.stepwise.min;
            maximum = &interval.stepwise.max;
            step = &interval.stepwise.step;
        } else if (interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
            minimum = &interval.stepwise.min;
            maximum = &interval.stepwise.max;
        } else {
            g_warning("Unknown V4L2 frame-interval type %u", interval.type);
            return;
        }

        add_interval(modes, fourcc, width, height, compressed, minimum);
        add_interval(modes, fourcc, width, height, compressed, maximum);
        if (step != NULL && step->numerator != 0 && step->denominator != 0) {
            gboolean exhausted = FALSE;
            for (guint64 k = 1; k < MAX_INTERVAL_EXPANSION; k++) {
                struct v4l2_fract current;
                if (!fract_at_step(minimum, step, k, &current))
                    break;
                if (compare_fract(&current, maximum) > 0) {
                    exhausted = TRUE;
                    break;
                }
                add_interval(modes, fourcc, width, height, compressed, &current);
            }
            if (!exhausted)
                g_message("Frame interval range for %ux%u exceeds explicit expansion limit; adding valid standard rates",
                          width, height);
        } else {
            g_message("Continuous frame interval range for %ux%u; adding valid standard rates and endpoints",
                      width, height);
        }

        static const guint rates[] = {240, 200, 144, 120, 100, 90, 75, 72,
            60, 59, 50, 48, 30, 29, 25, 24, 20, 15, 12, 10, 5, 1};
        for (guint i = 0; i < G_N_ELEMENTS(rates); i++) {
            struct v4l2_fract candidate = { .numerator = 1, .denominator = rates[i] };
            if (interval_in_range(&candidate, minimum, maximum) &&
                (step == NULL || fract_is_step(&candidate, minimum, step)))
                add_interval(modes, fourcc, width, height, compressed, &candidate);
        }
        return;
    }
}

typedef struct { guint width, height; } FrameSize;

static void
size_add(GArray *sizes, guint width, guint height)
{
    if (width == 0 || height == 0)
        return;
    for (guint i = 0; i < sizes->len; i++) {
        FrameSize *old = &g_array_index(sizes, FrameSize, i);
        if (old->width == width && old->height == height)
            return;
    }
    FrameSize size = { width, height };
    g_array_append_val(sizes, size);
}

static gboolean
size_on_step(guint value, guint minimum, guint step)
{
    return value >= minimum && step != 0 && (value - minimum) % step == 0;
}

static void
enumerate_frame_sizes(int fd, enum v4l2_buf_type type, guint32 fourcc,
                      gboolean compressed, GPtrArray *modes)
{
    GArray *sizes = g_array_new(FALSE, FALSE, sizeof(FrameSize));
    struct v4l2_frmsizeenum size = {0};
    size.pixel_format = fourcc;
    size.type = type;
    gboolean found = FALSE;

    for (guint index = 0; index < 4096; index++) {
        size.index = index;
        if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) {
            if (errno != EINVAL && !found)
                g_warning("VIDIOC_ENUM_FRAMESIZES failed: %s", g_strerror(errno));
            break;
        }
        found = TRUE;
        if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            size_add(sizes, size.discrete.width, size.discrete.height);
            continue;
        }

        guint min_w, max_w, step_w, min_h, max_h, step_h;
        if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
            min_w = size.stepwise.min_width; max_w = size.stepwise.max_width;
            step_w = size.stepwise.step_width; min_h = size.stepwise.min_height;
            max_h = size.stepwise.max_height; step_h = size.stepwise.step_height;
        } else if (size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
            min_w = size.stepwise.min_width; max_w = size.stepwise.max_width;
            min_h = size.stepwise.min_height; max_h = size.stepwise.max_height;
            step_w = step_h = 0;
        } else {
            g_warning("Unknown V4L2 frame-size type %u", size.type);
            continue;
        }

        guint64 nw = step_w ? ((guint64)max_w - min_w) / step_w + 1 : 2;
        guint64 nh = step_h ? ((guint64)max_h - min_h) / step_h + 1 : 2;
        if (nw * nh <= MAX_SIZE_EXPANSION && step_w != 0 && step_h != 0) {
            for (guint64 x = 0; x < nw; x++)
                for (guint64 y = 0; y < nh; y++)
                    size_add(sizes, min_w + (guint)(x * step_w),
                             min_h + (guint)(y * step_h));
        } else {
            size_add(sizes, min_w, min_h);
            size_add(sizes, max_w, max_h);
            static const FrameSize common[] = {
                {3840,2160}, {2560,1440}, {1920,1080}, {1600,1200},
                {1360,768}, {1280,1024}, {1280,960}, {1280,720},
                {1024,768}, {800,600}, {720,576}, {720,480}, {640,480}
            };
            for (guint i = 0; i < G_N_ELEMENTS(common); i++) {
                guint w = common[i].width, h = common[i].height;
                gboolean inside = w >= min_w && w <= max_w && h >= min_h && h <= max_h;
                gboolean aligned = size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ||
                    (size_on_step(w, min_w, step_w) && size_on_step(h, min_h, step_h));
                if (inside && aligned)
                    size_add(sizes, w, h);
            }
            g_message("Frame-size range for FourCC %.4s is represented by endpoints and valid standard sizes",
                      (const gchar *)&fourcc);
        }
    }

    for (guint i = 0; i < sizes->len; i++) {
        FrameSize current = g_array_index(sizes, FrameSize, i);
        enumerate_intervals(fd, type, fourcc, current.width, current.height,
                            compressed, modes);
    }
    g_array_unref(sizes);
}

GPtrArray *
capture_modes_enumerate(const CaptureDevice *device, GError **error)
{
    int fd = open(device->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot open %s: %s", device->path, g_strerror(errno));
        return NULL;
    }

    GPtrArray *modes = g_ptr_array_new_with_free_func((GDestroyNotify)capture_mode_free);
    struct v4l2_fmtdesc format = {0};
    format.type = device->buffer_type;
    gboolean found = FALSE;
    for (guint index = 0; index < 256; index++) {
        format.index = index;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &format) < 0) {
            if (errno != EINVAL && !found)
                g_warning("VIDIOC_ENUM_FMT on %s failed: %s", device->path,
                          g_strerror(errno));
            break;
        }
        found = TRUE;
        gboolean compressed = (format.flags & V4L2_FMT_FLAG_COMPRESSED) != 0;
        enumerate_frame_sizes(fd, device->buffer_type, format.pixelformat,
                              compressed, modes);
    }
    close(fd);

    if (modes->len == 0) {
        g_ptr_array_unref(modes);
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "No usable V4L2 capture modes were enumerated from %s", device->path);
        return NULL;
    }
    g_ptr_array_sort(modes, compare_modes);
    return modes;
}

gchar *
capture_mode_key(const CaptureMode *mode)
{
    return g_strdup_printf("%08x:%u:%u:%u:%u", mode->fourcc, mode->width,
                           mode->height, mode->fps_n, mode->fps_d);
}

gboolean
capture_mode_equal_key(const CaptureMode *mode, const gchar *key)
{
    if (mode == NULL || key == NULL)
        return FALSE;
    gchar *mode_key = capture_mode_key(mode);
    gboolean equal = g_str_equal(mode_key, key);
    g_free(mode_key);
    return equal;
}
