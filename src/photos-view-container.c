/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2012 – 2016 Red Hat, Inc.
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

/* Based on code from:
 *   + Documents
 */


#include "config.h"
#include <libgd/gd.h>

#include "photos-empty-results-box.h"
#include "photos-enums.h"
#include "photos-error-box.h"
#include "photos-item-manager.h"
#include "photos-offset-favorites-controller.h"
#include "photos-offset-collections-controller.h"
#include "photos-offset-overview-controller.h"
#include "photos-offset-search-controller.h"
#include "photos-remote-display-manager.h"
#include "photos-search-context.h"
#include "photos-selection-controller.h"
#include "photos-tracker-collections-controller.h"
#include "photos-tracker-favorites-controller.h"
#include "photos-tracker-overview-controller.h"
#include "photos-tracker-search-controller.h"
#include "photos-utils.h"
#include "photos-view-container.h"
#include "photos-view-model.h"
#include "photos-tracker-import-controller.h"
#include "photos-offset-import-controller.h"


struct _PhotosViewContainer
{
  GtkStack parent_instance;
  GdMainView *view;
  GtkListStore *model;
  GtkTreePath *current_path;
  GtkWidget *error_box;
  GtkWidget *no_results;
  PhotosBaseManager *item_mngr;
  PhotosModeController *mode_cntrlr;
  PhotosOffsetController *offset_cntrlr;
  PhotosRemoteDisplayManager *remote_mngr;
  PhotosSelectionController *sel_cntrlr;
  PhotosTrackerController *trk_cntrlr;
  PhotosWindowMode mode;
  gboolean disposed;
  gchar *name;
};

struct _PhotosViewContainerClass
{
  GtkStackClass parent_class;
};

enum
{
  PROP_0,
  PROP_MODE,
  PROP_NAME
};


G_DEFINE_TYPE (PhotosViewContainer, photos_view_container, GTK_TYPE_STACK);


static void
photos_view_container_edge_reached (PhotosViewContainer *self, GtkPositionType pos)
{
  if (pos == GTK_POS_BOTTOM)
    photos_offset_controller_increase_offset (self->offset_cntrlr);
}


static void
photos_view_container_connect_view (PhotosViewContainer *self)
{
  g_signal_connect_swapped (self->view, "edge-reached", G_CALLBACK (photos_view_container_edge_reached), self);
}


static void
photos_view_container_count_changed (PhotosViewContainer *self, gint count)
{
  if (count == 0)
    gtk_stack_set_visible_child_name (GTK_STACK (self), "no-results");
  else
    gtk_stack_set_visible_child_name (GTK_STACK (self), "view");
}


static void
photos_view_container_disconnect_view (PhotosViewContainer *self)
{
  g_signal_handlers_disconnect_by_func (self->view, photos_view_container_edge_reached, self);
}


static void
photos_view_container_item_activated (PhotosViewContainer *self, const gchar * id, const GtkTreePath *path)
{
  GObject *object;

  g_clear_pointer (&self->current_path, (GDestroyNotify) gtk_tree_path_free);

  self->current_path = gtk_tree_path_copy (path);
  object = photos_base_manager_get_object_by_id (self->item_mngr, id);

  if (!photos_base_item_is_collection (PHOTOS_BASE_ITEM (object)) &&
      photos_remote_display_manager_is_active (self->remote_mngr))
    photos_remote_display_manager_render (self->remote_mngr, PHOTOS_BASE_ITEM (object));
  else
    photos_base_manager_set_active_object (self->item_mngr, object);
}


static void
photos_view_container_set_error (PhotosViewContainer *self, const gchar *primary, const gchar *secondary)
{
  photos_error_box_update (PHOTOS_ERROR_BOX (self->error_box), primary, secondary);
  gtk_stack_set_visible_child_name (GTK_STACK (self), "error");
}


static void
photos_view_container_query_error (PhotosViewContainer *self, const gchar *primary, const gchar *secondary)
{
  photos_view_container_set_error (self, primary, secondary);
}


