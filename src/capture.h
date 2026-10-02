#pragma once

#include <glib.h>
#include <linux/videodev2.h>

G_BEGIN_DECLS

typedef struct {
    guint32 fourcc;
    guint width;
    guint height;
    guint fps_n;
    guint fps_d;
    gboolean compressed;
    gchar *format_name;
    gchar *label;
} CaptureMode;

typedef struct {
    gchar *path;
    gchar *interface_sysfs;
    gchar *usb_sysfs;
    gchar *manufacturer;
    gchar *product;
    gchar *serial;
    gchar *card_name;
    enum v4l2_buf_type buffer_type;
} CaptureDevice;

gboolean capture_hagibis_usb_detected(void);
CaptureDevice *capture_device_find_hagibis(GError **error);
void capture_device_free(CaptureDevice *device);
GPtrArray *capture_modes_enumerate(const CaptureDevice *device, GError **error);
void capture_mode_free(CaptureMode *mode);
gchar *capture_mode_key(const CaptureMode *mode);
gboolean capture_mode_equal_key(const CaptureMode *mode, const gchar *key);

G_END_DECLS
