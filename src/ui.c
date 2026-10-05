#include "ui.h"

#include <linux/videodev2.h>

struct _CaptureUi {
    CaptureUiCallbacks callbacks;
    gpointer user_data;
    GtkWidget *window;
    GtkWidget *root_overlay;
    GtkWidget *settings;
    GtkWidget *control_panel;
    GtkWidget *control_revealer;
    GtkWidget *edge_hint;
    GtkWidget *stats_overlay_label;
    GtkWidget *stats_overlay_box;
    GtkWidget *status_overlay;
    GtkWidget *status_overlay_label;
    GtkWidget *dwell_spin;
    GtkWidget *hide_delay_spin;
    GtkWidget *pin_button;
    GtkWidget *stats_button;
    GtkWidget *device_label;
    GtkWidget *mode_diagnostic_label;
    GtkWidget *renderer_label;
    GtkWidget *source_combo;
    GtkWidget *node_combo;
    GtkWidget *node_selector;
    GtkWidget *format_combo;
    GtkWidget *resolution_combo;
    GtkWidget *fps_combo;
    GtkWidget *scale_combo;
    GtkWidget *advanced_sources_check;
    GtkWidget *audio_combo;
    GtkWidget *audio_toggle;
    GtkWidget *volume_scale;
    GtkWidget *status_label;
    GtkWidget *connection_indicator;
    GtkWidget *connection_status;
    GtkWidget *audio_route_label;
    GtkWidget *pipeline_error_label;
    GtkWidget *log_path_label;
    GtkWidget *perf_label;
    guint dwell_watch_id;
    guint panel_hide_watch_id;
    guint panel_dwell_ms;
    guint panel_hide_delay_ms;
    gint64 keyboard_active_until_us;
    gboolean updating_controls;
    gboolean fullscreen;
    gboolean pinned;
    gboolean stats_visible;
    gboolean panel_visible;
    gboolean panel_pointer_inside;
    gboolean edge_hotspot_inside;
    gboolean mode_popup_open;
    gboolean interaction_active;
    gboolean settings_open;
};

typedef struct {
    CaptureUi *ui;
    GHashTable *seen;
    const gchar *selected_id;
    gboolean selected_present;
} AudioSelectorContext;

static void
emit_action(CaptureUi *ui,
            CaptureUiActionType type,
            const gchar *value,
            gdouble number,
            gboolean enabled)
{
    if (ui->callbacks.action == NULL)
        return;
    CaptureUiAction action = {
        .type = type,
        .value = value,
        .number = number,
        .enabled = enabled,
    };
    ui->callbacks.action(&action, ui->user_data);
}

static void ui_schedule_panel_hide(CaptureUi *ui);
static void ui_show_control_panel(CaptureUi *ui);
static void ui_hide_control_panel(CaptureUi *ui);
static void show_settings(CaptureUi *ui);
static void hide_settings(CaptureUi *ui);

static gboolean
interaction_holds_panel(CaptureUi *ui)
{
    GtkWidget *grab = gtk_grab_get_current();
    return ui->pinned || ui->panel_pointer_inside || ui->edge_hotspot_inside ||
        ui->settings_open || ui->mode_popup_open || ui->interaction_active ||
        (grab != NULL && grab != ui->window) ||
        g_get_monotonic_time() < ui->keyboard_active_until_us;
}

static gboolean
panel_hide_timeout(gpointer user_data)
{
    CaptureUi *ui = user_data;
    ui->panel_hide_watch_id = 0;
    if (interaction_holds_panel(ui))
        ui_schedule_panel_hide(ui);
    else
        ui_hide_control_panel(ui);
    return G_SOURCE_REMOVE;
}

static gboolean
panel_dwell_timeout(gpointer user_data)
{
    CaptureUi *ui = user_data;
    ui->dwell_watch_id = 0;
    if (ui->edge_hotspot_inside)
        ui_show_control_panel(ui);
    return G_SOURCE_REMOVE;
}

static void
ui_schedule_panel_hide(CaptureUi *ui)
{
    if (ui->panel_hide_watch_id != 0)
        g_source_remove(ui->panel_hide_watch_id);
    if (!ui->panel_visible)
        return;
    ui->panel_hide_watch_id = g_timeout_add(ui->panel_hide_delay_ms,
                                            panel_hide_timeout, ui);
}

static void
set_panel_revealed(CaptureUi *ui, gboolean visible)
{
    ui->panel_visible = visible;
    if (ui->control_revealer != NULL)
        gtk_revealer_set_reveal_child(GTK_REVEALER(ui->control_revealer), visible);
}

static void
ui_show_control_panel(CaptureUi *ui)
{
    if (ui->dwell_watch_id != 0) {
        g_source_remove(ui->dwell_watch_id);
        ui->dwell_watch_id = 0;
    }
    if (ui->panel_hide_watch_id != 0) {
        g_source_remove(ui->panel_hide_watch_id);
        ui->panel_hide_watch_id = 0;
    }
    set_panel_revealed(ui, TRUE);
}

static void
ui_hide_control_panel(CaptureUi *ui)
{
    if (ui->pinned || ui->panel_pointer_inside || ui->edge_hotspot_inside ||
        ui->settings_open || ui->mode_popup_open || ui->interaction_active)
        return;
    set_panel_revealed(ui, FALSE);
}

static void
ui_set_edge_hotspot(CaptureUi *ui, gboolean inside)
{
    if (ui->edge_hotspot_inside == inside)
        return;
    ui->edge_hotspot_inside = inside;
    if (ui->edge_hint != NULL) {
        GtkStyleContext *style = gtk_widget_get_style_context(ui->edge_hint);
        if (inside)
            gtk_style_context_add_class(style, "active");
        else
            gtk_style_context_remove_class(style, "active");
    }
    if (inside) {
        if (!ui->panel_visible && ui->dwell_watch_id == 0)
            ui->dwell_watch_id = g_timeout_add(ui->panel_dwell_ms,
                                               panel_dwell_timeout, ui);
        else
            ui_show_control_panel(ui);
    } else {
        if (ui->dwell_watch_id != 0) {
            g_source_remove(ui->dwell_watch_id);
            ui->dwell_watch_id = 0;
        }
        ui_schedule_panel_hide(ui);
    }
}

static gboolean
edge_hint_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
    ui_set_edge_hotspot(user_data, TRUE);
    (void)widget;
    (void)event;
    return FALSE;
}

static gboolean
edge_hint_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
    ui_set_edge_hotspot(user_data, FALSE);
    (void)widget;
    (void)event;
    return FALSE;
}

static gboolean
window_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
    ui_set_edge_hotspot(user_data, event->y <= 8.0);
    (void)widget;
    return FALSE;
}

static gboolean
panel_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
    CaptureUi *ui = user_data;
    ui->panel_pointer_inside = TRUE;
    if (ui->panel_hide_watch_id != 0) {
        g_source_remove(ui->panel_hide_watch_id);
        ui->panel_hide_watch_id = 0;
    }
    ui_show_control_panel(ui);
    (void)widget;
    (void)event;
    return FALSE;
}

static gboolean
panel_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (event->detail != GDK_NOTIFY_INFERIOR) {
        ui->panel_pointer_inside = FALSE;
        ui_schedule_panel_hide(ui);
    }
    (void)widget;
    (void)event;
    return FALSE;
}

static void
mode_popup_notify(GObject *object, GParamSpec *spec, gpointer user_data)
{
    CaptureUi *ui = user_data;
    g_object_get(object, "popup-shown", &ui->mode_popup_open, NULL);
    if (ui->mode_popup_open)
        ui_show_control_panel(ui);
    else
        ui_schedule_panel_hide(ui);
    (void)spec;
}

void
capture_ui_toggle_fullscreen(CaptureUi *ui)
{
    if (ui == NULL || ui->window == NULL)
        return;
    capture_ui_set_fullscreen(ui, !ui->fullscreen);
}

