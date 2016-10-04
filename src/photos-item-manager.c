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

#include <string.h>

#include <gio/gio.h>
#include <glib.h>
#include <tracker-sparql.h>

#include "egg-counter.h"
#include "photos-enums.h"
#include "photos-filterable.h"
#include "photos-item-manager.h"
#include "photos-local-item.h"
#include "photos-marshalers.h"
#include "photos-query.h"
#include "photos-search-context.h"
#include "photos-single-item-job.h"
#include "photos-tracker-change-event.h"
#include "photos-tracker-change-monitor.h"
#include "photos-utils.h"


struct _PhotosItemManager
{
  PhotosBaseManager parent_instance;
  GObject *active_object;
  GCancellable *loader_cancellable;
  GHashTable *collections;
  GHashTable *hidden_items;
  GIOExtensionPoint *extension_point;
  GQueue *collection_path;
  GQueue *history;
  PhotosBaseItem *active_collection;
  PhotosBaseManager **item_mngr_chldrn;
  PhotosLoadState load_state;
  PhotosTrackerChangeMonitor *monitor;
  PhotosWindowMode mode;
  gboolean fullscreen;
};

struct _PhotosItemManagerClass
{
  PhotosBaseManagerClass parent_class;

  /* signals */
  void (*active_collection_changed) (PhotosItemManager *self, PhotosBaseItem *collection);
  void (*can_fullscreen_changed)    (PhotosItemManager *self);
  void (*fullscreen_changed)        (PhotosItemManager *self, gboolean fullscreen);
  void (*load_finished)             (PhotosItemManager *self, PhotosBaseItem *item, GeglNode *node);
  void (*load_started)              (PhotosItemManager *self, PhotosBaseItem *item, GCancellable *cancellable);
  void (*window_mode_changed)       (PhotosItemManager *self, PhotosWindowMode mode, PhotosWindowMode old_mode);
};

enum
{
  ACTIVE_COLLECTION_CHANGED,
  CAN_FULLSCREEN_CHANGED,
  FULLSCREEN_CHANGED,
  LOAD_FINISHED,
  LOAD_STARTED,
  WINDOW_MODE_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };


G_DEFINE_TYPE (PhotosItemManager, photos_item_manager, PHOTOS_TYPE_BASE_MANAGER);
EGG_DEFINE_COUNTER (instances, "PhotosItemManager", "Instances", "Number of PhotosItemManager instances")


typedef struct _PhotosItemManagerHiddenItem PhotosItemManagerHiddenItem;

struct _PhotosItemManagerHiddenItem
{
  PhotosBaseItem *item;
  gboolean *modes;
  guint n_modes;
};


static PhotosItemManagerHiddenItem *
photos_item_manager_hidden_item_new (PhotosBaseItem *item)
{
  GEnumClass *window_mode_class;
  PhotosItemManagerHiddenItem *hidden_item;

  hidden_item = g_slice_new0 (PhotosItemManagerHiddenItem);
  hidden_item->item = g_object_ref (item);

  window_mode_class = G_ENUM_CLASS (g_type_class_ref (PHOTOS_TYPE_WINDOW_MODE));
  hidden_item->n_modes = window_mode_class->n_values;
  hidden_item->modes = (gboolean *) g_malloc0_n (hidden_item->n_modes, sizeof (gboolean));

  g_type_class_unref (window_mode_class);
  return hidden_item;
}


static void
photos_item_manager_hidden_item_free (PhotosItemManagerHiddenItem *hidden_item)
{
  g_free (hidden_item->modes);
  g_object_ref (hidden_item->item);
  g_slice_free (PhotosItemManagerHiddenItem, hidden_item);
}


static void
photos_item_manager_add_object (PhotosBaseManager *mngr, GObject *object)
{
  g_assert_not_reached ();
}


static void
photos_item_manager_item_created_executed (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (user_data);
  GError *error = NULL;
  PhotosSingleItemJob *job = PHOTOS_SINGLE_ITEM_JOB (source_object);
  TrackerSparqlCursor *cursor = NULL;

  cursor = photos_single_item_job_finish (job, res, &error);
  if (error != NULL)
    {
      g_warning ("Unable to query single item: %s", error->message);
      g_error_free (error);
      goto out;
    }

  if (cursor == NULL)
    goto out;

  photos_item_manager_add_item (self, cursor, FALSE);

 out:
  g_clear_object (&cursor);
  g_object_unref (self);
}


static void
photos_item_manager_item_created (PhotosItemManager *self, const gchar *urn)
{
  GApplication *app;
  PhotosBaseItem *old_hidden_item;
  PhotosSearchContextState *state;
  PhotosSingleItemJob *job;

  old_hidden_item = PHOTOS_BASE_ITEM (g_hash_table_lookup (self->hidden_items, urn));
  g_return_if_fail (old_hidden_item == NULL);

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  job = photos_single_item_job_new (urn);
  photos_single_item_job_run (job,
                              state,
                              PHOTOS_QUERY_FLAGS_NONE,
                              NULL,
                              photos_item_manager_item_created_executed,
                              g_object_ref (self));
  g_object_unref (job);
}


