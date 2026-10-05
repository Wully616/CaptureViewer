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
parse_usb_id(const gchar *value, guint16 *id)
{
    if (value == NULL || id == NULL || *value == '\0')
        return FALSE;
    gchar *end = NULL;
    guint64 parsed = g_ascii_strtoull(value, &end, 16);
    if (end == value || *end != '\0' || parsed > G_MAXUINT16)
        return FALSE;
    *id = (guint16)parsed;
    return TRUE;
}

static gboolean
find_usb_parent(const gchar *start, gchar **usb_sysfs, guint16 *vendor_id,
                guint16 *product_id, gchar **manufacturer, gchar **product,
                gchar **serial)
{
    gchar *path = g_strdup(start);
    while (path != NULL) {
        gchar *vendor = read_sysfs_attribute(path, "idVendor");
        gchar *product_value = read_sysfs_attribute(path, "idProduct");
        guint16 parsed_vendor = 0;
        guint16 parsed_product = 0;
        gboolean found = parse_usb_id(vendor, &parsed_vendor) &&
                         parse_usb_id(product_value, &parsed_product);
        g_free(vendor);
        g_free(product_value);
        if (found) {
            *usb_sysfs = g_strdup(path);
            *vendor_id = parsed_vendor;
            *product_id = parsed_product;
            *manufacturer = read_sysfs_attribute(path, "manufacturer");
            *product = read_sysfs_attribute(path, "product");
            *serial = read_sysfs_attribute(path, "serial");
            if (*manufacturer != NULL && **manufacturer == '\0')
                g_clear_pointer(manufacturer, g_free);
            if (*product != NULL && **product == '\0')
                g_clear_pointer(product, g_free);
            if (*serial != NULL && **serial == '\0')
                g_clear_pointer(serial, g_free);
            g_free(path);
            return TRUE;
        }

        gchar *parent = g_path_get_dirname(path);
        if (g_str_equal(parent, path)) {
            g_free(parent);
            break;
        }
        g_free(path);
        path = parent;
    }
    g_free(path);
    return FALSE;
}

static gboolean
video_class_entry(const gchar *name)
{
    if (!g_str_has_prefix(name, "video") || name[5] == '\0')
        return FALSE;
    for (const gchar *digit = name + 5; *digit != '\0'; digit++)
        if (!g_ascii_isdigit(*digit))
            return FALSE;
    return TRUE;
}

static CaptureVideoNode *
query_video_node(const gchar *path, const gchar *interface_sysfs)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g_warning("Skipping V4L2 node %s: cannot open: %s",
                  path, g_strerror(errno));
        return NULL;
    }

    struct v4l2_capability cap = {0};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
        g_warning("Skipping V4L2 node %s: VIDIOC_QUERYCAP failed: %s",
                  path, g_strerror(errno));
        close(fd);
        return NULL;
    }
    close(fd);

    guint32 capabilities = cap.capabilities & V4L2_CAP_DEVICE_CAPS
        ? cap.device_caps : cap.capabilities;
    if (!(capabilities & V4L2_CAP_STREAMING) ||
        (capabilities & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)))
        return NULL;

    enum v4l2_buf_type buffer_type;
    if (capabilities & V4L2_CAP_VIDEO_CAPTURE)
        buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    else if (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    else
        return NULL;

    CaptureVideoNode *node = g_new0(CaptureVideoNode, 1);
    node->path = g_strdup(path);
    node->interface_sysfs = g_strdup(interface_sysfs);
    node->card_name = g_strndup((const gchar *)cap.card, sizeof(cap.card));
    node->driver = g_strndup((const gchar *)cap.driver, sizeof(cap.driver));
    node->bus_info = g_strndup((const gchar *)cap.bus_info, sizeof(cap.bus_info));
    node->capabilities = capabilities;
    node->buffer_type = buffer_type;
    return node;
}

void
capture_video_node_free(CaptureVideoNode *node)
{
    if (node == NULL)
        return;
    g_free(node->path);
    g_free(node->interface_sysfs);
    g_free(node->card_name);
    g_free(node->driver);
    g_free(node->bus_info);
    g_free(node);
}