void
capture_ui_set_fullscreen(CaptureUi *ui, gboolean fullscreen)
{
    if (ui == NULL || ui->window == NULL)
        return;
    if (fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(ui->window));
    else
        gtk_window_unfullscreen(GTK_WINDOW(ui->window));
    ui->fullscreen = fullscreen;
    gtk_window_present(GTK_WINDOW(ui->window));
}

static void
show_settings(CaptureUi *ui)
{
    if (ui->settings == NULL)
        return;
    ui->settings_open = TRUE;
    ui_show_control_panel(ui);
    gtk_widget_show_all(ui->settings);
    gtk_window_present(GTK_WINDOW(ui->settings));
    emit_action(ui, CAPTURE_UI_ACTION_SETTINGS_VISIBILITY, NULL, 0.0, TRUE);
}

static void
hide_settings(CaptureUi *ui)
{
    ui->settings_open = FALSE;
    if (ui->settings != NULL)
        gtk_widget_hide(ui->settings);
    ui_schedule_panel_hide(ui);
    emit_action(ui, CAPTURE_UI_ACTION_SETTINGS_VISIBILITY, NULL, 0.0, FALSE);
}

static void
close_settings_button(GtkButton *button, gpointer user_data)
{
    hide_settings(user_data);
    (void)button;
}

static void
capture_support_button_clicked(GtkButton *button, gpointer user_data)
{
    CaptureUi *ui = user_data;
    emit_action(ui, CAPTURE_UI_ACTION_CAPTURE_SUPPORT, NULL, 0.0, FALSE);
    (void)button;
}

static gboolean
close_settings(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    hide_settings(user_data);
    (void)widget;
    (void)event;
    return TRUE;
}

static void
toggle_control_panel(CaptureUi *ui)
{
    if (ui->pinned)
        return;
    if (ui->panel_visible) {
        ui->keyboard_active_until_us = 0;
        if (ui->panel_hide_watch_id != 0) {
            g_source_remove(ui->panel_hide_watch_id);
            ui->panel_hide_watch_id = 0;
        }
        set_panel_revealed(ui, FALSE);
    } else {
        ui->keyboard_active_until_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        ui_show_control_panel(ui);
        ui_schedule_panel_hide(ui);
    }
}

static gboolean
key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    CaptureUi *ui = user_data;
    switch (event->keyval) {
    case GDK_KEY_Escape:
        ui->keyboard_active_until_us = 0;
        if (ui->settings_open) {
            hide_settings(ui);
            return TRUE;
        }
        if (ui->mode_popup_open) {
            GtkWidget *selectors[] = {
                ui->source_combo, ui->node_combo, ui->format_combo,
                ui->resolution_combo, ui->fps_combo
            };
            for (guint i = 0; i < G_N_ELEMENTS(selectors); i++) {
                if (selectors[i] != NULL)
                    gtk_combo_box_popdown(GTK_COMBO_BOX(selectors[i]));
            }
            ui->mode_popup_open = FALSE;
            return TRUE;
        }
        if (ui->fullscreen)
            capture_ui_toggle_fullscreen(ui);
        if (!ui->pinned) {
            if (ui->panel_hide_watch_id != 0) {
                g_source_remove(ui->panel_hide_watch_id);
                ui->panel_hide_watch_id = 0;
            }
            set_panel_revealed(ui, FALSE);
        }
        return TRUE;
    case GDK_KEY_F11:
        ui->keyboard_active_until_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        ui_show_control_panel(ui);
        capture_ui_toggle_fullscreen(ui);
        ui_schedule_panel_hide(ui);
        return TRUE;
    case GDK_KEY_s:
    case GDK_KEY_S:
        toggle_control_panel(ui);
        return TRUE;
    case GDK_KEY_q:
    case GDK_KEY_Q:
        emit_action(ui, CAPTURE_UI_ACTION_QUIT, NULL, 0.0, FALSE);
        return TRUE;
    default:
        ui->keyboard_active_until_us = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        if (!ui->settings_open) {
            ui_show_control_panel(ui);
            ui_schedule_panel_hide(ui);
        }
        (void)widget;
        return FALSE;
    }
}

static void
on_source_changed(GtkComboBox *combo, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_SOURCE,
                    gtk_combo_box_get_active_id(combo), 0.0, FALSE);
}

static void
on_video_node_changed(GtkComboBox *combo, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_VIDEO_NODE,
                    gtk_combo_box_get_active_id(combo), 0.0, FALSE);
}

static void
on_format_changed(GtkComboBox *combo, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_FORMAT,
                    gtk_combo_box_get_active_id(combo), 0.0, FALSE);
}

static void
on_resolution_changed(GtkComboBox *combo, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_RESOLUTION,
                    gtk_combo_box_get_active_id(combo), 0.0, FALSE);
}

static void
on_frame_rate_changed(GtkComboBox *combo, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_FRAME_RATE,
                    gtk_combo_box_get_active_id(combo), 0.0, FALSE);
}

static void
on_advanced_sources_toggled(GtkToggleButton *button, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_ADVANCED_SOURCES, NULL, 0.0,
                    gtk_toggle_button_get_active(button));
}

static void
on_audio_selection_changed(GtkComboBox *combo, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_AUDIO_SELECTION,
                    gtk_combo_box_get_active_id(combo), 0.0, FALSE);
}

static void
on_audio_toggled(GtkToggleButton *button, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_AUDIO_ENABLED, NULL, 0.0,
                    gtk_toggle_button_get_active(button));
}

static void
on_scaling_mode_changed(GtkComboBox *combo, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_SCALING,
                    gtk_combo_box_get_active_id(combo), 0.0, FALSE);
}

static void
on_volume_changed(GtkRange *range, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_VOLUME, NULL,
                    gtk_range_get_value(range), FALSE);
}

static void
on_pin_toggled(GtkToggleButton *button, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (ui->updating_controls)
        return;
    ui->pinned = gtk_toggle_button_get_active(button);
    gtk_button_set_label(GTK_BUTTON(button), ui->pinned ? "Pinned" : "Pin");
    if (ui->pinned)
        ui_show_control_panel(ui);
    else
        ui_schedule_panel_hide(ui);
    emit_action(ui, CAPTURE_UI_ACTION_PINNED, NULL, 0.0, ui->pinned);
}

static void
on_stats_toggled(GtkToggleButton *button, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (ui->updating_controls)
        return;
    ui->stats_visible = gtk_toggle_button_get_active(button);
    if (ui->stats_visible)
        gtk_widget_show(ui->stats_overlay_box);
    else
        gtk_widget_hide(ui->stats_overlay_box);
    emit_action(ui, CAPTURE_UI_ACTION_STATS_VISIBLE, NULL, 0.0,
                ui->stats_visible);
}

static void
on_dwell_changed(GtkSpinButton *spin, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_DWELL_DELAY, NULL,
                    (gdouble)gtk_spin_button_get_value_as_int(spin), FALSE);
}

static void
on_hide_delay_changed(GtkSpinButton *spin, gpointer user_data)
{
    CaptureUi *ui = user_data;
    if (!ui->updating_controls)
        emit_action(ui, CAPTURE_UI_ACTION_HIDE_DELAY, NULL,
                    (gdouble)gtk_spin_button_get_value_as_int(spin), FALSE);
}

static gboolean
slider_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    CaptureUi *ui = user_data;
    ui->interaction_active = TRUE;
    ui_show_control_panel(ui);
    (void)widget;
    (void)event;
    return FALSE;
}

static gboolean
slider_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    CaptureUi *ui = user_data;
    ui->interaction_active = FALSE;
    emit_action(ui, CAPTURE_UI_ACTION_VOLUME_RELEASED, NULL, 0.0, FALSE);
    ui_schedule_panel_hide(ui);
    (void)widget;
    (void)event;
    return FALSE;
}

static GtkWidget *
diagnostic_value_label(void)
{
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_hexpand(label, TRUE);
    return label;
}