static void
photos_item_manager_changes_pending_foreach (gpointer key, gpointer value, gpointer user_data)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (user_data);
  PhotosTrackerChangeEvent *change_event = (PhotosTrackerChangeEvent *) value;
  PhotosTrackerChangeEventType change_type;
  const gchar *change_urn;

  change_type = photos_tracker_change_event_get_type (change_event);
  change_urn = photos_tracker_change_event_get_urn (change_event);

  if (change_type == PHOTOS_TRACKER_CHANGE_EVENT_CHANGED)
    {
      GObject *object;

      object = photos_base_manager_get_object_by_id (PHOTOS_BASE_MANAGER (self), change_urn);
      if (object != NULL)
        photos_base_item_refresh (PHOTOS_BASE_ITEM (object));
    }
  else if (change_type == PHOTOS_TRACKER_CHANGE_EVENT_CREATED)
    {
      photos_item_manager_item_created (self, change_urn);
    }
  else if (change_type == PHOTOS_TRACKER_CHANGE_EVENT_DELETED)
    {
      GObject *object;

      object = photos_base_manager_get_object_by_id (PHOTOS_BASE_MANAGER (self), change_urn);
      if (object != NULL)
        {
          photos_base_item_destroy (PHOTOS_BASE_ITEM (object));
          g_hash_table_remove (self->hidden_items, change_urn);
          photos_base_manager_remove_object_by_id (PHOTOS_BASE_MANAGER (self), change_urn);
        }
    }
}


static void
photos_item_manager_changes_pending (PhotosItemManager *self, GHashTable *changes)
{
  g_hash_table_foreach (changes, photos_item_manager_changes_pending_foreach, self);
}


static void
photos_item_manager_clear_active_item_load (PhotosItemManager *self)
{
  if (self->loader_cancellable != NULL)
    {
      g_cancellable_cancel (self->loader_cancellable);
      g_clear_object (&self->loader_cancellable);
    }
}


static void
photos_item_manager_collection_path_free_foreach (gpointer data, gpointer user_data)
{
  g_clear_object (&data);
}


static void
photos_item_manager_collection_path_free (PhotosItemManager *self)
{
  g_queue_foreach (self->collection_path, photos_item_manager_collection_path_free_foreach, NULL);
  g_queue_free (self->collection_path);
}


static gboolean
photos_item_manager_cursor_is_collection (TrackerSparqlCursor *cursor)
{
  gboolean ret_val;
  const gchar *rdf_type;

  rdf_type = tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_RDF_TYPE, NULL);
  ret_val = strstr (rdf_type, "nfo#DataContainer") != NULL;
  return ret_val;
}


static gboolean
photos_item_manager_cursor_is_favorite (TrackerSparqlCursor *cursor)
{
  gboolean favorite;
  const gchar *rdf_type;

  favorite = tracker_sparql_cursor_get_boolean (cursor, PHOTOS_QUERY_COLUMNS_RESOURCE_FAVORITE);
  rdf_type = tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_RDF_TYPE, NULL);
  if (strstr (rdf_type, "nfo#DataContainer") != NULL)
    favorite = FALSE;

  return favorite;
}


static GObject *
photos_item_manager_get_active_object (PhotosBaseManager *mngr)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (mngr);
  return self->active_object;
}


static GObject *
photos_item_manager_get_object_by_id (PhotosBaseManager *mngr, const gchar *id)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (mngr);
  GObject *ret_val;

  ret_val = photos_base_manager_get_object_by_id (self->item_mngr_chldrn[0], id);
  return ret_val;
}


static GHashTable *
photos_item_manager_get_objects (PhotosBaseManager *mngr)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (mngr);
  GHashTable *ret_val;

  ret_val = photos_base_manager_get_objects (self->item_mngr_chldrn[0]);
  return ret_val;
}


static gchar *
photos_item_manager_get_where (PhotosBaseManager *mngr, gint flags)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (mngr);

  if (self->active_collection == NULL)
    return g_strdup ("");

  return photos_base_item_get_where (self->active_collection);
}


