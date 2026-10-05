#include "../src/uvc_setup.h"

#include <glib/gstdio.h>

static CaptureUvcSetupFacts
facts_for(CaptureUsbStatus usb_status)
{
    return (CaptureUvcSetupFacts) {
        .target_steamos = TRUE,
        .helper_available = TRUE,
        .usb_status = usb_status,
    };
}

static void
test_native_driver_precedes_compatibility(void)
{
    CaptureUvcSetupFacts facts = facts_for(CAPTURE_USB_UVC_INTERFACE);
    facts.kernel_module_available = TRUE;
    facts.service_installed = TRUE;
    facts.current_kernel_modules = FALSE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_NATIVE_AVAILABLE);
}

static void
test_flatpak_does_not_request_host_install(void)
{
    CaptureUvcSetupFacts facts = facts_for(CAPTURE_USB_UVC_INTERFACE);
    facts.flatpak = TRUE;
    facts.target_steamos = FALSE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_FLATPAK_UNSUPPORTED);
}

static void
test_kernel_update_requires_exact_bundle(void)
{
    CaptureUvcSetupFacts facts = facts_for(CAPTURE_USB_UVC_INTERFACE);
    facts.service_installed = TRUE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_DRIVER_UPDATE_REQUIRED);
    facts.current_kernel_modules = TRUE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_COMPAT_INSTALLED_NOT_LOADED);
    facts.module_loaded = TRUE;
    facts.current_kernel_modules = FALSE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_COMPAT_LOADED);
}

static void
test_installed_unloaded_bundle_requires_usb_interface(void)
{
    CaptureUvcSetupFacts facts = facts_for(CAPTURE_USB_NO_DEVICES);
    facts.service_installed = TRUE;
    facts.current_kernel_modules = TRUE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_USB_NOT_ENUMERATING);
    facts.usb_status = CAPTURE_USB_SYSFS_UNAVAILABLE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_USB_STATUS_UNAVAILABLE);
    facts.usb_status = CAPTURE_USB_ENUMERATED_NO_VIDEO;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_NO_USB_VIDEO_DEVICE);
    facts.usb_status = CAPTURE_USB_KNOWN_CAPTURE_NO_UVC;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_DEVICE_NOT_UVC);
    facts.usb_status = CAPTURE_USB_UVC_INTERFACE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_COMPAT_INSTALLED_NOT_LOADED);
}

static void
test_usb_absence_precedes_missing_helper(void)
{
    CaptureUvcSetupFacts facts = facts_for(CAPTURE_USB_NO_DEVICES);
    facts.service_installed = TRUE;
    facts.helper_available = FALSE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_USB_NOT_ENUMERATING);
}

static void
test_unsafe_helper_blocks_setup(void)
{
    CaptureUvcSetupFacts facts = facts_for(CAPTURE_USB_UVC_INTERFACE);
    facts.helper_available = FALSE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_HELPER_UNAVAILABLE);
}

static void
test_no_uvc_interface_does_not_offer_install(void)
{
    CaptureUvcSetupFacts facts = facts_for(CAPTURE_USB_ENUMERATED_NO_VIDEO);
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_NO_USB_VIDEO_DEVICE);
}

static void
test_helper_path_is_fixed_root_location(void)
{
    gchar *path = capture_uvc_setup_helper_path();
    g_assert_cmpstr(path, ==,
                    "/srv/captureviewer/uvc-helper/"
                    "captureviewer-driver-helper");
    g_free(path);
}


static void
test_usb_absence_does_not_trigger_driver_install(void)
{
    CaptureUvcSetupFacts facts = facts_for(CAPTURE_USB_SYSFS_UNAVAILABLE);
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_USB_STATUS_UNAVAILABLE);
    facts.usb_status = CAPTURE_USB_NO_DEVICES;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_USB_NOT_ENUMERATING);
    facts.usb_status = CAPTURE_USB_ENUMERATED_NO_VIDEO;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_NO_USB_VIDEO_DEVICE);
    facts.usb_status = CAPTURE_USB_KNOWN_CAPTURE_NO_UVC;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_DEVICE_NOT_UVC);
}