static void
attach_diagnostic_row(GtkGrid *grid,
                      const gchar *title,
                      GtkWidget *value,
                      gint row)
{
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_valign(label, GTK_ALIGN_START);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_grid_attach(grid, value, 1, row, 2, 1);
}

static void
create_settings(CaptureUi *ui, const CapturePreferencesValues *preferences)
{
    ui->settings = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(ui->settings), "Advanced Capture Diagnostics");
    gtk_window_set_transient_for(GTK_WINDOW(ui->settings), GTK_WINDOW(ui->window));
    gtk_window_set_modal(GTK_WINDOW(ui->settings), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(ui->settings), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(ui->settings), 820, 740);
    g_signal_connect(ui->settings, "delete-event", G_CALLBACK(close_settings), ui);
    g_signal_connect(ui->settings, "key-press-event", G_CALLBACK(key_press), ui);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(ui->settings));
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 24);
    gtk_widget_set_size_request(grid, 720, -1);
    gtk_container_add(GTK_CONTAINER(scroll), grid);

    ui->device_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Capture device", ui->device_label, 0);
    ui->mode_diagnostic_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Capture mode", ui->mode_diagnostic_label, 1);
    ui->renderer_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Renderer / decoder", ui->renderer_label, 2);
    ui->perf_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Capture statistics", ui->perf_label, 3);
    ui->status_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Pipeline status", ui->status_label, 4);
    ui->audio_route_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Audio route", ui->audio_route_label, 5);
    ui->pipeline_error_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Latest pipeline error",
                          ui->pipeline_error_label, 6);
    ui->log_path_label = diagnostic_value_label();
    attach_diagnostic_row(GTK_GRID(grid), "Log file", ui->log_path_label, 7);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Show controls after"), 0, 8, 1, 1);
    ui->dwell_spin = gtk_spin_button_new_with_range(300, 500, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->dwell_spin),
                              preferences->panel_dwell_ms);
    gtk_grid_attach(GTK_GRID(grid), ui->dwell_spin, 1, 8, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("ms pointer dwell"), 2, 8, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Hide controls after"), 0, 9, 1, 1);
    ui->hide_delay_spin = gtk_spin_button_new_with_range(500, 1000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->hide_delay_spin),
                              preferences->panel_hide_delay_ms);
    gtk_grid_attach(GTK_GRID(grid), ui->hide_delay_spin, 1, 9, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("ms outside panel"), 2, 9, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Video scaling"), 0, 10, 1, 1);
    ui->scale_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->scale_combo),
                              "fit", "Fit entire frame");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->scale_combo),
                              "fill", "Fill screen, crop edges");
    gtk_widget_set_size_request(ui->scale_combo, 260, 48);
    gtk_grid_attach(GTK_GRID(grid), ui->scale_combo, 1, 10, 2, 1);
    g_signal_connect(ui->scale_combo, "changed",
                     G_CALLBACK(on_scaling_mode_changed), ui);

    ui->advanced_sources_check =
        gtk_check_button_new_with_label("Show internal/virtual video sources");
    gtk_grid_attach(GTK_GRID(grid), ui->advanced_sources_check, 0, 11, 3, 1);
    g_signal_connect(ui->advanced_sources_check, "toggled",
                     G_CALLBACK(on_advanced_sources_toggled), ui);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Audio input"), 0, 12, 1, 1);
    ui->audio_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(ui->audio_combo, 420, 48);
    gtk_grid_attach(GTK_GRID(grid), ui->audio_combo, 1, 12, 2, 1);
    g_signal_connect(ui->audio_combo, "changed",
                     G_CALLBACK(on_audio_selection_changed), ui);
    GtkWidget *capture_support =
        gtk_button_new_with_label("Manage Capture Support");
    gtk_widget_set_size_request(capture_support, 300, 48);
    gtk_grid_attach(GTK_GRID(grid), capture_support, 0, 13, 3, 1);
    g_signal_connect(capture_support, "clicked",
                     G_CALLBACK(capture_support_button_clicked), ui);
    g_signal_connect(ui->dwell_spin, "value-changed",
                     G_CALLBACK(on_dwell_changed), ui);
    g_signal_connect(ui->hide_delay_spin, "value-changed",
                     G_CALLBACK(on_hide_delay_changed), ui);

    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(close, 120, 48);
    gtk_box_pack_end(GTK_BOX(content), close, FALSE, FALSE, 12);
    g_signal_connect(close, "clicked", G_CALLBACK(close_settings_button), ui);
}

static void
quit_clicked(GtkButton *button, gpointer user_data)
{
    CaptureUi *ui = user_data;
    emit_action(ui, CAPTURE_UI_ACTION_QUIT, NULL, 0.0, FALSE);
    (void)button;
}

