#include "app.h"

#include <gst/gst.h>

int
main(int argc, char **argv)
{
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