static void
photos_item_manager_info_updated (PhotosBaseItem *item, gpointer user_data)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (user_data);
  PhotosBaseItem *updated_item;
  gboolean is_collection;
  gboolean is_favorite;
  const gchar *id;

  g_return_if_fail (PHOTOS_IS_BASE_ITEM (item));

  id = photos_filterable_get_id (PHOTOS_FILTERABLE (item));
  updated_item = PHOTOS_BASE_ITEM (photos_base_manager_get_object_by_id (PHOTOS_BASE_MANAGER (self), id));
  g_return_if_fail (updated_item == item);

  is_collection = photos_base_item_is_collection (item);
  is_favorite = photos_base_item_is_favorite (item);

  if (is_collection)
    {
      if (self->active_collection == NULL)
        photos_base_manager_add_object (self->item_mngr_chldrn[PHOTOS_WINDOW_MODE_COLLECTIONS], G_OBJECT (item));
    }
  else
    {
      if (is_favorite)
        photos_base_manager_add_object (self->item_mngr_chldrn[PHOTOS_WINDOW_MODE_FAVORITES], G_OBJECT (item));

      photos_base_manager_add_object (self->item_mngr_chldrn[PHOTOS_WINDOW_MODE_OVERVIEW], G_OBJECT (item));
    }

  if (is_collection)
    {
      photos_base_manager_remove_object (self->item_mngr_chldrn[PHOTOS_WINDOW_MODE_FAVORITES], G_OBJECT (item));
      photos_base_manager_remove_object (self->item_mngr_chldrn[PHOTOS_WINDOW_MODE_OVERVIEW], G_OBJECT (item));
    }
  else
    {
      if (self->active_collection == NULL)
        photos_base_manager_remove_object (self->item_mngr_chldrn[PHOTOS_WINDOW_MODE_COLLECTIONS], G_OBJECT (item));

      if (!is_favorite)
        photos_base_manager_remove_object (self->item_mngr_chldrn[PHOTOS_WINDOW_MODE_FAVORITES], G_OBJECT (item));
    }
}


static void
photos_item_manager_item_load (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (user_data);
  GError *error;
  GeglNode *node = NULL;
  PhotosBaseItem *item = PHOTOS_BASE_ITEM (source_object);

  g_clear_object (&self->loader_cancellable);

  error = NULL;
  node = photos_base_item_load_finish (item, res, &error);
  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Unable to load the item: %s", error->message);

      self->load_state = PHOTOS_LOAD_STATE_ERROR;
      g_error_free (error);
    }
  else
    {
      self->load_state = PHOTOS_LOAD_STATE_FINISHED;
    }

  g_signal_emit (self, signals[LOAD_FINISHED], 0, item, node);

  g_clear_object (&node);
  g_object_unref (self);
}


static void
photos_item_manager_remove_object_by_id (PhotosBaseManager *mngr, const gchar *id)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (mngr);
  PhotosBaseItem *item;
  guint i;

  g_hash_table_remove (self->collections, id);

  item = PHOTOS_BASE_ITEM (photos_base_manager_get_object_by_id (PHOTOS_BASE_MANAGER (self), id));
  if (item == NULL)
    return;

  g_signal_handlers_disconnect_by_func (item, photos_item_manager_info_updated, self);
  g_object_ref (item);

  for (i = 0; self->item_mngr_chldrn[i] != NULL; i++)
    photos_base_manager_remove_object_by_id (self->item_mngr_chldrn[i], id);

  g_signal_emit_by_name (self, "object-removed", G_OBJECT (item));
  g_object_unref (item);
}


static void
photos_item_manager_update_fullscreen (PhotosItemManager *self)
{
  /* Should be called after priv->mode has been updated. */

  if (!photos_mode_controller_get_can_fullscreen (self) && self->fullscreen)
    photos_mode_controller_set_fullscreen (self, FALSE);

  g_signal_emit (self, signals[CAN_FULLSCREEN_CHANGED], 0);
}


static gboolean
photos_item_manager_set_window_mode_internal (PhotosItemManager *self,
                                              PhotosWindowMode mode,
                                              PhotosWindowMode *out_old_mode)
{
  PhotosWindowMode old_mode;
  gboolean ret_val = FALSE;

  old_mode = self->mode;

  if (old_mode == mode)
    goto out;

  g_queue_push_head (self->history, GINT_TO_POINTER (old_mode));
  self->mode = mode;

  if (out_old_mode != NULL)
    *out_old_mode = old_mode;

  ret_val = TRUE;

 out:
  return ret_val;
}


