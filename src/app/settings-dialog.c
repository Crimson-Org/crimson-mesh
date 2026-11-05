#include <adwaita.h>

#include "lang/en.h"
#include "app/settings-dialog.h"
#include "app/view-options.h"

#define SPEED_STEP 10
#define DIALOG_WIDTH 520
#define PERCENT_SIGN '%'


static void change_speed(AdwSpinRow *row, GParamSpec *property, int *speed) {
	*speed = (int)adw_spin_row_get_value(row);
	save_view_options();
}


/* Speeds read as percentages of the default, such as 150% */
static gboolean show_percent(AdwSpinRow *row) {
	g_autofree char *text = g_strdup_printf(TEXT_PERCENT, (int)adw_spin_row_get_value(row));
	gtk_editable_set_text(GTK_EDITABLE(row), text);
	return TRUE;
}


/* A typed speed may end with a percent sign, and any other text keeps the old speed */
static int read_percent(AdwSpinRow *row, double *value) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(row));
	char *end = NULL;
	double typed = g_ascii_strtod(text, &end);
	gboolean is_number = end != text && (*end == 0 || (*end == PERCENT_SIGN && end[1] == 0));
	*value = is_number ? typed : adw_spin_row_get_value(row);
	return TRUE;
}


static GtkWidget *create_speed_row(const char *title, const char *subtitle, int *speed) {
	GtkWidget *row = adw_spin_row_new_with_range(MINIMUM_SPEED, MAXIMUM_SPEED, SPEED_STEP);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
	/* A row made with a range takes only digits, which would drop the percent sign */
	adw_spin_row_set_numeric(ADW_SPIN_ROW(row), FALSE);
	g_signal_connect(row, "output", G_CALLBACK(show_percent), NULL);
	g_signal_connect(row, "input", G_CALLBACK(read_percent), NULL);
	adw_spin_row_set_value(ADW_SPIN_ROW(row), *speed);
	g_signal_connect(row, "notify::value", G_CALLBACK(change_speed), speed);
	return row;
}


void show_settings(GtkWidget *parent) {
	ViewOptions *options = get_view_options();
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), TEXT_CAMERA_GROUP);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), create_speed_row(TEXT_TURN_SPEED, TEXT_TURN_SPEED_DETAIL,
		&options->mouse_speed));
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), create_speed_row(TEXT_FLY_SPEED, TEXT_FLY_SPEED_DETAIL,
		&options->fly_speed));
	GtkWidget *page = adw_preferences_page_new();
	gtk_widget_set_size_request(page, DIALOG_WIDTH, -1);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
	AdwDialog *dialog = adw_preferences_dialog_new();
	adw_dialog_set_title(dialog, TEXT_SETTINGS);
	/* The two rows fit without scrolling, so the dialog takes their height instead of its default one */
	adw_dialog_set_follows_content_size(dialog, TRUE);
	adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));
	adw_dialog_present(dialog, parent);
}