static CaptureDevice *
find_device_by_parent(GPtrArray *devices, const gchar *physical_sysfs)
{
    for (guint i = 0; i < devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(devices, i);
        if (g_strcmp0(device->physical_sysfs, physical_sysfs) == 0)
            return device;
    }
    return NULL;
}

static gchar *
device_display_name_base(const CaptureDevice *device)
{
    if (device->manufacturer != NULL && device->product != NULL)
        return g_strdup_printf("%s %s", device->manufacturer, device->product);
    if (device->product != NULL)
        return g_strdup(device->product);
    if (device->nodes->len != 0) {
        const CaptureVideoNode *node = g_ptr_array_index(device->nodes, 0);
        if (node->card_name != NULL && *node->card_name != '\0')
            return g_strdup(node->card_name);
    }
    return g_strdup("Video capture device");
}

gchar *
capture_device_stable_id(GPtrArray *devices, const CaptureDevice *device)
{
    if (device == NULL)
        return NULL;

    if (device->usb_sysfs != NULL) {
        if (device->serial == NULL)
            return g_strdup_printf("usb:%04x:%04x:sysfs:%s",
                device->usb_vid, device->usb_pid, device->usb_sysfs);

        gchar *stable_id = g_strdup_printf("usb:%04x:%04x:serial:%s",
            device->usb_vid, device->usb_pid, device->serial);
        for (guint i = 0; devices != NULL && i < devices->len; i++) {
            const CaptureDevice *other = g_ptr_array_index(devices, i);
            if (other == device || other->usb_sysfs == NULL ||
                other->serial == NULL || other->usb_vid != device->usb_vid ||
                other->usb_pid != device->usb_pid ||
                g_strcmp0(other->serial, device->serial) != 0 ||
                g_strcmp0(other->usb_sysfs, device->usb_sysfs) == 0)
                continue;
            gchar *disambiguated = g_strdup_printf("%s:sysfs:%s",
                stable_id, device->usb_sysfs);
            g_free(stable_id);
            return disambiguated;
        }
        return stable_id;
    }

    const CaptureVideoNode *node = device->nodes != NULL && device->nodes->len != 0
        ? g_ptr_array_index(device->nodes, 0) : NULL;
    return g_strdup_printf("sysfs:%s:%s:%s",
        device->physical_sysfs != NULL ? device->physical_sysfs : "",
        node != NULL && node->driver != NULL ? node->driver : "",
        node != NULL && node->bus_info != NULL ? node->bus_info : "");
}


static void
assign_device_ids_and_names(GPtrArray *devices)
{
    GPtrArray *name_bases = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(devices, i);
        device->stable_id = capture_device_stable_id(devices, device);
        g_ptr_array_add(name_bases, device_display_name_base(device));
    }

    for (guint i = 0; i < devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(devices, i);
        gboolean duplicate_name = FALSE;
        gboolean duplicate_serial = FALSE;
        for (guint j = 0; j < devices->len; j++) {
            CaptureDevice *other = g_ptr_array_index(devices, j);
            if (i == j ||
                g_strcmp0(g_ptr_array_index(name_bases, i),
                          g_ptr_array_index(name_bases, j)) != 0)
                continue;
            duplicate_name = TRUE;
            if (device->serial != NULL && other->serial != NULL &&
                g_strcmp0(device->serial, other->serial) == 0)
                duplicate_serial = TRUE;
        }
        if (duplicate_name) {
            const gchar *suffix = device->serial != NULL && !duplicate_serial
                ? device->serial : device->physical_sysfs;
            device->display_name = g_strdup_printf("%s (%s)",
                (const gchar *)g_ptr_array_index(name_bases, i), suffix);
        } else {
            device->display_name = g_strdup(g_ptr_array_index(name_bases, i));
        }
    }
    g_ptr_array_unref(name_bases);
}