static void
photos_view_container_query_status_changed (PhotosViewContainer *self, gboolean query_status)
{
  if (!query_status)
    {
      gd_main_view_set_model (self->view, GTK_TREE_MODEL (self->model));
      photos_selection_controller_freeze_selection (self->sel_cntrlr, FALSE);
      /* TODO: update selection */
    }
  else
    {
      photos_selection_controller_freeze_selection (self->sel_cntrlr, TRUE);
      gd_main_view_set_model (self->view, NULL);
    }
}


static void
photos_view_container_select_all (PhotosViewContainer *self)
{
  PhotosWindowMode mode;

  mode = photos_mode_controller_get_window_mode (self->mode_cntrlr);
  if (self->mode != mode)
    return;

  photos_selection_controller_set_selection_mode (self->sel_cntrlr, TRUE);
  gd_main_view_select_all (self->view);
}


static void
photos_view_container_select_none (PhotosViewContainer *self)
{
  PhotosWindowMode mode;

  mode = photos_mode_controller_get_window_mode (self->mode_cntrlr);
  if (self->mode != mode)
    return;

  gd_main_view_unselect_all (self->view);
}


static void
photos_view_container_selection_mode_request (PhotosViewContainer *self)
{
  photos_selection_controller_set_selection_mode (self->sel_cntrlr, TRUE);
}


static void
photos_view_container_set_selection_mode (PhotosViewContainer *self, gboolean selection_mode)
{
  PhotosWindowMode window_mode;

  window_mode = photos_mode_controller_get_window_mode (self->mode_cntrlr);
  if (self->mode != window_mode)
    return;

  gd_main_view_set_selection_mode (self->view, selection_mode);
}


static void
photos_view_container_view_selection_changed (PhotosViewContainer *self)
{
  GList *selected_urns;
  GList *selection;

  selection = gd_main_view_get_selection (self->view);
  selected_urns = photos_utils_get_urns_from_paths (selection, GTK_TREE_MODEL (self->model));
  photos_selection_controller_set_selection (self->sel_cntrlr, selected_urns);

  g_list_free_full (selected_urns, g_free);
  g_list_free_full (selection, (GDestroyNotify) gtk_tree_path_free);
}


static void
photos_view_container_window_mode_changed (PhotosViewContainer *self,
                                           PhotosWindowMode mode,
                                           PhotosWindowMode old_mode)
{
  photos_view_container_disconnect_view (self);

  if (mode == PHOTOS_WINDOW_MODE_COLLECTIONS
      || mode == PHOTOS_WINDOW_MODE_FAVORITES
      || mode == PHOTOS_WINDOW_MODE_OVERVIEW
      || mode == PHOTOS_WINDOW_MODE_SEARCH
      || mode == PHOTOS_WINDOW_MODE_IMPORT)
    photos_view_container_connect_view (self);
}