static void
test_only_enumerated_uvc_interface_offers_install(void)
{
    CaptureUvcSetupFacts facts = facts_for(CAPTURE_USB_ENUMERATED_NO_VIDEO);
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_NO_USB_VIDEO_DEVICE);
    facts.usb_status = CAPTURE_USB_UVC_INTERFACE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_DRIVER_REQUIRED);
    facts.helper_available = FALSE;
    g_assert_cmpint(capture_uvc_setup_state(&facts), ==,
                    CAPTURE_UVC_STATE_HELPER_UNAVAILABLE);
}

static void
remove_tree(const gchar *path)
{
    GDir *directory = g_dir_open(path, 0, NULL);
    if (directory != NULL) {
        const gchar *name = NULL;
        while ((name = g_dir_read_name(directory)) != NULL) {
            gchar *child = g_build_filename(path, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR))
                remove_tree(child);
            else
                g_remove(child);
            g_free(child);
        }
        g_dir_close(directory);
    }
    g_rmdir(path);
}

static gchar *
new_sysfs_tree(void)
{
    GError *error = NULL;
    gchar *path = g_dir_make_tmp("captureviewer-usb-test-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(path);
    return path;
}

static void
write_attribute(const gchar *root, const gchar *device, const gchar *name,
                const gchar *value)
{
    gchar *directory = g_build_filename(root, device, NULL);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    gchar *path = g_build_filename(directory, name, NULL);
    g_assert_true(g_file_set_contents(path, value, -1, NULL));
    g_free(path);
    g_free(directory);
}

static void
test_usb_sysfs_states(void)
{
    gchar *missing_parent = new_sysfs_tree();
    gchar *missing = g_build_filename(missing_parent, "devices", NULL);
    g_assert_cmpint(capture_usb_status_at(missing), ==,
                    CAPTURE_USB_SYSFS_UNAVAILABLE);
    g_rmdir(missing_parent);
    g_free(missing);
    g_free(missing_parent);

    gchar *root = new_sysfs_tree();
    g_assert_cmpint(capture_usb_status_at(root), ==, CAPTURE_USB_NO_DEVICES);

    write_attribute(root, "usb1", "idVendor", "1d6b");
    write_attribute(root, "usb1", "idProduct", "0002");
    g_assert_cmpint(capture_usb_status_at(root), ==,
                    CAPTURE_USB_ENUMERATED_NO_VIDEO);

    write_attribute(root, "1-1", "idVendor", "345f");
    write_attribute(root, "1-1", "idProduct", "2130");
    g_assert_cmpint(capture_usb_status_at(root), ==,
                    CAPTURE_USB_KNOWN_CAPTURE_NO_UVC);

    write_attribute(root, "1-1:1.0", "bInterfaceClass", "0e");
    g_assert_cmpint(capture_usb_status_at(root), ==,
                    CAPTURE_USB_UVC_INTERFACE);
    remove_tree(root);
    g_free(root);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/uvc/setup/native-precedence",
                    test_native_driver_precedes_compatibility);
    g_test_add_func("/uvc/setup/flatpak-host-boundary",
                    test_flatpak_does_not_request_host_install);
    g_test_add_func("/uvc/setup/kernel-update-requires-exact-bundle",
                    test_kernel_update_requires_exact_bundle);
    g_test_add_func("/uvc/setup/installed-unloaded-needs-uvc-interface",
                    test_installed_unloaded_bundle_requires_usb_interface);
    g_test_add_func("/uvc/setup/usb-absence-precedes-missing-helper",
                    test_usb_absence_precedes_missing_helper);
    g_test_add_func("/uvc/setup/usb-absence-does-not-install",
                    test_usb_absence_does_not_trigger_driver_install);
    g_test_add_func("/uvc/setup/only-uvc-interface-offers-install",
                    test_only_enumerated_uvc_interface_offers_install);
    g_test_add_func("/uvc/setup/unsafe-helper-blocks-setup",
                    test_unsafe_helper_blocks_setup);
    g_test_add_func("/uvc/setup/no-uvc-interface-does-not-install",
                    test_no_uvc_interface_does_not_offer_install);
    g_test_add_func("/uvc/setup/helper-fixed-root-location",
                    test_helper_path_is_fixed_root_location);
    g_test_add_func("/uvc/usb/sysfs-states", test_usb_sysfs_states);
    return g_test_run();
}