static void
create_control_panel(CaptureUi *ui, const CapturePreferencesValues *preferences)
{
    GtkWidget *panel = gtk_event_box_new();
    ui->control_panel = panel;
    gtk_widget_set_name(panel, "capture-control-panel");
    ui->edge_hint = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ui->edge_hint), TRUE);
    gtk_widget_set_name(ui->edge_hint, "capture-edge-hint");
    gtk_widget_set_size_request(ui->edge_hint, 68, 8);
    gtk_widget_set_halign(ui->edge_hint, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(ui->edge_hint, GTK_ALIGN_START);
    gtk_widget_set_margin_top(ui->edge_hint, 6);
    gtk_widget_set_tooltip_text(ui->edge_hint, "Show controls");
    gtk_widget_add_events(ui->edge_hint, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(ui->edge_hint, "enter-notify-event", G_CALLBACK(edge_hint_enter), ui);
    g_signal_connect(ui->edge_hint, "leave-notify-event", G_CALLBACK(edge_hint_leave), ui);
    gtk_widget_add_events(panel, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(panel, "enter-notify-event", G_CALLBACK(panel_enter), ui);
    g_signal_connect(panel, "leave-notify-event", G_CALLBACK(panel_leave), ui);
    gtk_widget_set_halign(panel, GTK_ALIGN_FILL);
    gtk_widget_set_valign(panel, GTK_ALIGN_START);
    gtk_widget_set_margin_start(panel, 18);
    gtk_widget_set_margin_end(panel, 18);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    GtkWidget *panel_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(panel_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(panel_scroll),
                                        GTK_SHADOW_NONE);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(panel_scroll), 220);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(panel_scroll), TRUE);
    gtk_container_add(GTK_CONTAINER(panel_scroll), outer);
    gtk_container_add(GTK_CONTAINER(panel), panel_scroll);
    GtkWidget *selectors = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(selectors), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(selectors), FALSE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(selectors), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(selectors), 5);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(selectors), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(selectors), 8);
    gtk_widget_set_hexpand(selectors, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), selectors, FALSE, TRUE, 0);
    GtkWidget *controls = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(controls), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(controls), FALSE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(controls), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(controls), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(controls), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(controls), 8);
    gtk_widget_set_hexpand(controls, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), controls, FALSE, TRUE, 0);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "#capture-control-panel { background-color: rgba(11, 15, 20, 0.94); "
        "border: 1px solid rgba(96, 165, 250, 0.30); border-radius: 16px; } "
        "#capture-control-panel label { color: #F1F5F9; } "
        "#capture-control-panel button, #capture-control-panel combobox, "
        "#capture-control-panel checkbutton { min-width: 64px; min-height: 40px; border-radius: 8px; } "
        "#capture-control-panel button:hover, #capture-control-panel combobox:hover { "
        "background-color: #1E2633; border-color: #33C3FF; } "
        "#capture-control-panel #capture-brand-title { font-size: 16px; font-weight: bold; } "
        "#capture-connection-dot.connected, #capture-connection-state.connected { color: #22C55E; } "
        "#capture-connection-dot.connecting, #capture-connection-state.connecting { color: #60A5FA; } "
        "#capture-connection-dot.disconnected, #capture-connection-state.disconnected { color: #94A3B8; } "
        "#capture-connection-dot.error, #capture-connection-state.error { color: #F87171; } "
        "#capture-edge-hint { background-color: rgba(148, 163, 184, 0.38); border-radius: 4px; } "
        "#capture-edge-hint:hover, #capture-edge-hint.active { background-color: #33C3FF; "
        "box-shadow: 0 0 8px rgba(51, 195, 255, 0.70); }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(panel),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_style_context_add_provider(gtk_widget_get_style_context(ui->edge_hint),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *logo = gtk_image_new_from_icon_name("io.github.wully616.captureviewer", GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 26);
    gtk_box_pack_start(GTK_BOX(brand), logo, FALSE, FALSE, 0);
    GtkWidget *brand_title = gtk_label_new("CaptureViewer");
    gtk_widget_set_name(brand_title, "capture-brand-title");
    gtk_box_pack_start(GTK_BOX(brand), brand_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), brand, TRUE, TRUE, 0);
    GtkWidget *connection = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui->connection_indicator = gtk_label_new("●");
    gtk_widget_set_name(ui->connection_indicator, "capture-connection-dot");
    ui->connection_status = gtk_label_new("Disconnected");
    gtk_widget_set_name(ui->connection_status, "capture-connection-state");
    gtk_box_pack_start(GTK_BOX(connection), ui->connection_indicator, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(connection), ui->connection_status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), connection, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), header, FALSE, TRUE, 4);
    GtkWidget *selector_label = gtk_label_new("Capture device");
    gtk_widget_set_halign(selector_label, GTK_ALIGN_START);
    GtkWidget *selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    ui->source_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(ui->source_combo, 230, 48);
    gtk_box_pack_start(GTK_BOX(selector), selector_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), ui->source_combo, FALSE, FALSE, 0);
    gtk_flow_box_insert(GTK_FLOW_BOX(selectors), selector, -1);
    g_signal_connect(ui->source_combo, "changed",
                     G_CALLBACK(on_source_changed), ui);
    g_signal_connect(ui->source_combo, "notify::popup-shown",
                     G_CALLBACK(mode_popup_notify), ui);

    selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    selector_label = gtk_label_new("Video interface");
    gtk_widget_set_halign(selector_label, GTK_ALIGN_START);
    ui->node_selector = selector;
    ui->node_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(ui->node_combo, 190, 48);
    gtk_box_pack_start(GTK_BOX(selector), selector_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), ui->node_combo, FALSE, FALSE, 0);
    gtk_flow_box_insert(GTK_FLOW_BOX(selectors), selector, -1);
    g_signal_connect(ui->node_combo, "changed",
                     G_CALLBACK(on_video_node_changed), ui);
    g_signal_connect(ui->node_combo, "notify::popup-shown",
                     G_CALLBACK(mode_popup_notify), ui);

    selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    selector_label = gtk_label_new("Format");
    gtk_widget_set_halign(selector_label, GTK_ALIGN_START);
    ui->format_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(ui->format_combo, 150, 48);
    gtk_box_pack_start(GTK_BOX(selector), selector_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), ui->format_combo, FALSE, FALSE, 0);
    gtk_flow_box_insert(GTK_FLOW_BOX(selectors), selector, -1);
    g_signal_connect(ui->format_combo, "changed",
                     G_CALLBACK(on_format_changed), ui);
    g_signal_connect(ui->format_combo, "notify::popup-shown",
                     G_CALLBACK(mode_popup_notify), ui);

    selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    selector_label = gtk_label_new("Resolution");
    gtk_widget_set_halign(selector_label, GTK_ALIGN_START);
    ui->resolution_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(ui->resolution_combo, 170, 48);
    gtk_box_pack_start(GTK_BOX(selector), selector_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), ui->resolution_combo, FALSE, FALSE, 0);
    gtk_flow_box_insert(GTK_FLOW_BOX(selectors), selector, -1);
    g_signal_connect(ui->resolution_combo, "changed",
                     G_CALLBACK(on_resolution_changed), ui);
    g_signal_connect(ui->resolution_combo, "notify::popup-shown",
                     G_CALLBACK(mode_popup_notify), ui);

    selector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    selector_label = gtk_label_new("Frame rate");
    gtk_widget_set_halign(selector_label, GTK_ALIGN_START);
    ui->fps_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(ui->fps_combo, 130, 48);
    gtk_box_pack_start(GTK_BOX(selector), selector_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector), ui->fps_combo, FALSE, FALSE, 0);
    gtk_flow_box_insert(GTK_FLOW_BOX(selectors), selector, -1);
    g_signal_connect(ui->fps_combo, "changed",
                     G_CALLBACK(on_frame_rate_changed), ui);
    g_signal_connect(ui->fps_combo, "notify::popup-shown",
                     G_CALLBACK(mode_popup_notify), ui);

    ui->pin_button = gtk_toggle_button_new_with_label("Pin");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->pin_button),
                                 preferences->pinned);
    gtk_button_set_label(GTK_BUTTON(ui->pin_button),
                         preferences->pinned ? "Pinned" : "Pin");
    gtk_widget_set_size_request(ui->pin_button, 72, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), ui->pin_button, -1);
    g_signal_connect(ui->pin_button, "toggled", G_CALLBACK(on_pin_toggled), ui);
    ui->audio_toggle = gtk_check_button_new_with_label("Audio");
    gtk_widget_set_size_request(ui->audio_toggle, 140, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), ui->audio_toggle, -1);
    g_signal_connect(ui->audio_toggle, "toggled", G_CALLBACK(on_audio_toggled), ui);
    GtkWidget *volume_label = gtk_label_new("Volume");
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), volume_label, -1);
    ui->volume_scale =
        gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
    gtk_widget_set_size_request(ui->volume_scale, 140, 48);
    gtk_range_set_value(GTK_RANGE(ui->volume_scale), preferences->volume);
    gtk_scale_set_draw_value(GTK_SCALE(ui->volume_scale), TRUE);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), ui->volume_scale, -1);
    gtk_widget_add_events(ui->volume_scale,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(ui->volume_scale, "button-press-event",
                     G_CALLBACK(slider_press), ui);
    g_signal_connect(ui->volume_scale, "button-release-event",
                     G_CALLBACK(slider_release), ui);
    g_signal_connect(ui->volume_scale, "value-changed",
                     G_CALLBACK(on_volume_changed), ui);
    ui->stats_button = gtk_toggle_button_new_with_label("Stats");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->stats_button),
                                 preferences->stats_visible);
    gtk_widget_set_size_request(ui->stats_button, 88, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), ui->stats_button, -1);
    g_signal_connect(ui->stats_button, "toggled",
                     G_CALLBACK(on_stats_toggled), ui);
    GtkWidget *advanced = gtk_button_new_with_label("Advanced");
    gtk_widget_set_size_request(advanced, 112, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), advanced, -1);
    g_signal_connect_swapped(advanced, "clicked", G_CALLBACK(show_settings), ui);
    GtkWidget *full = gtk_button_new_with_label("Fullscreen");
    gtk_widget_set_size_request(full, 120, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), full, -1);
    g_signal_connect_swapped(full, "clicked",
                             G_CALLBACK(capture_ui_toggle_fullscreen), ui);
    GtkWidget *quit = gtk_button_new_with_label("Quit");
    gtk_widget_set_size_request(quit, 76, 48);
    gtk_flow_box_insert(GTK_FLOW_BOX(controls), quit, -1);
    g_signal_connect(quit, "clicked", G_CALLBACK(quit_clicked), ui);
    gtk_overlay_add_overlay(GTK_OVERLAY(ui->root_overlay), ui->edge_hint);
    ui->control_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(ui->control_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(ui->control_revealer), 180);
    gtk_container_add(GTK_CONTAINER(ui->control_revealer), panel);
    gtk_overlay_add_overlay(GTK_OVERLAY(ui->root_overlay), ui->control_revealer);
    gtk_widget_set_hexpand(ui->control_revealer, TRUE);
    gtk_widget_set_halign(ui->control_revealer, GTK_ALIGN_FILL);
    gtk_widget_set_valign(ui->control_revealer, GTK_ALIGN_START);
    gtk_widget_set_halign(panel, GTK_ALIGN_FILL);
    gtk_widget_set_valign(panel, GTK_ALIGN_START);

    ui->stats_overlay_box = gtk_event_box_new();
    gtk_widget_set_halign(ui->stats_overlay_box, GTK_ALIGN_START);
    gtk_widget_set_valign(ui->stats_overlay_box, GTK_ALIGN_END);
    gtk_widget_set_hexpand(ui->stats_overlay_box, FALSE);
    gtk_widget_set_vexpand(ui->stats_overlay_box, FALSE);
    gtk_widget_set_margin_start(ui->stats_overlay_box, 22);
    gtk_widget_set_margin_end(ui->stats_overlay_box, 22);
    gtk_widget_set_margin_bottom(ui->stats_overlay_box, 22);
    GtkWidget *stats_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(stats_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(stats_scroll),
                                        GTK_SHADOW_NONE);
    gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(stats_scroll),
                                              680);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(stats_scroll),
                                               180);
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(stats_scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(stats_scroll), TRUE);
    ui->stats_overlay_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(ui->stats_overlay_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(ui->stats_overlay_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(ui->stats_overlay_label), 80);
    gtk_widget_set_hexpand(ui->stats_overlay_label, TRUE);
    gtk_widget_set_vexpand(ui->stats_overlay_label, FALSE);
    gtk_container_add(GTK_CONTAINER(stats_scroll), ui->stats_overlay_label);
    gtk_container_add(GTK_CONTAINER(ui->stats_overlay_box), stats_scroll);
    gtk_widget_show_all(stats_scroll);
    gtk_widget_set_name(ui->stats_overlay_box, "stats-overlay");
    GtkCssProvider *stats_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(stats_css,
        "#stats-overlay { background-color: rgba(12, 15, 20, 0.78); "
        "padding: 12px; border-radius: 8px; } "
        "#stats-overlay label { color: white; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(ui->stats_overlay_box),
        GTK_STYLE_PROVIDER(stats_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(stats_css);
    gtk_overlay_add_overlay(GTK_OVERLAY(ui->root_overlay), ui->stats_overlay_box);
    gtk_widget_set_no_show_all(ui->stats_overlay_box, TRUE);
    if (preferences->stats_visible)
        gtk_widget_show(ui->stats_overlay_box);
    else
        gtk_widget_hide(ui->stats_overlay_box);

    ui->status_overlay = gtk_event_box_new();
    gtk_widget_set_name(ui->status_overlay, "capture-status");
    gtk_widget_set_halign(ui->status_overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(ui->status_overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(ui->status_overlay, 680, -1);
    ui->status_overlay_label = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(ui->status_overlay_label), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(ui->status_overlay_label), TRUE);
    gtk_container_add(GTK_CONTAINER(ui->status_overlay), ui->status_overlay_label);
    gtk_widget_show(ui->status_overlay_label);
    GtkCssProvider *status_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(status_css,
        "#capture-status { background-color: rgba(12, 15, 20, 0.90); "
        "border-radius: 12px; padding: 22px; } "
        "#capture-status label { color: white; font-size: 20px; }",
        -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(ui->status_overlay),
        GTK_STYLE_PROVIDER(status_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(status_css);
    gtk_overlay_add_overlay(GTK_OVERLAY(ui->root_overlay), ui->status_overlay);
    gtk_widget_set_no_show_all(ui->status_overlay, TRUE);
    gtk_widget_hide(ui->status_overlay);
}

CaptureUi *
capture_ui_new(GtkApplication *application,
               CaptureRenderer *renderer,
               const CapturePreferencesValues *preferences,
               const CaptureUiCallbacks *callbacks,
               gpointer user_data)
{
    g_return_val_if_fail(application != NULL, NULL);
    g_return_val_if_fail(renderer != NULL, NULL);
    g_return_val_if_fail(preferences != NULL, NULL);
    CaptureUi *ui = g_new0(CaptureUi, 1);
    ui->user_data = user_data;
    if (callbacks != NULL)
        ui->callbacks = *callbacks;
    ui->pinned = preferences->pinned;
    ui->stats_visible = preferences->stats_visible;
    ui->panel_dwell_ms = preferences->panel_dwell_ms;
    ui->panel_hide_delay_ms = preferences->panel_hide_delay_ms;

    ui->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(ui->window), "CaptureViewer");
    gtk_window_set_icon_name(GTK_WINDOW(ui->window), "io.github.wully616.captureviewer");
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 1280, 720);
    gtk_widget_set_can_focus(ui->window, TRUE);
    gtk_widget_set_app_paintable(ui->window, TRUE);
    g_signal_connect(ui->window, "key-press-event", G_CALLBACK(key_press), ui);
    ui->root_overlay = gtk_overlay_new();
    GtkWidget *video_area = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(video_area), FALSE);
    gtk_widget_set_hexpand(video_area, TRUE);
    gtk_widget_set_vexpand(video_area, TRUE);
    gtk_container_add(GTK_CONTAINER(video_area),
                      capture_renderer_get_widget(renderer));
    gtk_container_add(GTK_CONTAINER(ui->root_overlay), video_area);
    gtk_container_add(GTK_CONTAINER(ui->window), ui->root_overlay);
    create_control_panel(ui, preferences);
    create_settings(ui, preferences);
    capture_renderer_connect_motion_events(renderer, window_motion, ui);
    gtk_widget_show_all(ui->window);
    GdkCursor *arrow_cursor = gdk_cursor_new_for_display(
        gtk_widget_get_display(ui->window), GDK_LEFT_PTR);
    if (arrow_cursor != NULL) {
        gdk_window_set_cursor(gtk_widget_get_window(ui->window), arrow_cursor);
        g_object_unref(arrow_cursor);
    }
    gtk_widget_hide(ui->settings);
    set_panel_revealed(ui, FALSE);
    if (ui->pinned)
        ui_show_control_panel(ui);
    return ui;
}

void
capture_ui_free(CaptureUi *ui)
{
    if (ui == NULL)
        return;
    if (ui->dwell_watch_id != 0)
        g_source_remove(ui->dwell_watch_id);
    if (ui->panel_hide_watch_id != 0)
        g_source_remove(ui->panel_hide_watch_id);
    gtk_widget_destroy(ui->window);
    ui->window = NULL;
    g_free(ui);
}

GtkWidget *
capture_ui_get_window(CaptureUi *ui)
{
    return ui != NULL ? ui->window : NULL;
}

void
capture_ui_present(CaptureUi *ui)
{
    if (ui == NULL || ui->window == NULL)
        return;
    gtk_widget_realize(ui->window);
    gtk_widget_grab_focus(ui->window);
    capture_ui_set_fullscreen(ui, TRUE);
}

static void
append_audio_source(const gchar *identity,
                    const gchar *display_name,
                    gpointer user_data)
{
    AudioSelectorContext *context = user_data;
    CaptureUi *ui = context->ui;
    if (!g_hash_table_contains(context->seen, identity)) {
        g_hash_table_add(context->seen, g_strdup(identity));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->audio_combo),
                                  identity, display_name);
    }
    if (g_strcmp0(identity, context->selected_id) == 0)
        context->selected_present = TRUE;
}

