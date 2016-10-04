/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2014 Pranav Kant
 * Copyright © 2014 – 2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */


#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <libtracker-control/tracker-control.h>


#include "photos-base-item.h"
#include "photos-base-manager.h"
#include "photos-import-notification.h"
#include "photos-icons.h"
#include "photos-notification-manager.h"
#include "photos-search-context.h"
#include "photos-item-manager.h"
#include "photos-application.h"


struct _PhotosImportNotification {
	GtkGrid parent_instance;
	GtkWidget *ntfctn_mngr;
	PhotosBaseManager *item_mngr;
	guint timeout_id;
	GMount *mount;
	TrackerMinerManager *manager;
};

struct _PhotosImportNotificationClass {
	GtkGridClass parent_class;
};

enum {
	PROP_0,
	PROP_MOUNT,
};


G_DEFINE_TYPE (PhotosImportNotification, photos_import_notification, GTK_TYPE_GRID);


enum {
	IMPORT_TIMEOUT = 10 /* s */
};


static void
photos_import_notification_remove_timeout(PhotosImportNotification *self) {
	if (self->timeout_id != 0) {
		g_source_remove(self->timeout_id);
		self->timeout_id = 0;
	}
}


static void
photos_import_notification_destroy(PhotosImportNotification *self) {
	photos_import_notification_remove_timeout(self);
	gtk_widget_destroy(GTK_WIDGET (self));
}

static gboolean
photos_import_notification_timeout(gpointer user_data) {
	PhotosImportNotification *self = PHOTOS_IMPORT_NOTIFICATION (user_data);

	self->timeout_id = 0;
	photos_import_notification_destroy(self);
	return G_SOURCE_REMOVE;
}


static void
photos_import_notification_import_clicked(PhotosImportNotification *self) {
	GError *error;

	error = NULL;
	tracker_miner_manager_index_file(self->manager, g_mount_get_root(self->mount), &error); //TODO IMPORT replace with tracker_miner_manager_index_file_for_process - Tracker 1.10
	if (error != NULL)
	{
		g_warning ("Unable to index removable device: %s", error->message);
		g_error_free (error);
	}
	photos_import_notification_destroy(self);
}


static void
photos_import_notification_constructed(GObject *object) {
	PhotosImportNotification *self = PHOTOS_IMPORT_NOTIFICATION (object);
	GtkWidget *close;
	GtkWidget *image;
	GtkWidget *label;
	GtkWidget *import_button;
	gchar *label_title;

	G_OBJECT_CLASS (photos_import_notification_parent_class)->constructed(object);

	label_title = g_strdup_printf(_("<b>New Device Discovered</b>\n%s"),
	                              g_mount_get_name(self->mount));

	label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(label), label_title);
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_container_add(GTK_CONTAINER (self), label);

	g_free(label_title);

	import_button = gtk_button_new_with_label(_("Import"));
	gtk_actionable_set_action_name (GTK_ACTIONABLE (import_button), "app.import");
	gtk_widget_set_valign(import_button, GTK_ALIGN_CENTER);
	gtk_container_add(GTK_CONTAINER (self), import_button);
	g_signal_connect_object (import_button, "clicked", G_CALLBACK(photos_import_notification_import_clicked), self, G_CONNECT_AFTER | G_CONNECT_SWAPPED);

	image = gtk_image_new_from_icon_name(PHOTOS_ICON_WINDOW_CLOSE_SYMBOLIC, GTK_ICON_SIZE_INVALID);
	gtk_widget_set_margin_bottom(image, 2);
	gtk_widget_set_margin_top(image, 2);
	gtk_image_set_pixel_size(GTK_IMAGE (image), 16);

	close = gtk_button_new();
	gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
	gtk_widget_set_focus_on_click(close, FALSE);
	gtk_button_set_relief(GTK_BUTTON (close), GTK_RELIEF_NONE);
	gtk_button_set_image(GTK_BUTTON (close), image);
	gtk_container_add(GTK_CONTAINER (self), close);
	g_signal_connect_swapped (close, "clicked", G_CALLBACK(photos_import_notification_destroy), self);

	photos_notification_manager_add_notification(PHOTOS_NOTIFICATION_MANAGER (self->ntfctn_mngr),
	                                             GTK_WIDGET (self));

	self->timeout_id = g_timeout_add_seconds(IMPORT_TIMEOUT, photos_import_notification_timeout, self);
}


static void
photos_import_notification_dispose(GObject *object) {
	PhotosImportNotification *self = PHOTOS_IMPORT_NOTIFICATION (object);

	photos_import_notification_remove_timeout(self);

	g_clear_object (&self->ntfctn_mngr);
	g_clear_object (&self->item_mngr);

	G_OBJECT_CLASS (photos_import_notification_parent_class)->dispose(object);
}


static void
photos_import_notification_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
	PhotosImportNotification *self = PHOTOS_IMPORT_NOTIFICATION (object);

	switch (prop_id) {
		case PROP_MOUNT:
			self->mount = g_value_get_pointer(value);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}


static void
photos_import_notification_init(PhotosImportNotification *self) {
	GApplication *app;
	PhotosSearchContextState *state;
	GError *error;

	app = g_application_get_default();
	state = photos_search_context_get_state(PHOTOS_SEARCH_CONTEXT (app));

	self->ntfctn_mngr = photos_notification_manager_dup_singleton();
	self->item_mngr = g_object_ref(state->item_mngr);

	error = NULL;
	self->manager = tracker_miner_manager_new_full (FALSE, &error);
	if (error != NULL)
	{
		g_warning ("Unable to create a TrackerMinerManager, indexing progress notification won't work: %s",
		           error->message);
		g_error_free (error);
		return;
	}
}


static void
photos_import_notification_class_init(PhotosImportNotificationClass *class) {
	GObjectClass *object_class = G_OBJECT_CLASS (class);

	object_class->constructed = photos_import_notification_constructed;
	object_class->dispose = photos_import_notification_dispose;
	object_class->set_property = photos_import_notification_set_property;

	g_object_class_install_property(object_class,
	                                PROP_MOUNT,
	                                g_param_spec_pointer("mount",
	                                                    "Mount",
	                                                    "A GMount representing added mount.",
	                                                    G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

}


void
photos_import_notification_new(GMount *mount) {
	g_object_new(PHOTOS_TYPE_IMPORT_NOTIFICATION,
	             "column-spacing", 12,
	             "mount", mount,
	             "orientation", GTK_ORIENTATION_HORIZONTAL,
	             NULL);
}
