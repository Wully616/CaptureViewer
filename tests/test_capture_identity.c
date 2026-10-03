#include "../src/capture.h"

static CaptureDevice *
new_test_device(const gchar *sysfs_path)
{
    CaptureDevice *device = g_new0(CaptureDevice, 1);
    device->physical_sysfs = g_strdup(sysfs_path);
    device->usb_sysfs = g_strdup(sysfs_path);
    device->usb_vid = 0x345f;
    device->usb_pid = 0x2130;
    device->serial = g_strdup("duplicate-serial");
    device->nodes = g_ptr_array_new_with_free_func(
        (GDestroyNotify)capture_video_node_free);
    return device;
}

static GPtrArray *
new_snapshot(gboolean include_sibling)
{
    GPtrArray *devices = g_ptr_array_new_with_free_func(
        (GDestroyNotify)capture_device_free);
    g_ptr_array_add(devices, new_test_device("/sys/devices/usb/1-1"));
    if (include_sibling)
        g_ptr_array_add(devices, new_test_device("/sys/devices/usb/1-2"));
    for (guint i = 0; i < devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(devices, i);
        device->stable_id = capture_device_stable_id(devices, device);
    }
    return devices;
}

static void
test_duplicate_serial_sibling_rescan(void)
{
    GPtrArray *before_add = new_snapshot(FALSE);
    CaptureDevice *selected = g_ptr_array_index(before_add, 0);
    gchar *selected_id = g_strdup(selected->stable_id);
    gchar *selected_parent = g_strdup(selected->physical_sysfs);
    g_assert_cmpstr(selected_id, ==, "usb:345f:2130:serial:duplicate-serial");

    GPtrArray *after_add = new_snapshot(TRUE);
    CaptureDevice *retained =
        capture_device_find_by_id(after_add, selected_id);
    g_assert_null(retained);
    retained = capture_device_find_by_physical_path(after_add, selected_parent);
    g_assert_nonnull(retained);
    g_assert_cmpstr(retained->physical_sysfs, ==, selected_parent);
    g_assert_cmpstr(retained->stable_id, !=, selected_id);
    g_assert_cmpstr(retained->stable_id, ==,
                    "usb:345f:2130:serial:duplicate-serial:sysfs:/sys/devices/usb/1-1");
    g_free(selected_id);
    selected_id = g_strdup(retained->stable_id);
    CaptureDevice *sibling = g_ptr_array_index(after_add, 1);
    g_assert_cmpstr(sibling->stable_id, !=, selected_id);
    g_assert_cmpstr(sibling->stable_id, ==,
                    "usb:345f:2130:serial:duplicate-serial:sysfs:/sys/devices/usb/1-2");

    g_ptr_array_unref(after_add);
    GPtrArray *after_remove = new_snapshot(FALSE);
    retained = capture_device_find_by_id(after_remove, selected_id);
    g_assert_null(retained);
    retained = capture_device_find_by_physical_path(after_remove, selected_parent);
    g_assert_nonnull(retained);
    g_assert_cmpstr(retained->physical_sysfs, ==, selected_parent);
    g_assert_cmpstr(retained->stable_id, ==,
                    "usb:345f:2130:serial:duplicate-serial");

    g_free(selected_id);
    g_free(selected_parent);
    g_ptr_array_unref(after_remove);
    g_ptr_array_unref(before_add);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/capture/device-id/duplicate-serial-sibling-rescan",
                    test_duplicate_serial_sibling_rescan);
    return g_test_run();
}