void
capture_ui_refresh_audio_sources(CaptureUi *ui,
                                 CaptureAudio *audio,
                                 const gchar *selected_id)
{
    if (ui == NULL || ui->audio_combo == NULL)
        return;
    ui->updating_controls = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui->audio_combo));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->audio_combo),
                              "auto", "Auto — match selected capture device");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->audio_combo), "none", "None");
    AudioSelectorContext context = {
        .ui = ui,
        .seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        .selected_id = selected_id,
        .selected_present =
            g_strcmp0(selected_id, "auto") == 0 ||
            g_strcmp0(selected_id, "none") == 0,
    };
    capture_audio_foreach_source(audio, append_audio_source, &context);
    if (!context.selected_present && selected_id != NULL)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->audio_combo),
                                  selected_id,
                                  "Selected audio input unavailable");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->audio_combo),
                                selected_id != NULL ? selected_id : "auto");
    g_hash_table_unref(context.seen);
    ui->updating_controls = FALSE;
}

void
capture_ui_refresh_video_sources(CaptureUi *ui,
                                 GPtrArray *devices,
                                 CaptureDevice *selected_device,
                                 CaptureVideoNode *selected_node,
                                 const CapturePreferencesValues *preferences)
{
    if (ui == NULL || ui->source_combo == NULL || preferences == NULL)
        return;
    ui->updating_controls = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui->source_combo));
    gboolean selected_present = FALSE;
    for (guint i = 0; devices != NULL && i < devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(devices, i);
        if (g_strcmp0(device->stable_id,
                      preferences->selected_video_device_id) == 0)
            selected_present = TRUE;
    }
    if (preferences->selected_video_device_id != NULL && !selected_present)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->source_combo),
                                  preferences->selected_video_device_id,
                                  "Saved source unavailable");
    for (guint i = 0; devices != NULL && i < devices->len; i++) {
        CaptureDevice *device = g_ptr_array_index(devices, i);
        gboolean selected = g_strcmp0(device->stable_id,
                                      preferences->selected_video_device_id) == 0;
        if (device->usb_sysfs == NULL &&
            !preferences->include_advanced_sources && !selected)
            continue;
        gchar *label = device->usb_sysfs != NULL
            ? g_strdup(device->display_name)
            : g_strdup_printf("%s (%s · Internal/virtual)",
                device->display_name != NULL ? device->display_name : "Video source",
                device->nodes->len > 0
                    ? ((CaptureVideoNode *)g_ptr_array_index(device->nodes, 0))->driver
                    : "non-USB");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->source_combo),
                                  device->stable_id, label);
        g_free(label);
    }
    if (preferences->selected_video_device_id != NULL)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->source_combo),
                                    preferences->selected_video_device_id);
    else
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->source_combo), -1);

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui->node_combo));
    GHashTable *seen_interfaces =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint interface_count = 0;
    for (guint i = 0; selected_device != NULL && i < selected_device->nodes->len; i++) {
        CaptureVideoNode *node = g_ptr_array_index(selected_device->nodes, i);
        if (node->interface_sysfs == NULL ||
            g_hash_table_contains(seen_interfaces, node->interface_sysfs))
            continue;
        g_hash_table_add(seen_interfaces, g_strdup(node->interface_sysfs));
        gchar *label = g_strdup_printf("%s%s%s",
            node->card_name != NULL ? node->card_name : "Video interface",
            node->driver != NULL ? " · " : "",
            node->driver != NULL ? node->driver : "");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->node_combo),
                                  node->interface_sysfs, label);
        g_free(label);
        interface_count++;
    }
    const gchar *node_id = selected_node != NULL
        ? selected_node->interface_sysfs : preferences->selected_node_interface;
    if (node_id != NULL)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->node_combo), node_id);
    else if (interface_count > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->node_combo), 0);
    gtk_widget_set_visible(ui->node_selector, interface_count > 1);
    gtk_widget_set_sensitive(ui->node_combo, interface_count > 1);
    g_hash_table_unref(seen_interfaces);
    ui->updating_controls = FALSE;
}

