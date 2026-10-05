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
    gchar *card_name;
    gchar *driver;
    gchar *bus_info;
    guint32 capabilities;
    enum v4l2_buf_type buffer_type;
} CaptureVideoNode;

typedef struct {
    gchar *stable_id;
    gchar *display_name;
    gchar *physical_sysfs;
    gchar *usb_sysfs;
    guint16 usb_vid;
    guint16 usb_pid;
    gchar *manufacturer;
    gchar *product;
    gchar *serial;
    GPtrArray *nodes;
} CaptureDevice;

typedef enum {
    CAPTURE_USB_SYSFS_UNAVAILABLE,
    CAPTURE_USB_NO_DEVICES,
    CAPTURE_USB_ENUMERATED_NO_VIDEO,
    CAPTURE_USB_KNOWN_CAPTURE_NO_UVC,
    CAPTURE_USB_UVC_INTERFACE,
} CaptureUsbStatus;

/* Describes USB enumeration independently of V4L2 node discovery. */
CaptureUsbStatus capture_usb_status(void);
/* Allows callers to inspect a fixture tree; production uses /sys/bus/usb/devices. */
CaptureUsbStatus capture_usb_status_at(const gchar *sysfs_devices_path);

GPtrArray *capture_devices_enumerate(GError **error);
/* Returns the snapshot-specific stable ID for a device; caller owns the result. */
gchar *capture_device_stable_id(GPtrArray *devices, const CaptureDevice *device);
/* Returns a borrowed device pointer, or NULL when the ID is absent. */
CaptureDevice *capture_device_find_by_id(GPtrArray *devices, const gchar *stable_id);
/* Returns a borrowed device pointer, or NULL when the physical path is absent. */
CaptureDevice *capture_device_find_by_physical_path(GPtrArray *devices,
                                                    const gchar *physical_sysfs);
void capture_video_node_free(CaptureVideoNode *node);
void capture_device_free(CaptureDevice *device);
GPtrArray *capture_modes_enumerate(const CaptureVideoNode *node, GError **error);
void capture_mode_free(CaptureMode *mode);
gchar *capture_mode_key(const CaptureMode *mode);
gboolean capture_mode_equal_key(const CaptureMode *mode, const gchar *key);

G_END_DECLS
