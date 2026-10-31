#include <adwaita.h>

#include "lang/en.h"
#include "app/settings-dialog.h"
#include "window/view-options.h"

#define SPEED_STEP 10
#define DIALOG_WIDTH 520


static void change_speed(AdwSpinRow *row, GParamSpec *property, int *speed) {
	*speed = (int)adw_spin_row_get_value(row);
	save_view_options();
}


static GtkWidget *create_speed_row(const char *title, const char *subtitle, int *speed) {
	GtkWidget *row = adw_spin_row_new_with_range(MINIMUM_SPEED, MAXIMUM_SPEED, SPEED_STEP);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
	adw_spin_row_set_value(ADW_SPIN_ROW(row), *speed);
	g_signal_connect(row, "notify::value", G_CALLBACK(change_speed), speed);
	return row;
}


static GtkWidget *create_group(const char *title, GtkWidget *row) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
	return group;
}


void show_settings(GtkWidget *parent) {
	ViewOptions *options = get_view_options();
	GtkWidget *page = adw_preferences_page_new();
	gtk_widget_set_size_request(page, DIALOG_WIDTH, -1);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(create_group(TEXT_MOUSE_GROUP,
		create_speed_row(TEXT_MOUSE_SPEED, TEXT_MOUSE_SPEED_DETAIL, &options->mouse_speed))));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(create_group(TEXT_FLYING_GROUP,
		create_speed_row(TEXT_FLY_SPEED, TEXT_FLY_SPEED_DETAIL, &options->fly_speed))));
	AdwDialog *dialog = adw_preferences_dialog_new();
	adw_dialog_set_title(dialog, TEXT_SETTINGS);
	/* The two rows fit without scrolling, so the dialog takes their height instead of its default one */
	adw_dialog_set_follows_content_size(dialog, TRUE);
	adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));
	adw_dialog_present(dialog, parent);
}
