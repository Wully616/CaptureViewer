#pragma once

#include <gtk/gtk.h>

typedef struct _CaptureViewerApp CaptureViewerApp;

CaptureViewerApp *capture_viewer_app_new(void);
/* Frees the app and any subsystems not already released by shutdown. */
void capture_viewer_app_free(CaptureViewerApp *app);
void capture_viewer_app_activate(GtkApplication *application,
                                 gpointer user_data);
void capture_viewer_app_shutdown(GApplication *application,
                                 gpointer user_data);
gint capture_viewer_app_list_modes(void);