static gboolean
photos_item_manager_set_active_object (PhotosBaseManager *manager, GObject *object)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (manager);
  PhotosWindowMode old_mode;
  gboolean active_collection_changed = FALSE;
  gboolean ret_val = FALSE;
  gboolean start_loading = FALSE;
  gboolean window_mode_changed = FALSE;

  g_return_val_if_fail (object != NULL, FALSE);
  g_return_val_if_fail (PHOTOS_IS_BASE_ITEM (object), FALSE);

  if (object == self->active_object)
    goto out;

  photos_item_manager_clear_active_item_load (self);

  if (photos_base_item_is_collection (PHOTOS_BASE_ITEM (object)))
    {
      g_queue_push_head (self->collection_path,
                         (self->active_collection != NULL) ? g_object_ref (self->active_collection) : NULL);

      g_set_object (&self->active_collection, PHOTOS_BASE_ITEM (object));
      self->load_state = PHOTOS_LOAD_STATE_NONE;
      active_collection_changed = TRUE;
    }
  else
    {
      window_mode_changed = photos_item_manager_set_window_mode_internal (self,
                                                                          PHOTOS_WINDOW_MODE_PREVIEW,
                                                                          &old_mode);
      photos_item_manager_update_fullscreen (self);
      self->load_state = PHOTOS_LOAD_STATE_STARTED;
      start_loading = TRUE;
    }

  g_set_object (&self->active_object, object);
  g_signal_emit_by_name (self, "active-changed", self->active_object);
  /* We have already eliminated the possibility of failure. */
  ret_val = TRUE;

  if (active_collection_changed)
    {
      g_signal_emit (self, signals[ACTIVE_COLLECTION_CHANGED], 0, self->active_collection);
      g_assert (self->active_object == (GObject *) self->active_collection);
    }

  if (start_loading)
    {
      GtkRecentManager *recent;
      const gchar *uri;

      recent = gtk_recent_manager_get_default ();
      uri = photos_base_item_get_uri (PHOTOS_BASE_ITEM (object));
      gtk_recent_manager_add_item (recent, uri);

      self->loader_cancellable = g_cancellable_new ();
      photos_base_item_load_async (PHOTOS_BASE_ITEM (object),
                                   self->loader_cancellable,
                                   photos_item_manager_item_load,
                                   g_object_ref (self));

      g_signal_emit (self, signals[LOAD_STARTED], 0, PHOTOS_BASE_ITEM (object));

      if (window_mode_changed)
        g_signal_emit (self, signals[WINDOW_MODE_CHANGED], 0, PHOTOS_WINDOW_MODE_PREVIEW, old_mode);

      g_assert (self->active_object != (GObject *) self->active_collection);
    }

 out:
  return ret_val;
}


static void
photos_item_manager_dispose (GObject *object)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (object);

  if (self->collection_path != NULL)
    {
      photos_item_manager_collection_path_free (self);
      self->collection_path = NULL;
    }

  if (self->item_mngr_chldrn != NULL)
    {
      guint i;

      for (i = 0; self->item_mngr_chldrn[i] != NULL; i++)
        g_object_unref (self->item_mngr_chldrn[i]);

      g_free (self->item_mngr_chldrn);
      self->item_mngr_chldrn = NULL;
    }

  g_clear_pointer (&self->collections, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&self->hidden_items, (GDestroyNotify) g_hash_table_unref);
  g_clear_object (&self->active_object);
  g_clear_object (&self->loader_cancellable);
  g_clear_object (&self->active_collection);
  g_clear_object (&self->monitor);

  G_OBJECT_CLASS (photos_item_manager_parent_class)->dispose (object);
}


static void
photos_item_manager_finalize (GObject *object)
{
  PhotosItemManager *self = PHOTOS_ITEM_MANAGER (object);

  g_queue_free (self->history);

  G_OBJECT_CLASS (photos_item_manager_parent_class)->finalize (object);

  EGG_COUNTER_DEC (instances);
}


static void
photos_item_manager_init (PhotosItemManager *self)
{
  GEnumClass *window_mode_class;
  guint i;

  EGG_COUNTER_INC (instances);

  self->collections = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->hidden_items = g_hash_table_new_full (g_str_hash,
                                              g_str_equal,
                                              g_free,
                                              (GDestroyNotify) photos_item_manager_hidden_item_free);
  self->extension_point = g_io_extension_point_lookup (PHOTOS_BASE_ITEM_EXTENSION_POINT_NAME);
  self->collection_path = g_queue_new ();
  self->history = g_queue_new ();

  window_mode_class = G_ENUM_CLASS (g_type_class_ref (PHOTOS_TYPE_WINDOW_MODE));
  self->item_mngr_chldrn = (PhotosBaseManager **) g_malloc0_n (window_mode_class->n_values + 1,
                                                               sizeof (PhotosBaseManager *));
  for (i = 0; i < window_mode_class->n_values; i++)
    self->item_mngr_chldrn[i] = photos_base_manager_new ();

  self->mode = PHOTOS_WINDOW_MODE_NONE;

  self->monitor = photos_tracker_change_monitor_dup_singleton (NULL, NULL);
  if (G_LIKELY (self->monitor != NULL))
    g_signal_connect_object (self->monitor,
                             "changes-pending",
                             G_CALLBACK (photos_item_manager_changes_pending),
                             self,
                             G_CONNECT_SWAPPED);

  self->fullscreen = FALSE;

  g_type_class_unref (window_mode_class);
}