static gint
compare_video_nodes_by_path(gconstpointer left_pointer,
                            gconstpointer right_pointer)
{
    const CaptureVideoNode *left = *(CaptureVideoNode * const *)left_pointer;
    const CaptureVideoNode *right = *(CaptureVideoNode * const *)right_pointer;
    return g_strcmp0(left->path, right->path);
}

static gint
compare_devices(gconstpointer left_pointer, gconstpointer right_pointer)
{
    const CaptureDevice *left = *(CaptureDevice * const *)left_pointer;
    const CaptureDevice *right = *(CaptureDevice * const *)right_pointer;
    gint by_name = g_strcmp0(left->display_name, right->display_name);
    return by_name != 0 ? by_name : g_strcmp0(left->stable_id, right->stable_id);
}

GPtrArray *
capture_devices_enumerate(GError **error)
{
    DIR *dir = opendir("/sys/class/video4linux");
    if (dir == NULL) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot scan /sys/class/video4linux: %s", g_strerror(errno));
        return NULL;
    }

    GPtrArray *devices =
        g_ptr_array_new_with_free_func((GDestroyNotify)capture_device_free);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!video_class_entry(entry->d_name))
            continue;

        gchar *class_path = g_build_filename("/sys/class/video4linux",
                                             entry->d_name, "device", NULL);
        gchar resolved[PATH_MAX];
        if (realpath(class_path, resolved) == NULL) {
            g_debug("Skipping %s: cannot resolve sysfs device: %s",
                    entry->d_name, g_strerror(errno));
            g_free(class_path);
            continue;
        }
        g_free(class_path);

        gchar *dev_path = g_build_filename("/dev", entry->d_name, NULL);
        CaptureVideoNode *node = query_video_node(dev_path, resolved);
        g_free(dev_path);
        if (node == NULL)
            continue;

        gchar *usb_sysfs = NULL;
        gchar *manufacturer = NULL;
        gchar *product = NULL;
        gchar *serial = NULL;
        guint16 usb_vid = 0;
        guint16 usb_pid = 0;
        gboolean is_usb = find_usb_parent(
            resolved, &usb_sysfs, &usb_vid, &usb_pid,
            &manufacturer, &product, &serial);
        const gchar *physical_sysfs = is_usb ? usb_sysfs : resolved;
        CaptureDevice *device = find_device_by_parent(devices, physical_sysfs);
        if (device == NULL) {
            device = g_new0(CaptureDevice, 1);
            device->physical_sysfs = g_strdup(physical_sysfs);
            device->usb_sysfs = usb_sysfs;
            device->usb_vid = usb_vid;
            device->usb_pid = usb_pid;
            device->manufacturer = manufacturer;
            device->product = product;
            device->serial = serial;
            device->nodes =
                g_ptr_array_new_with_free_func((GDestroyNotify)capture_video_node_free);
            g_ptr_array_add(devices, device);
        } else {
            g_free(usb_sysfs);
            g_free(manufacturer);
            g_free(product);
            g_free(serial);
        }
        g_ptr_array_add(device->nodes, node);
    }
    closedir(dir);

    for (guint i = 0; i < devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(devices, i);
        g_ptr_array_sort(device->nodes, compare_video_nodes_by_path);
    }
    assign_device_ids_and_names(devices);
    g_ptr_array_sort(devices, compare_devices);
    return devices;
}

CaptureDevice *
capture_device_find_by_id(GPtrArray *devices, const gchar *stable_id)
{
    for (guint i = 0; devices != NULL && stable_id != NULL && i < devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(devices, i);
        if (g_strcmp0(device->stable_id, stable_id) == 0)
            return device;
    }
    return NULL;
}

CaptureDevice *
capture_device_find_by_physical_path(GPtrArray *devices,
                                     const gchar *physical_sysfs)
{
    for (guint i = 0; devices != NULL && physical_sysfs != NULL &&
         i < devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(devices, i);
        if (g_strcmp0(device->physical_sysfs, physical_sysfs) == 0)
            return device;
    }
    return NULL;
}