static void
photos_view_container_constructed (GObject *object)
{
  PhotosViewContainer *self = PHOTOS_VIEW_CONTAINER (object);
  AtkObject *accessible;
  GAction *action;
  GApplication *app;
  GtkWidget *generic_view;
  GtkWidget *grid;
  PhotosSearchContextState *state;
  gboolean selection_mode;
  gboolean status;
  gint size;

  G_OBJECT_CLASS (photos_view_container_parent_class)->constructed (object);

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  accessible = gtk_widget_get_accessible (GTK_WIDGET (self));
  if (accessible != NULL)
    atk_object_set_name (accessible, self->name);

  self->model = photos_view_model_new (self->mode);

  grid = gtk_grid_new ();
  gtk_orientable_set_orientation (GTK_ORIENTABLE (grid), GTK_ORIENTATION_VERTICAL);
  gtk_stack_add_named (GTK_STACK (self), grid, "view");

  self->no_results = photos_empty_results_box_new (self->mode);
  gtk_stack_add_named (GTK_STACK (self), self->no_results, "no-results");

  self->error_box = photos_error_box_new ();
  gtk_stack_add_named (GTK_STACK (self), self->error_box, "error");

  self->view = gd_main_view_new (GD_MAIN_VIEW_ICON);
  generic_view = gd_main_view_get_generic_view (self->view);
  size = photos_utils_get_icon_size_unscaled ();
  gtk_icon_view_set_item_width (GTK_ICON_VIEW (generic_view), size + 24);
  gtk_container_add (GTK_CONTAINER (grid), GTK_WIDGET (self->view));

  gtk_widget_show_all (GTK_WIDGET (self));

  gtk_stack_set_visible_child_full (GTK_STACK (self), "view", GTK_STACK_TRANSITION_TYPE_NONE);

  g_signal_connect_swapped (self->view, "item-activated", G_CALLBACK (photos_view_container_item_activated), self);
  g_signal_connect_swapped (self->view,
                            "selection-mode-request",
                            G_CALLBACK (photos_view_container_selection_mode_request),
                            self);
  g_signal_connect_swapped (self->view,
                            "view-selection-changed",
                            G_CALLBACK (photos_view_container_view_selection_changed),
                            self);

  self->item_mngr = g_object_ref (state->item_mngr);

  self->sel_cntrlr = photos_selection_controller_dup_singleton ();
  g_signal_connect_object (self->sel_cntrlr,
                           "selection-mode-changed",
                           G_CALLBACK (photos_view_container_set_selection_mode),
                           self,
                           G_CONNECT_SWAPPED);

  self->mode_cntrlr = g_object_ref (state->mode_cntrlr);
  g_signal_connect_object (self->mode_cntrlr,
                           "window-mode-changed",
                           G_CALLBACK (photos_view_container_window_mode_changed),
                           self,
                           G_CONNECT_SWAPPED);

  self->remote_mngr = photos_remote_display_manager_dup_singleton ();

  switch (self->mode)
    {
    case PHOTOS_WINDOW_MODE_COLLECTIONS:
      self->trk_cntrlr = photos_tracker_collections_controller_dup_singleton ();
      self->offset_cntrlr = photos_offset_collections_controller_dup_singleton ();
      break;

    case PHOTOS_WINDOW_MODE_FAVORITES:
      self->trk_cntrlr = photos_tracker_favorites_controller_dup_singleton ();
      self->offset_cntrlr = photos_offset_favorites_controller_dup_singleton ();
      break;

    case PHOTOS_WINDOW_MODE_OVERVIEW:
      self->trk_cntrlr = photos_tracker_overview_controller_dup_singleton ();
      self->offset_cntrlr = photos_offset_overview_controller_dup_singleton ();
      break;

    case PHOTOS_WINDOW_MODE_SEARCH:
      self->trk_cntrlr = photos_tracker_search_controller_dup_singleton ();
      self->offset_cntrlr = photos_offset_search_controller_dup_singleton ();
      break;

      case PHOTOS_WINDOW_MODE_IMPORT:
        self->trk_cntrlr = photos_tracker_import_controller_dup_singleton ();
        self->offset_cntrlr = photos_offset_import_controller_dup_singleton ();
        break;

    case PHOTOS_WINDOW_MODE_NONE:
    case PHOTOS_WINDOW_MODE_EDIT:
    case PHOTOS_WINDOW_MODE_PREVIEW:
    default:
      g_assert_not_reached ();
      break;
    }

  action = g_action_map_lookup_action (G_ACTION_MAP (app), "select-all");
  g_signal_connect_object (action,
                           "activate",
                           G_CALLBACK (photos_view_container_select_all),
                           self,
                           G_CONNECT_SWAPPED);

  action = g_action_map_lookup_action (G_ACTION_MAP (app), "select-none");
  g_signal_connect_object (action,
                           "activate",
                           G_CALLBACK (photos_view_container_select_none),
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->offset_cntrlr,
                           "count-changed",
                           G_CALLBACK (photos_view_container_count_changed),
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->trk_cntrlr,
                           "query-error",
                           G_CALLBACK (photos_view_container_query_error),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->trk_cntrlr,
                           "query-status-changed",
                           G_CALLBACK (photos_view_container_query_status_changed),
                           self,
                           G_CONNECT_SWAPPED);

  selection_mode = photos_selection_controller_get_selection_mode (self->sel_cntrlr);
  photos_view_container_set_selection_mode (self, selection_mode);

  photos_tracker_controller_start (self->trk_cntrlr);

  status = photos_tracker_controller_get_query_status (self->trk_cntrlr);
  photos_view_container_query_status_changed (self, status);
}