static void
photos_item_manager_class_init (PhotosItemManagerClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  PhotosBaseManagerClass *base_manager_class = PHOTOS_BASE_MANAGER_CLASS (class);

  object_class->dispose = photos_item_manager_dispose;
  object_class->finalize = photos_item_manager_finalize;
  base_manager_class->add_object = photos_item_manager_add_object;
  base_manager_class->get_active_object = photos_item_manager_get_active_object;
  base_manager_class->get_where = photos_item_manager_get_where;
  base_manager_class->get_object_by_id = photos_item_manager_get_object_by_id;
  base_manager_class->get_objects = photos_item_manager_get_objects;
  base_manager_class->set_active_object = photos_item_manager_set_active_object;
  base_manager_class->remove_object_by_id = photos_item_manager_remove_object_by_id;

  signals[ACTIVE_COLLECTION_CHANGED] = g_signal_new ("active-collection-changed",
                                                     G_TYPE_FROM_CLASS (class),
                                                     G_SIGNAL_RUN_LAST,
                                                     G_STRUCT_OFFSET (PhotosItemManagerClass,
                                                                      active_collection_changed),
                                                     NULL, /*accumulator */
                                                     NULL, /*accu_data */
                                                     g_cclosure_marshal_VOID__OBJECT,
                                                     G_TYPE_NONE,
                                                     1,
                                                     PHOTOS_TYPE_BASE_ITEM);

  signals[CAN_FULLSCREEN_CHANGED] = g_signal_new ("can-fullscreen-changed",
                                                  G_TYPE_FROM_CLASS (class),
                                                  G_SIGNAL_RUN_LAST,
                                                  G_STRUCT_OFFSET (PhotosItemManagerClass,
                                                                   can_fullscreen_changed),
                                                  NULL, /*accumulator */
                                                  NULL, /*accu_data */
                                                  g_cclosure_marshal_VOID__VOID,
                                                  G_TYPE_NONE,
                                                  0);

  signals[FULLSCREEN_CHANGED] = g_signal_new ("fullscreen-changed",
                                              G_TYPE_FROM_CLASS (class),
                                              G_SIGNAL_RUN_LAST,
                                              G_STRUCT_OFFSET (PhotosItemManagerClass,
                                                               fullscreen_changed),
                                              NULL, /*accumulator */
                                              NULL, /* accu_data */
                                              g_cclosure_marshal_VOID__BOOLEAN,
                                              G_TYPE_NONE,
                                              1,
                                              G_TYPE_BOOLEAN);

  signals[LOAD_FINISHED] = g_signal_new ("load-finished",
                                         G_TYPE_FROM_CLASS (class),
                                         G_SIGNAL_RUN_LAST,
                                         G_STRUCT_OFFSET (PhotosItemManagerClass,
                                                          load_finished),
                                         NULL, /*accumulator */
                                         NULL, /*accu_data */
                                         g_cclosure_marshal_generic,
                                         G_TYPE_NONE,
                                         2,
                                         PHOTOS_TYPE_BASE_ITEM,
                                         GEGL_TYPE_NODE);

  signals[LOAD_STARTED] = g_signal_new ("load-started",
                                        G_TYPE_FROM_CLASS (class),
                                        G_SIGNAL_RUN_LAST,
                                        G_STRUCT_OFFSET (PhotosItemManagerClass,
                                                         load_started),
                                        NULL, /*accumulator */
                                        NULL, /*accu_data */
                                        g_cclosure_marshal_VOID__OBJECT,
                                        G_TYPE_NONE,
                                        1,
                                        PHOTOS_TYPE_BASE_ITEM);

  signals[WINDOW_MODE_CHANGED] = g_signal_new ("window-mode-changed",
                                               G_TYPE_FROM_CLASS (class),
                                               G_SIGNAL_RUN_LAST,
                                               G_STRUCT_OFFSET (PhotosItemManagerClass,
                                                                window_mode_changed),
                                               NULL, /*accumulator */
                                               NULL, /*accu_data */
                                               _photos_marshal_VOID__ENUM_ENUM,
                                               G_TYPE_NONE,
                                               2,
                                               PHOTOS_TYPE_WINDOW_MODE,
                                               PHOTOS_TYPE_WINDOW_MODE);
}


PhotosBaseManager *
photos_item_manager_new (void)
{
  return g_object_new (PHOTOS_TYPE_ITEM_MANAGER, NULL);
}


void
photos_item_manager_activate_previous_collection (PhotosItemManager *self)
{
  gpointer *collection;

  photos_item_manager_clear_active_item_load (self);

  collection = g_queue_pop_head (self->collection_path);
  g_assert (collection == NULL || PHOTOS_IS_BASE_ITEM (collection));

  g_set_object (&self->active_collection, PHOTOS_BASE_ITEM (collection));
  g_set_object (&self->active_object, G_OBJECT (collection));

  g_signal_emit_by_name (self, "active-changed", self->active_object);
  g_signal_emit (self, signals[ACTIVE_COLLECTION_CHANGED], 0, self->active_collection);

  g_clear_object (&collection);

  g_return_if_fail (self->active_collection == NULL);
  g_return_if_fail (self->active_object == NULL);
}