void
capture_ui_refresh_mode_selectors(CaptureUi *ui,
                                  GPtrArray *modes,
                                  guint current_mode)
{
    if (ui == NULL || ui->format_combo == NULL)
        return;
    ui->updating_controls = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui->format_combo));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui->resolution_combo));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui->fps_combo));
    gboolean have_mode = modes != NULL && modes->len > 0 &&
        current_mode < modes->len;
    if (have_mode) {
        CaptureMode *current = g_ptr_array_index(modes, current_mode);
        GHashTable *formats = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GHashTable *resolutions =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GHashTable *rates = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        gchar *format_id = g_strdup_printf("%08x", current->fourcc);
        gchar *resolution_id =
            g_strdup_printf("%u:%u", current->width, current->height);
        gchar *current_key = capture_mode_key(current);
        for (guint i = 0; i < modes->len; i++) {
            CaptureMode *mode = g_ptr_array_index(modes, i);
            gchar *candidate_format = g_strdup_printf("%08x", mode->fourcc);
            if (!g_hash_table_contains(formats, candidate_format)) {
                g_hash_table_add(formats, g_strdup(candidate_format));
                const gchar *format_name = mode->format_name != NULL
                    ? mode->format_name : candidate_format;
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->format_combo),
                                          candidate_format, format_name);
            }
            g_free(candidate_format);
            if (mode->fourcc != current->fourcc)
                continue;
            gchar *candidate_resolution =
                g_strdup_printf("%u:%u", mode->width, mode->height);
            if (!g_hash_table_contains(resolutions, candidate_resolution)) {
                g_hash_table_add(resolutions, g_strdup(candidate_resolution));
                gchar *resolution_label =
                    g_strdup_printf("%u × %u", mode->width, mode->height);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->resolution_combo),
                                          candidate_resolution, resolution_label);
                g_free(resolution_label);
            }
            g_free(candidate_resolution);
            if (mode->width != current->width || mode->height != current->height)
                continue;
            gchar *rate_key = capture_mode_key(mode);
            if (!g_hash_table_contains(rates, rate_key)) {
                g_hash_table_add(rates, g_strdup(rate_key));
                gchar *rate_label = mode->fps_d == 1
                    ? g_strdup_printf("%u fps", mode->fps_n)
                    : g_strdup_printf("%u/%u fps", mode->fps_n, mode->fps_d);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->fps_combo),
                                          rate_key, rate_label);
                g_free(rate_label);
            }
            g_free(rate_key);
        }
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->format_combo), format_id);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->resolution_combo), resolution_id);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->fps_combo), current_key);
        g_free(format_id);
        g_free(resolution_id);
        g_free(current_key);
        g_hash_table_unref(formats);
        g_hash_table_unref(resolutions);
        g_hash_table_unref(rates);
    }
    gtk_widget_set_sensitive(ui->format_combo, have_mode);
    gtk_widget_set_sensitive(ui->resolution_combo, have_mode);
    gtk_widget_set_sensitive(ui->fps_combo, have_mode);
    ui->updating_controls = FALSE;
}

static const gchar *
video_buffer_type_name(enum v4l2_buf_type type)
{
    switch (type) {
    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
        return "VIDEO_CAPTURE";
    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
        return "VIDEO_CAPTURE_MPLANE";
    default:
        return "unknown";
    }
}

static gboolean
is_mjpeg_mode(const CaptureMode *mode)
{
    return mode->fourcc == V4L2_PIX_FMT_MJPEG || mode->fourcc == V4L2_PIX_FMT_JPEG;
}

static const gchar *
selected_decoder_description(const CaptureMode *mode)
{
    if (mode == NULL)
        return "unavailable";
    if (!mode->compressed)
        return "none (raw capture)";
    return is_mjpeg_mode(mode) ? "jpegdec" : "decodebin (dynamic selection)";
}

static void
append_renderer_timing_summary(GString *text, const gchar *name,
                               const CaptureRendererTimingSummary *summary)
{
    g_string_append_printf(text, "%s ", name);
    if (summary->sample_count == 0) {
        g_string_append(text, "n/a");
        return;
    }
    g_string_append_printf(text, "%.3f/%.3f/%.3f ms (n=%u)",
                           summary->average_ms, summary->p50_ms,
                           summary->p95_ms, summary->sample_count);
}

