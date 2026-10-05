#define _GNU_SOURCE
#include "uvc_setup.h"
#include "captureviewer-config.h"

#include <glib/gstdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <string.h>

static gboolean
path_is_root_owned(const gchar *path, gboolean directory, gboolean executable)
{
    GStatBuf st;
    if (g_lstat(path, &st) != 0 || st.st_uid != 0 ||
        (st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        return FALSE;
    if (directory && !S_ISDIR(st.st_mode))
        return FALSE;
    if (!directory && !S_ISREG(st.st_mode))
        return FALSE;
    return !executable || (st.st_mode & S_IXUSR) != 0;
}

static gboolean
root_owned_path_with_safe_ancestors(const gchar *path, gboolean directory,
                                    gboolean executable)
{
    if (!path_is_root_owned(path, directory, executable))
        return FALSE;
    gchar *parent = g_path_get_dirname(path);
    gboolean safe = TRUE;
    while (TRUE) {
        if (!path_is_root_owned(parent, TRUE, FALSE)) {
            safe = FALSE;
            break;
        }
        if (g_str_equal(parent, "/"))
            break;
        gchar *next = g_path_get_dirname(parent);
        g_free(parent);
        parent = next;
    }
    g_free(parent);
    return safe;
}

static gchar *
running_executable_dir(void)
{
    gchar *executable = g_file_read_link("/proc/self/exe", NULL);
    if (executable == NULL)
        return NULL;
    gchar *directory = g_path_get_dirname(executable);
    g_free(executable);
    return directory;
}

gchar *
capture_uvc_setup_script_path(void)
{
    gchar *executable_dir = running_executable_dir();
    if (executable_dir != NULL) {
        gchar *relocated = g_build_filename(executable_dir, "..", "libexec",
                                            "captureviewer", "uvc",
                                            "setup.sh", NULL);
        if (g_file_test(relocated, G_FILE_TEST_IS_EXECUTABLE)) {
            gchar *resolved = g_canonicalize_filename(relocated, NULL);
            g_free(relocated);
            g_free(executable_dir);
            return resolved;
        }
        g_free(relocated);
        g_free(executable_dir);
    }
    return g_build_filename(CAPTUREVIEWER_UVC_LIBEXECDIR, "setup.sh", NULL);
}

gchar *
capture_uvc_setup_helper_path(void)
{
    return g_strdup("/srv/captureviewer/uvc-helper/"
                    "captureviewer-driver-helper");
}

static gboolean
helper_files_are_secure(void)
{
    static const struct {
        const gchar *path;
        gboolean executable;
    } assets[] = {
        { "/srv/captureviewer/uvc-helper/captureviewer-driver-helper", TRUE },
        { "/srv/captureviewer/uvc-helper/load.sh", TRUE },
        { "/srv/captureviewer/uvc-helper/verify.py", FALSE },
        { "/srv/captureviewer/uvc-helper/captureviewer-uvc.service", FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(assets); i++) {
        if (!root_owned_path_with_safe_ancestors(assets[i].path, FALSE,
                                                 assets[i].executable))
            return FALSE;
    }
    return TRUE;
}

static gboolean
service_path_is_secure(const gchar *path)
{
    return root_owned_path_with_safe_ancestors(path, FALSE, FALSE);
}

static gboolean
support_directory_is_secure(const gchar *path)
{
    return root_owned_path_with_safe_ancestors(path, TRUE, FALSE);
}

static gboolean
os_release_is_steamos(const gchar *path)
{
    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return FALSE;
    gboolean found = FALSE;
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!g_str_has_prefix(line, "ID="))
            continue;
        gchar *value = g_strstrip(line + 3);
        gsize length = strlen(value);
        if (length >= 2 &&
            ((value[0] == '"' && value[length - 1] == '"') ||
             (value[0] == '\'' && value[length - 1] == '\''))) {
            value[length - 1] = '\0';
            value++;
        }
        found = g_str_equal(value, "steamos");
        break;
    }
    g_strfreev(lines);
    g_free(contents);
    return found;
}

static gboolean
is_steamos_arm64(gboolean flatpak)
{
    struct utsname system_info;
    if (uname(&system_info) != 0 || !g_str_equal(system_info.machine, "aarch64"))
        return FALSE;
    return os_release_is_steamos(flatpak
        ? "/run/host/etc/os-release" : "/etc/os-release");
}

static gboolean
run_program_succeeded(gchar **argv)
{
    gint status = 0;
    GError *error = NULL;
    gboolean spawned = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                    NULL, NULL, NULL, NULL, &status, &error);
    gboolean succeeded = spawned && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    g_clear_error(&error);
    return succeeded;
}