void
capture_device_free(CaptureDevice *device)
{
    if (device == NULL)
        return;
    g_free(device->stable_id);
    g_free(device->display_name);
    g_free(device->physical_sysfs);
    g_free(device->usb_sysfs);
    g_free(device->manufacturer);
    g_free(device->product);
    g_free(device->serial);
    if (device->nodes != NULL)
        g_ptr_array_unref(device->nodes);
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
            if (errno != EINVAL && errno != ENOTTY &&
                errno != EOPNOTSUPP && errno != ENOSYS && !found)
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
                g_debug("Frame interval range for %ux%u exceeds explicit expansion limit; adding valid standard rates",
                        width, height);
        } else {
            g_debug("Continuous frame interval range for %ux%u; adding valid standard rates and endpoints",
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
            g_debug("Frame-size range for FourCC %.4s is represented by endpoints and valid standard sizes",
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
capture_modes_enumerate(const CaptureVideoNode *node, GError **error)
{
    int fd = open(node->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot open %s: %s", node->path, g_strerror(errno));
        return NULL;
    }

    GPtrArray *modes = g_ptr_array_new_with_free_func((GDestroyNotify)capture_mode_free);
    struct v4l2_fmtdesc format = {0};
    format.type = node->buffer_type;
    gboolean found = FALSE;
    for (guint index = 0; index < 256; index++) {
        format.index = index;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &format) < 0) {
            if (errno != EINVAL && !found)
                g_warning("VIDIOC_ENUM_FMT on %s failed: %s", node->path,
                          g_strerror(errno));
            break;
        }
        found = TRUE;
        gboolean compressed = (format.flags & V4L2_FMT_FLAG_COMPRESSED) != 0;
        enumerate_frame_sizes(fd, node->buffer_type, format.pixelformat,
                              compressed, modes);
    }
    close(fd);

    if (modes->len == 0) {
        g_ptr_array_unref(modes);
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "No usable V4L2 capture modes were enumerated from %s", node->path);
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

CaptureUsbStatus
capture_usb_status_at(const gchar *sysfs_devices_path)
{
    if (sysfs_devices_path == NULL || *sysfs_devices_path == '\0')
        return CAPTURE_USB_SYSFS_UNAVAILABLE;
    GError *error = NULL;
    GDir *directory = g_dir_open(sysfs_devices_path, 0, &error);
    if (directory == NULL) {
        g_clear_error(&error);
        return CAPTURE_USB_SYSFS_UNAVAILABLE;
    }

    gboolean have_usb_device = FALSE;
    gboolean known_capture = FALSE;
    gboolean uvc_interface = FALSE;
    const gchar *entry = NULL;
    while ((entry = g_dir_read_name(directory)) != NULL) {
        gchar *path = g_build_filename(sysfs_devices_path, entry, NULL);
        gchar *vendor = read_sysfs_attribute(path, "idVendor");
        gchar *product = read_sysfs_attribute(path, "idProduct");
        gchar *interface_class = read_sysfs_attribute(path, "bInterfaceClass");
        guint16 vendor_id = 0;
        guint16 product_id = 0;

        if (parse_usb_id(vendor, &vendor_id) &&
            parse_usb_id(product, &product_id)) {
            have_usb_device = TRUE;
            if (vendor_id == 0x345f && product_id == 0x2130)
                known_capture = TRUE;
        }
        if (g_strcmp0(interface_class, "0e") == 0)
            uvc_interface = TRUE;

        g_free(vendor);
        g_free(product);
        g_free(interface_class);
        g_free(path);
    }
    g_dir_close(directory);

    if (uvc_interface)
        return CAPTURE_USB_UVC_INTERFACE;
    if (known_capture)
        return CAPTURE_USB_KNOWN_CAPTURE_NO_UVC;
    if (have_usb_device)
        return CAPTURE_USB_ENUMERATED_NO_VIDEO;
    return CAPTURE_USB_NO_DEVICES;
}

CaptureUsbStatus
capture_usb_status(void)
{
    return capture_usb_status_at("/sys/bus/usb/devices");
}
