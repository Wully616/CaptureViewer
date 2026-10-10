#include "app.h"

#include <gst/gst.h>

static void
configure_bundled_gstreamer_plugins(void)
{
    gchar *executable = g_file_read_link("/proc/self/exe", NULL);
    if (executable == NULL)
        return;

    gchar *binary_dir = g_path_get_dirname(executable);
    gchar *relative_plugin_dir = g_build_filename(
        binary_dir, "..", "lib", "captureviewer", "gstreamer-1.0", NULL);
    gchar *plugin_dir = g_canonicalize_filename(relative_plugin_dir, NULL);
    if (g_file_test(plugin_dir, G_FILE_TEST_IS_DIR)) {
        const gchar *existing_path = g_getenv("GST_PLUGIN_PATH_1_0");
        if (existing_path == NULL || *existing_path == '\0')
            existing_path = g_getenv("GST_PLUGIN_PATH");
        gchar *search_path = existing_path != NULL && *existing_path != '\0'
                                 ? g_strjoin(G_SEARCHPATH_SEPARATOR_S,
                                             plugin_dir, existing_path, NULL)
                                 : g_strdup(plugin_dir);
        g_setenv("GST_PLUGIN_PATH_1_0", search_path, TRUE);
        g_free(search_path);
    }
    g_free(plugin_dir);
    g_free(relative_plugin_dir);
    g_free(binary_dir);
    g_free(executable);
}

int
main(int argc, char **argv)
{
    configure_bundled_gstreamer_plugins();
    gst_init(&argc, &argv);
    if (argc > 1 && g_str_equal(argv[1], "--list-modes"))
        return capture_viewer_app_list_modes();

    CaptureViewerApp *app = capture_viewer_app_new();
    GtkApplication *application = gtk_application_new(
        "io.github.wully616.captureviewer", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate",
                     G_CALLBACK(capture_viewer_app_activate), app);
    g_signal_connect(application, "shutdown",
                     G_CALLBACK(capture_viewer_app_shutdown), app);
    gint status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    capture_viewer_app_free(app);
    return status;
}