static gboolean
kernel_has_uvcvideo(void)
{
    struct utsname system_info;
    gchar *modinfo = g_find_program_in_path("modinfo");
    if (modinfo == NULL || uname(&system_info) != 0) {
        g_free(modinfo);
        return FALSE;
    }
    gchar *argv[] = { modinfo, "-k", system_info.release, "uvcvideo", NULL };
    gboolean available = run_program_succeeded(argv);
    g_free(modinfo);
    return available;
}

static gboolean
installed_service_exists(void)
{
    const gchar *unit =
        "/etc/systemd/system/captureviewer-uvc.service";
    const gchar *marker =
        "/srv/captureviewer/uvc/.captureviewer-owned";
    return helper_files_are_secure() &&
        root_owned_path_with_safe_ancestors(unit, FALSE, FALSE) &&
        root_owned_path_with_safe_ancestors(marker, FALSE, FALSE) &&
        support_directory_is_secure("/srv/captureviewer/uvc");
}

static gboolean
modules_exist_for_kernel(void)
{
    struct utsname system_info;
    if (uname(&system_info) != 0)
        return FALSE;
    gchar *directory = g_build_filename("/srv/captureviewer/uvc",
                                        system_info.release, NULL);
    gchar *verifier = g_build_filename("/srv/captureviewer/uvc-helper",
                                       "verify.py", NULL);
    gchar *python = g_find_program_in_path("python3");
    if (python == NULL || !service_path_is_secure(verifier) ||
        !support_directory_is_secure(directory)) {
        g_free(python);
        g_free(verifier);
        g_free(directory);
        return FALSE;
    }
    gchar *argv[] = { python, verifier, directory, system_info.release,
                      "--require-root", NULL };
    gboolean valid = run_program_succeeded(argv);
    g_free(python);
    g_free(verifier);
    g_free(directory);
    return valid;
}


void
capture_uvc_setup_probe(CaptureUvcSetupFacts *facts)
{
    g_return_if_fail(facts != NULL);
    memset(facts, 0, sizeof(*facts));
    const gchar *flatpak_id = g_getenv("FLATPAK_ID");
    facts->flatpak = flatpak_id != NULL && *flatpak_id != '\0';
    facts->target_steamos = is_steamos_arm64(facts->flatpak);
    facts->module_loaded = g_file_test("/sys/module/uvcvideo", G_FILE_TEST_EXISTS);
    facts->usb_status = capture_usb_status();
    if (!facts->module_loaded &&
        facts->usb_status != CAPTURE_USB_UVC_INTERFACE)
        return;

    facts->kernel_module_available = kernel_has_uvcvideo();
    facts->helper_available = helper_files_are_secure();
    facts->service_installed = installed_service_exists();
    facts->current_kernel_modules = modules_exist_for_kernel();
}

CaptureUvcState
capture_uvc_setup_state(const CaptureUvcSetupFacts *facts)
{
    g_return_val_if_fail(facts != NULL, CAPTURE_UVC_STATE_UNSUPPORTED_PLATFORM);
    if (facts->kernel_module_available &&
        (facts->target_steamos || facts->flatpak))
        return CAPTURE_UVC_STATE_NATIVE_AVAILABLE;
    if (facts->flatpak)
        return CAPTURE_UVC_STATE_FLATPAK_UNSUPPORTED;
    if (!facts->target_steamos)
        return CAPTURE_UVC_STATE_UNSUPPORTED_PLATFORM;
    if (facts->module_loaded)
        return facts->service_installed
            ? CAPTURE_UVC_STATE_COMPAT_LOADED
            : CAPTURE_UVC_STATE_COMPAT_LOADED_UNMANAGED;
    if (facts->service_installed &&
        facts->usb_status == CAPTURE_USB_UVC_INTERFACE) {
        if (!facts->helper_available)
            return CAPTURE_UVC_STATE_HELPER_UNAVAILABLE;
        if (!facts->current_kernel_modules)
            return CAPTURE_UVC_STATE_DRIVER_UPDATE_REQUIRED;
        return CAPTURE_UVC_STATE_COMPAT_INSTALLED_NOT_LOADED;
    }
    if (facts->usb_status == CAPTURE_USB_SYSFS_UNAVAILABLE)
        return CAPTURE_UVC_STATE_USB_STATUS_UNAVAILABLE;
    if (facts->usb_status == CAPTURE_USB_NO_DEVICES)
        return CAPTURE_UVC_STATE_USB_NOT_ENUMERATING;
    if (facts->usb_status == CAPTURE_USB_KNOWN_CAPTURE_NO_UVC)
        return CAPTURE_UVC_STATE_DEVICE_NOT_UVC;
    if (facts->usb_status == CAPTURE_USB_UVC_INTERFACE)
        return facts->helper_available
            ? CAPTURE_UVC_STATE_DRIVER_REQUIRED
            : CAPTURE_UVC_STATE_HELPER_UNAVAILABLE;
    return CAPTURE_UVC_STATE_NO_USB_VIDEO_DEVICE;
}