static gchar *
format_renderer_timing_stats(const CaptureRendererTimingStats *stats)
{
    GString *text = g_string_new(
        "Software-only timing; source-pad buffer arrival → OpenGL render "
        "callback entry; PTS is frame identity only; avg/p50/p95 "
        "(rolling 256 samples):\n");
    append_renderer_timing_summary(text, "Source→decoded",
                                   &stats->source_to_decoded);
    g_string_append(text, " · ");
    append_renderer_timing_summary(text, "Decoded→appsink",
                                   &stats->decoded_to_appsink);
    g_string_append_c(text, '\n');
    append_renderer_timing_summary(text, "Appsink→dispatch",
                                   &stats->appsink_to_dispatch);
    g_string_append(text, " · ");
    append_renderer_timing_summary(text, "Dispatch→GL",
                                   &stats->dispatch_to_gl);
    g_string_append_c(text, '\n');
    append_renderer_timing_summary(text, "Appsink→GL",
                                   &stats->appsink_to_gl);
    g_string_append(text, " · ");
    append_renderer_timing_summary(text, "Source→GL",
                                   &stats->source_to_gl);
    return g_string_free(text, FALSE);
}

static void
update_connection_status(CaptureUi *ui, const CaptureUiModel *model)
{
    if (ui->connection_status == NULL || ui->connection_indicator == NULL)
        return;
    const gchar *text;
    const gchar *state;
    if (model->pipeline_error != NULL && model->pipeline_error[0] != '\0') {
        text = "Error";
        state = "error";
    } else if (model->video_device != NULL && model->pipeline_running &&
               !model->waiting_for_frames) {
        text = "Connected";
        state = "connected";
    } else if (model->video_device != NULL) {
        text = "Connecting";
        state = "connecting";
    } else {
        text = "Disconnected";
        state = "disconnected";
    }
    GtkStyleContext *label_style =
        gtk_widget_get_style_context(ui->connection_status);
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(ui->connection_status)), text) == 0 &&
        gtk_style_context_has_class(label_style, state))
        return;
    gtk_label_set_text(GTK_LABEL(ui->connection_status), text);
    GtkWidget *widgets[] = { ui->connection_indicator, ui->connection_status };
    static const gchar *const states[] = {
        "connected", "connecting", "disconnected", "error"
    };
    for (guint i = 0; i < G_N_ELEMENTS(widgets); i++) {
        GtkStyleContext *style = gtk_widget_get_style_context(widgets[i]);
        for (guint j = 0; j < G_N_ELEMENTS(states); j++)
            gtk_style_context_remove_class(style, states[j]);
        gtk_style_context_add_class(style, state);
    }
}