void
photos_item_manager_add_item (PhotosItemManager *self, TrackerSparqlCursor *cursor, gboolean force)
{
  g_return_if_fail (PHOTOS_IS_ITEM_MANAGER (self));
  g_return_if_fail (TRACKER_SPARQL_IS_CURSOR (cursor));

  if (photos_item_manager_cursor_is_collection (cursor))
    {
      if (self->active_collection != NULL && force)
        photos_item_manager_activate_previous_collection (self);

      if (self->active_collection == NULL)
        photos_item_manager_add_item_for_mode (self, PHOTOS_WINDOW_MODE_COLLECTIONS, cursor);
    }
  else
    {
      if (photos_item_manager_cursor_is_favorite (cursor))
        photos_item_manager_add_item_for_mode (self, PHOTOS_WINDOW_MODE_FAVORITES, cursor);

      photos_item_manager_add_item_for_mode (self, PHOTOS_WINDOW_MODE_OVERVIEW, cursor);
    }
}


void
photos_item_manager_add_item_for_mode (PhotosItemManager *self, PhotosWindowMode mode, TrackerSparqlCursor *cursor)
{
  PhotosBaseItem *item = NULL;
  PhotosBaseManager *item_mngr_chld;
  gboolean is_collection;
  const gchar *id;

  g_return_if_fail (PHOTOS_IS_ITEM_MANAGER (self));
  g_return_if_fail (TRACKER_SPARQL_IS_CURSOR (cursor));
  g_return_if_fail (mode != PHOTOS_WINDOW_MODE_NONE);
  g_return_if_fail (mode != PHOTOS_WINDOW_MODE_EDIT);
  g_return_if_fail (mode != PHOTOS_WINDOW_MODE_PREVIEW);

  is_collection = photos_item_manager_cursor_is_collection (cursor);
  g_return_if_fail ((is_collection && (mode == PHOTOS_WINDOW_MODE_COLLECTIONS || mode == PHOTOS_WINDOW_MODE_SEARCH))
                    || (!is_collection && (mode != PHOTOS_WINDOW_MODE_COLLECTIONS || self->active_collection != NULL)) || mode == PHOTOS_WINDOW_MODE_IMPORT);

  item_mngr_chld = self->item_mngr_chldrn[mode];
  id = tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_URN, NULL);

  item = PHOTOS_BASE_ITEM (photos_base_manager_get_object_by_id (item_mngr_chld, id));
  if (item != NULL)
    {
      g_object_ref (item);
    }
  else
    {
      gboolean already_present = FALSE;

      item = PHOTOS_BASE_ITEM (photos_base_manager_get_object_by_id (PHOTOS_BASE_MANAGER (self), id));
      if (item != NULL)
        {
          g_object_ref (item);
          already_present = TRUE;
        }
      else
        {
          item = photos_item_manager_create_item (self, cursor);
          if (photos_base_item_is_collection (item))
            g_hash_table_insert (self->collections, g_strdup (id), g_object_ref (item));

          g_signal_connect_object (item, "info-updated", G_CALLBACK (photos_item_manager_info_updated), self, 0);
        }

      photos_base_manager_add_object (item_mngr_chld, G_OBJECT (item));
      photos_base_manager_add_object (self->item_mngr_chldrn[0], G_OBJECT (item));

      if (!already_present)
        g_signal_emit_by_name (self, "object-added", G_OBJECT (item));
    }

  g_clear_object (&item);
}


void
photos_item_manager_clear (PhotosItemManager *self, PhotosWindowMode mode)
{
  GHashTable *items;
  GHashTableIter iter;
  PhotosBaseItem *item;
  PhotosBaseManager *item_mngr_chld;
  const gchar *id;

  g_return_if_fail (PHOTOS_IS_ITEM_MANAGER (self));
  g_return_if_fail (mode != PHOTOS_WINDOW_MODE_NONE);
  g_return_if_fail (mode != PHOTOS_WINDOW_MODE_EDIT);
  g_return_if_fail (mode != PHOTOS_WINDOW_MODE_PREVIEW);

  item_mngr_chld = self->item_mngr_chldrn[mode];
  items = photos_base_manager_get_objects (item_mngr_chld);

  g_hash_table_iter_init (&iter, items);
  while (g_hash_table_iter_next (&iter, (gpointer *) &id, (gpointer *) &item))
    {
      PhotosBaseItem *item1 = NULL;
      guint i;

      for (i = 1; self->item_mngr_chldrn[i] != NULL; i++)
        {
          if (item_mngr_chld == self->item_mngr_chldrn[i])
            continue;

          item1 = PHOTOS_BASE_ITEM (photos_base_manager_get_object_by_id (self->item_mngr_chldrn[i], id));
          if (item1 != NULL)
            break;
        }

      if (item1 == NULL)
        {
          item1 = PHOTOS_BASE_ITEM (photos_base_manager_get_object_by_id (self->item_mngr_chldrn[0], id));
          g_assert_true (item == item1);

          photos_base_manager_remove_object_by_id (self->item_mngr_chldrn[0], id);
        }
    }

  photos_base_manager_clear (item_mngr_chld);
}


