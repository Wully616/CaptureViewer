#pragma once

#include "capture.h"

#include <glib.h>

typedef enum {
    CAPTURE_UVC_STATE_UNSUPPORTED_PLATFORM,
    CAPTURE_UVC_STATE_FLATPAK_UNSUPPORTED,
    CAPTURE_UVC_STATE_NATIVE_AVAILABLE,
    CAPTURE_UVC_STATE_COMPAT_LOADED,
    CAPTURE_UVC_STATE_COMPAT_LOADED_UNMANAGED,
    CAPTURE_UVC_STATE_COMPAT_INSTALLED_NOT_LOADED,
    CAPTURE_UVC_STATE_DRIVER_REQUIRED,
    CAPTURE_UVC_STATE_USB_STATUS_UNAVAILABLE,
    CAPTURE_UVC_STATE_DRIVER_UPDATE_REQUIRED,
    CAPTURE_UVC_STATE_USB_NOT_ENUMERATING,
    CAPTURE_UVC_STATE_NO_USB_VIDEO_DEVICE,
    CAPTURE_UVC_STATE_DEVICE_NOT_UVC,
    CAPTURE_UVC_STATE_HELPER_UNAVAILABLE,
} CaptureUvcState;

typedef struct {
    gboolean target_steamos;
    gboolean flatpak;
    gboolean kernel_module_available;
    gboolean helper_available;
    gboolean service_installed;
    gboolean current_kernel_modules;
    gboolean module_loaded;
    CaptureUsbStatus usb_status;
} CaptureUvcSetupFacts;

CaptureUvcState capture_uvc_setup_state(const CaptureUvcSetupFacts *facts);
void capture_uvc_setup_probe(CaptureUvcSetupFacts *facts);
gchar *capture_uvc_setup_script_path(void);
gchar *capture_uvc_setup_helper_path(void);