void
capture_ui_update(CaptureUi *ui, const CaptureUiModel *model)
{
    if (ui == NULL || ui->window == NULL || model == NULL ||
        model->preferences == NULL)
        return;
    const CapturePreferencesValues *preferences = model->preferences;
    gboolean pipeline_running = model->pipeline_running;
    CapturePipelineStats stats = model->pipeline_stats;
    const gchar *pipeline_error = model->pipeline_error;
    gboolean waiting_for_frames = model->waiting_for_frames;
    update_connection_status(ui, model);
    gboolean show_diagnostics =
        ui->settings != NULL && gtk_widget_get_visible(ui->settings);
    CaptureMode *mode = model->modes != NULL &&
        model->current_mode < model->modes->len
        ? g_ptr_array_index(model->modes, model->current_mode) : NULL;
    const gchar *mode_name = mode != NULL ? mode->label : "No capture mode";

    ui->updating_controls = TRUE;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->audio_toggle)) !=
        preferences->audio_enabled)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->audio_toggle),
                                     preferences->audio_enabled);
    gtk_widget_set_sensitive(ui->audio_toggle,
        g_strcmp0(preferences->audio_selection_id, "none") != 0);
    gtk_widget_set_sensitive(ui->volume_scale,
        preferences->audio_enabled && model->audio.source != NULL);
    if (gtk_range_get_value(GTK_RANGE(ui->volume_scale)) != preferences->volume)
        gtk_range_set_value(GTK_RANGE(ui->volume_scale), preferences->volume);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->pin_button)) !=
        preferences->pinned)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->pin_button),
                                     preferences->pinned);
    gtk_button_set_label(GTK_BUTTON(ui->pin_button),
                         preferences->pinned ? "Pinned" : "Pin");
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->stats_button)) !=
        preferences->stats_visible)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->stats_button),
                                     preferences->stats_visible);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->advanced_sources_check)) !=
        preferences->include_advanced_sources)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->advanced_sources_check),
                                     preferences->include_advanced_sources);
    if ((guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui->dwell_spin)) !=
        preferences->panel_dwell_ms)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->dwell_spin),
                                  preferences->panel_dwell_ms);
    if ((guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui->hide_delay_spin)) !=
        preferences->panel_hide_delay_ms)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->hide_delay_spin),
                                  preferences->panel_hide_delay_ms);
    const gchar *scale_id = preferences->scale_mode == CAPTURE_PREFERENCES_SCALE_FILL
        ? "fill" : "fit";
    if (g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui->scale_combo)),
                  scale_id) != 0)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->scale_combo), scale_id);
    ui->updating_controls = FALSE;

    if (ui->pinned != preferences->pinned) {
        ui->pinned = preferences->pinned;
        if (ui->pinned)
            ui_show_control_panel(ui);
        else
            ui_schedule_panel_hide(ui);
    }
    if (ui->stats_visible != preferences->stats_visible) {
        ui->stats_visible = preferences->stats_visible;
        if (ui->stats_visible)
            gtk_widget_show(ui->stats_overlay_box);
        else
            gtk_widget_hide(ui->stats_overlay_box);
    }
    ui->panel_dwell_ms = preferences->panel_dwell_ms;
    ui->panel_hide_delay_ms = preferences->panel_hide_delay_ms;

    if (show_diagnostics) {
        gchar *device_text;
        if (model->video_device != NULL && model->video_node != NULL) {
            CaptureDevice *device = model->video_device;
            CaptureVideoNode *node = model->video_node;
            gchar *usb_ids = device->usb_sysfs != NULL
                ? g_strdup_printf("%04x:%04x", device->usb_vid, device->usb_pid)
                : g_strdup("not USB");
            device_text = g_strdup_printf(
                "Name: %s\nStable ID: %s\nPhysical sysfs: %s\nUSB parent: %s\n"
                "USB IDs: %s · serial: %s\nNode: %s\nV4L2 card: %s\n"
                "Driver: %s · bus: %s\nInterface sysfs: %s\n"
                "Capabilities: 0x%08x · buffer type: %s\nUsable modes: %u",
                device->display_name != NULL ? device->display_name : "Video capture device",
                device->stable_id != NULL ? device->stable_id : "unavailable",
                device->physical_sysfs != NULL ? device->physical_sysfs : "unavailable",
                device->usb_sysfs != NULL ? device->usb_sysfs : "not USB",
                usb_ids, device->serial != NULL ? device->serial : "unavailable",
                node->path != NULL ? node->path : "unavailable",
                node->card_name != NULL ? node->card_name : "unavailable",
                node->driver != NULL ? node->driver : "unavailable",
                node->bus_info != NULL ? node->bus_info : "unavailable",
                node->interface_sysfs != NULL ? node->interface_sysfs : "unavailable",
                node->capabilities, video_buffer_type_name(node->buffer_type),
                model->modes != NULL ? model->modes->len : 0);
            g_free(usb_ids);
        } else if (model->video_device != NULL) {
            device_text = g_strdup_printf(
                "Name: %s\nStable ID: %s\nPhysical sysfs: %s\n"
                "Connected, but no usable video interface is selected",
                model->video_device->display_name != NULL
                    ? model->video_device->display_name : "Video capture device",
                model->video_device->stable_id != NULL
                    ? model->video_device->stable_id : "unavailable",
                model->video_device->physical_sysfs != NULL
                    ? model->video_device->physical_sysfs : "unavailable");
        } else if (preferences->selected_video_device_id != NULL) {
            device_text = g_strdup_printf(
                "Saved video source unavailable: %s\nWaiting for reconnection or another source",
                preferences->selected_video_device_id);
        } else {
            device_text = g_strdup(
                "No video source selected. Connect a USB capture device or choose a source.");
        }
        gtk_label_set_text(GTK_LABEL(ui->device_label), device_text);

        gchar *mode_text;
        if (mode != NULL) {
            gchar *mode_key = capture_mode_key(mode);
            gchar *expected_text = capture_pipeline_mode_caps_description(mode);
            gchar *negotiated_text = model->pipeline != NULL
                ? capture_pipeline_dup_video_input_caps(model->pipeline)
                : g_strdup("element unavailable");
            mode_text = g_strdup_printf(
                "Mode: %s\nExact mode ID: %s\nExpected capture caps: %s\n"
                "Negotiated videoconvert input caps: %s",
                mode->label, mode_key, expected_text, negotiated_text);
            g_free(mode_key);
            g_free(expected_text);
            g_free(negotiated_text);
        } else {
            mode_text = g_strdup("No usable capture mode is selected");
        }
        gtk_label_set_text(GTK_LABEL(ui->mode_diagnostic_label), mode_text);
        gchar *renderer_text = g_strdup_printf("Renderer: %s\nDecoder path: %s",
            model->renderer_backend != NULL ? model->renderer_backend : "unavailable",
            selected_decoder_description(mode));
        gtk_label_set_text(GTK_LABEL(ui->renderer_label), renderer_text);
        g_free(device_text);
        g_free(mode_text);
        g_free(renderer_text);
    }

    gchar *overlay_text = NULL;
    gchar *perf = NULL;
    gchar *latency_text =
        format_renderer_timing_stats(&model->renderer_timing_stats);
    gchar *audio_stats = NULL;
    if (show_diagnostics) {
        perf = g_strdup_printf("FPS %.1f avg %.1f · dropped %" G_GUINT64_FORMAT
            " · queue %" G_GUINT64_FORMAT " · CPU %.1f%%\n%s",
            stats.current_fps, stats.average_fps, stats.frames_dropped,
            stats.queue_level, stats.cpu_percent, latency_text);
        audio_stats = model->audio.source != NULL && preferences->audio_enabled &&
            model->pipeline_running && model->pipeline_has_audio_source &&
            stats.audio_source_latency_us >= 0 && stats.audio_source_buffer_us >= 0
            ? g_strdup_printf("Source timing: %.1f ms latency / %.1f ms buffer",
                stats.audio_source_latency_us / 1000.0,
                stats.audio_source_buffer_us / 1000.0)
            : g_strdup("Source timing: unavailable");
        gtk_label_set_text(GTK_LABEL(ui->perf_label), perf);

        gchar *sink_display =
            model->audio.default_output_name != NULL &&
                *model->audio.default_output_name != '\0'
            ? (model->audio.default_output_id != NULL &&
                    *model->audio.default_output_id != '\0'
                ? g_strdup_printf("%s (%s)", model->audio.default_output_name,
                                  model->audio.default_output_id)
                : g_strdup(model->audio.default_output_name))
            : (model->audio.default_output_id != NULL &&
                    *model->audio.default_output_id != '\0'
                ? g_strdup(model->audio.default_output_id) : NULL);
        const gchar *policy = preferences->audio_selection_id == NULL ||
            g_str_equal(preferences->audio_selection_id, "auto") ? "Auto" :
            g_str_equal(preferences->audio_selection_id, "none") ? "None" : "Explicit source";
        const gchar *input_name = model->audio.source_display_name != NULL
            ? model->audio.source_display_name : "No audio input";
        const gchar *input_identity = model->audio.source_id != NULL
            ? model->audio.source_id : "unavailable";
        gchar *route_text;
        if (!preferences->audio_enabled)
            route_text = g_strdup("Audio is disabled");
        else if (model->audio.source == NULL)
            route_text = g_strdup(model->audio.source_status != NULL
                ? model->audio.source_status : "No selected audio input is available");
        else if (!model->pipeline_running || !model->pipeline_has_audio_source)
            route_text = g_strdup(
                "Input detected; playback inactive because capture pipeline is stopped");
        else if (sink_display != NULL)
            route_text = g_strdup_printf("PulseAudio default output: %s", sink_display);
        else
            route_text = g_strdup("PulseAudio default output unavailable");
        gchar *audio_route = g_strdup_printf(
            "Policy: %s\nInput: %s\nInput identity: %s\nStatus: %s\n%s\n%s",
            policy, input_name, input_identity,
            model->audio.source_status != NULL ? model->audio.source_status
                                               : "No audio status available",
            route_text, audio_stats);
        gtk_label_set_text(GTK_LABEL(ui->audio_route_label), audio_route);
        const gchar *status_base;
        if (pipeline_running && waiting_for_frames)
            status_base = "Pipeline active; waiting for video frames (signal status unavailable)";
        else if (pipeline_running)
            status_base = "Capture pipeline active";
        else if (pipeline_error != NULL)
            status_base = "Capture pipeline stopped after an error";
        else if (model->video_device == NULL &&
                 preferences->selected_video_device_id != NULL)
            status_base = "Saved video source is unavailable";
        else if (model->video_device == NULL)
            status_base = "No video capture source selected";
        else if (model->modes == NULL || model->modes->len == 0)
            status_base = "No usable video mode selected";
        else
            status_base = "Capture pipeline is not running";
        gint64 remaining_us = model->retry_after_us - g_get_monotonic_time();
        gchar *status_text = remaining_us > 0
            ? g_strdup_printf("%s; retry scheduled in %" G_GINT64_FORMAT " ms",
                              status_base, (remaining_us + 999) / 1000)
            : g_strdup(status_base);
        gtk_label_set_text(GTK_LABEL(ui->status_label), status_text);
        gtk_label_set_text(GTK_LABEL(ui->pipeline_error_label),
            pipeline_error != NULL ? pipeline_error : "No pipeline error recorded");
        gtk_label_set_text(GTK_LABEL(ui->log_path_label),
            model->log_path != NULL ? model->log_path : "Log path unavailable");
        g_free(sink_display);
        g_free(route_text);
        g_free(audio_route);
        g_free(status_text);
    }

    if (ui->stats_overlay_label != NULL) {
        overlay_text = g_strdup_printf(
            "%s\nFPS %.1f avg %.1f · dropped %" G_GUINT64_FORMAT
            " · CPU %.1f%%\n%s",
            mode_name, stats.current_fps, stats.average_fps,
            stats.frames_dropped, stats.cpu_percent, latency_text);
        gtk_label_set_text(GTK_LABEL(ui->stats_overlay_label), overlay_text);
    }

    if (ui->status_overlay_label != NULL && ui->status_overlay != NULL) {
        const gchar *message = NULL;
        if (model->video_device == NULL &&
            preferences->selected_video_device_id != NULL)
            message = "Saved source unavailable\nReconnect it or choose another source.";
        else if (model->video_device == NULL)
            message = "Waiting for a USB video capture source";
        else if (pipeline_error != NULL)
            message = "Capture error\nOpen Advanced for the error, debug trace, device, and caps.";
        else if (model->modes == NULL || model->modes->len == 0)
            message = "No usable capture mode\nOpen Advanced for decoder and caps details.";
        else if (waiting_for_frames)
            message = "Waiting for video frames\nCapture signal status is unavailable.";
        else if (!pipeline_running)
            message = "Capture device connected\nWaiting for capture to start.";
        gboolean show_status = message != NULL;
        if (show_status &&
            g_strcmp0(gtk_label_get_text(GTK_LABEL(ui->status_overlay_label)), message) != 0)
            gtk_label_set_text(GTK_LABEL(ui->status_overlay_label), message);
        if (show_status && !gtk_widget_get_visible(ui->status_overlay))
            gtk_widget_show(ui->status_overlay);
        else if (!show_status && gtk_widget_get_visible(ui->status_overlay))
            gtk_widget_hide(ui->status_overlay);
    }

    g_free(overlay_text);
    g_free(perf);
    g_free(audio_stats);
    g_free(latency_text);
}