PhotosBaseItem *
photos_item_manager_create_item (PhotosItemManager *self, TrackerSparqlCursor *cursor)
{
  GIOExtension *extension;
  GType type;
  PhotosBaseItem *ret_val = NULL;
  const gchar *extension_name = "local";
  gchar *identifier = NULL;
  gchar **split_identifier = NULL;

  identifier = g_strdup (tracker_sparql_cursor_get_string (cursor, PHOTOS_QUERY_COLUMNS_IDENTIFIER, NULL));
  if (identifier == NULL)
    goto final;

  split_identifier = g_strsplit (identifier, ":", 4);

  if (photos_item_manager_cursor_is_collection (cursor))
    {
      /* Its a collection. */
      extension_name = split_identifier[2];
    }
  else
    {
      /* Its a normal photo item. */
      if (g_strv_length (split_identifier) > 1)
        extension_name = split_identifier[0];
    }

 final:
  extension = g_io_extension_point_get_extension_by_name (self->extension_point, extension_name);
  if (G_UNLIKELY (extension == NULL))
    {
      g_warning ("Unable to find extension %s for identifier: %s", extension_name, identifier);
      goto out;
    }

  type = g_io_extension_get_type (extension);
  ret_val = PHOTOS_BASE_ITEM (g_object_new (type,
                                            "cursor", cursor,
                                            "failed-thumbnailing", FALSE,
                                            NULL));

 out:
  g_strfreev (split_identifier);
  g_free (identifier);
  return ret_val;
}


PhotosBaseItem *
photos_item_manager_get_active_collection (PhotosItemManager *self)
{
  return self->active_collection;
}


GHashTable *
photos_item_manager_get_collections (PhotosItemManager *self)
{
  return self->collections;
}


PhotosBaseManager *
photos_item_manager_get_for_mode (PhotosItemManager *self, PhotosWindowMode mode)
{
  g_return_val_if_fail (mode != PHOTOS_WINDOW_MODE_NONE, NULL);
  g_return_val_if_fail (mode != PHOTOS_WINDOW_MODE_EDIT, NULL);
  g_return_val_if_fail (mode != PHOTOS_WINDOW_MODE_PREVIEW, NULL);

  return self->item_mngr_chldrn[mode];
}


PhotosLoadState
photos_item_manager_get_load_state (PhotosItemManager *self)
{
  return self->load_state;
}


void
photos_item_manager_hide_item (PhotosItemManager *self, PhotosBaseItem *item)
{
  PhotosItemManagerHiddenItem *hidden_item;
  PhotosItemManagerHiddenItem *old_hidden_item;
  const gchar *id;
  guint i;

  g_return_if_fail (PHOTOS_IS_ITEM_MANAGER (self));
  g_return_if_fail (PHOTOS_IS_BASE_ITEM (item));

  id = photos_filterable_get_id (PHOTOS_FILTERABLE (item));
  g_return_if_fail (id != NULL && id[0] != '\0');

  old_hidden_item = (PhotosItemManagerHiddenItem *) g_hash_table_lookup (self->hidden_items, id);
  g_return_if_fail (old_hidden_item == NULL);

  hidden_item = photos_item_manager_hidden_item_new (item);
  for (i = 0; self->item_mngr_chldrn[i] != NULL; i++)
    {
      PhotosBaseItem *item1;

      g_assert_cmpuint (i, <, hidden_item->n_modes);

      item1 = PHOTOS_BASE_ITEM (photos_base_manager_get_object_by_id (self->item_mngr_chldrn[i], id));
      if (item1 != NULL)
        {
          g_assert_true (item == item1);
          hidden_item->modes[i] = TRUE;
        }
    }

  g_hash_table_insert (self->hidden_items, g_strdup (id), hidden_item);
  photos_base_manager_remove_object_by_id (PHOTOS_BASE_MANAGER (self), id);
}


void
photos_item_manager_unhide_item (PhotosItemManager *self, PhotosBaseItem *item)
{
  PhotosItemManagerHiddenItem *hidden_item;
  const gchar *id;
  guint i;

  g_return_if_fail (PHOTOS_IS_ITEM_MANAGER (self));
  g_return_if_fail (PHOTOS_IS_BASE_ITEM (item));

  id = photos_filterable_get_id (PHOTOS_FILTERABLE (item));
  g_return_if_fail (id != NULL && id[0] != '\0');

  hidden_item = (PhotosItemManagerHiddenItem *) g_hash_table_lookup (self->hidden_items, id);
  g_return_if_fail (hidden_item->item == item);

  for (i = 0; self->item_mngr_chldrn[i] != NULL; i++)
    {
      g_assert_cmpuint (i, <, hidden_item->n_modes);

      if (hidden_item->modes[i])
        photos_base_manager_add_object (self->item_mngr_chldrn[i], G_OBJECT (item));
    }

  g_hash_table_remove (self->hidden_items, id);
  g_signal_emit_by_name (self, "object-added", G_OBJECT (item));
}


gboolean
photos_mode_controller_get_can_fullscreen (PhotosModeController *self)
{
  return self->mode == PHOTOS_WINDOW_MODE_PREVIEW;
}