static void
photos_view_container_dispose (GObject *object)
{
  PhotosViewContainer *self = PHOTOS_VIEW_CONTAINER (object);

  if (!self->disposed)
    {
      photos_view_container_disconnect_view (self);
      self->disposed = TRUE;
    }

  g_clear_object (&self->model);
  g_clear_object (&self->item_mngr);
  g_clear_object (&self->mode_cntrlr);
  g_clear_object (&self->offset_cntrlr);
  g_clear_object (&self->remote_mngr);
  g_clear_object (&self->sel_cntrlr);
  g_clear_object (&self->trk_cntrlr);

  G_OBJECT_CLASS (photos_view_container_parent_class)->dispose (object);
}


static void
photos_view_container_finalize (GObject *object)
{
  PhotosViewContainer *self = PHOTOS_VIEW_CONTAINER (object);

  g_clear_pointer (&self->current_path, (GDestroyNotify) gtk_tree_path_free);
  g_free (self->name);

  G_OBJECT_CLASS (photos_view_container_parent_class)->finalize (object);
}


static void
photos_view_container_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  PhotosViewContainer *self = PHOTOS_VIEW_CONTAINER (object);

  switch (prop_id)
    {
    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
photos_view_container_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  PhotosViewContainer *self = PHOTOS_VIEW_CONTAINER (object);

  switch (prop_id)
    {
    case PROP_MODE:
      self->mode = (PhotosWindowMode) g_value_get_enum (value);
      break;

    case PROP_NAME:
      self->name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
photos_view_container_init (PhotosViewContainer *self)
{
}


static void
photos_view_container_class_init (PhotosViewContainerClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->constructed = photos_view_container_constructed;
  object_class->dispose = photos_view_container_dispose;
  object_class->finalize = photos_view_container_finalize;
  object_class->get_property = photos_view_container_get_property;
  object_class->set_property = photos_view_container_set_property;

  g_object_class_install_property (object_class,
                                   PROP_MODE,
                                   g_param_spec_enum ("mode",
                                                      "PhotosWindowMode enum",
                                                      "The mode for which the widget is a view",
                                                      PHOTOS_TYPE_WINDOW_MODE,
                                                      PHOTOS_WINDOW_MODE_NONE,
                                                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

  g_object_class_install_property (object_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Title",
                                                        "The string used to identify this view",
                                                        NULL,
                                                        G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE));
}


GtkWidget *
photos_view_container_new (PhotosWindowMode mode, const gchar *name)
{
  return g_object_new (PHOTOS_TYPE_VIEW_CONTAINER,
                       "homogeneous", TRUE,
                       "mode", mode,
                       "name", name,
                       "transition-type", GTK_STACK_TRANSITION_TYPE_CROSSFADE,
                       NULL);
}


void
photos_view_container_activate_result (PhotosViewContainer *self)
{
  GtkTreeIter iter;

  if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (self->model), &iter))
    {
      GtkTreePath *path;
      GtkWidget *generic_view;

      path = gtk_tree_model_get_path (GTK_TREE_MODEL (self->model), &iter);
      generic_view = gd_main_view_get_generic_view (self->view);
      gtk_icon_view_item_activated (GTK_ICON_VIEW (generic_view), path);
      gtk_tree_path_free (path);
    }
}


GtkTreePath *
photos_view_container_get_current_path (PhotosViewContainer *self)
{
  return self->current_path;
}


GtkListStore *
photos_view_container_get_model (PhotosViewContainer *self)
{
  return self->model;
}


const gchar *
photos_view_container_get_name (PhotosViewContainer *self)
{
  return self->name;
}