gboolean
photos_mode_controller_get_fullscreen (PhotosModeController *self)
{
  return self->fullscreen;
}


PhotosWindowMode
photos_mode_controller_get_window_mode (PhotosModeController *self)
{
  return self->mode;
}


void
photos_mode_controller_go_back (PhotosModeController *self)
{
  PhotosWindowMode old_mode;
  PhotosWindowMode tmp;

  g_return_if_fail (!g_queue_is_empty (self->history));

  old_mode = (PhotosWindowMode) GPOINTER_TO_INT (g_queue_peek_head (self->history));

  /* Always go back to the overview when activated from the search
   * provider. It is easier to special case it here instead of all
   * over the code.
   */
  if (self->mode == PHOTOS_WINDOW_MODE_PREVIEW && old_mode == PHOTOS_WINDOW_MODE_NONE)
    old_mode = PHOTOS_WINDOW_MODE_OVERVIEW;

  g_return_if_fail (old_mode != PHOTOS_WINDOW_MODE_NONE);

  if (self->mode == PHOTOS_WINDOW_MODE_EDIT)
    {
      g_return_if_fail (self->load_state == PHOTOS_LOAD_STATE_FINISHED);
      g_return_if_fail (old_mode == PHOTOS_WINDOW_MODE_PREVIEW);
    }
  else
    {
      g_return_if_fail (old_mode != PHOTOS_WINDOW_MODE_PREVIEW);
    }

  g_queue_pop_head (self->history);

  /* Swap the old and current modes */
  tmp = old_mode;
  old_mode = self->mode;
  self->mode = tmp;

  photos_item_manager_update_fullscreen (self);
  photos_item_manager_clear_active_item_load (self);

  if (old_mode == PHOTOS_WINDOW_MODE_PREVIEW)
    {
      self->load_state = PHOTOS_LOAD_STATE_NONE;
      g_set_object (&self->active_object, G_OBJECT (self->active_collection));
      g_signal_emit_by_name (self, "active-changed", self->active_object);
    }
  else if (old_mode != PHOTOS_WINDOW_MODE_EDIT)
    {
      photos_item_manager_collection_path_free (self);
      self->collection_path = g_queue_new ();

      g_clear_object (&self->active_collection);
      g_clear_object (&self->active_object);
      self->load_state = PHOTOS_LOAD_STATE_NONE;

      g_signal_emit_by_name (self, "active-changed", self->active_object);
      g_signal_emit (self, signals[ACTIVE_COLLECTION_CHANGED], 0, self->active_collection);
    }

  g_signal_emit (self, signals[WINDOW_MODE_CHANGED], 0, self->mode, old_mode);
}


void
photos_mode_controller_toggle_fullscreen (PhotosModeController *self)
{
  photos_mode_controller_set_fullscreen (self, !self->fullscreen);
}


void
photos_mode_controller_set_fullscreen (PhotosModeController *self, gboolean fullscreen)
{
  if (self->fullscreen == fullscreen)
    return;

  self->fullscreen = fullscreen;
  g_signal_emit (self, signals[FULLSCREEN_CHANGED], 0, self->fullscreen);
}


void
photos_mode_controller_set_window_mode (PhotosModeController *self, PhotosWindowMode mode)
{
  PhotosWindowMode old_mode;
  gboolean active_collection_changed = FALSE;

  g_return_if_fail (mode != PHOTOS_WINDOW_MODE_NONE);
  g_return_if_fail (mode != PHOTOS_WINDOW_MODE_PREVIEW);

  if (mode == PHOTOS_WINDOW_MODE_EDIT)
    {
      g_return_if_fail (self->load_state == PHOTOS_LOAD_STATE_FINISHED);
      g_return_if_fail (self->mode == PHOTOS_WINDOW_MODE_PREVIEW);
    }
  else
    {
      g_return_if_fail (self->mode != PHOTOS_WINDOW_MODE_PREVIEW);
    }

  if (!photos_item_manager_set_window_mode_internal (self, mode, &old_mode))
    return;

  photos_item_manager_update_fullscreen (self);
  photos_item_manager_clear_active_item_load (self);

  if (mode != PHOTOS_WINDOW_MODE_EDIT)
    {
      self->load_state = PHOTOS_LOAD_STATE_NONE;
      photos_item_manager_collection_path_free (self);
      self->collection_path = g_queue_new ();

      if (self->active_collection != NULL)
        {
          g_clear_object (&self->active_collection);
          active_collection_changed = TRUE;
        }

      g_clear_object (&self->active_object);
      g_signal_emit_by_name (self, "active-changed", self->active_object);

      if (active_collection_changed)
        g_signal_emit (self, signals[ACTIVE_COLLECTION_CHANGED], 0, self->active_collection);
    }

  g_signal_emit (self, signals[WINDOW_MODE_CHANGED], 0, mode, old_mode);
}
