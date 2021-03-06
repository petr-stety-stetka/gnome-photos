/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2013 – 2016 Red Hat, Inc.
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

#include <glib.h>

#include "photos-base-item.h"
#include "photos-base-manager.h"
#include "photos-preview-model.h"
#include "photos-search-context.h"
#include "photos-view-model.h"


struct _PhotosPreviewModel
{
  GtkTreeModelFilter parent_instance;
  PhotosBaseManager *item_mngr;
};

struct _PhotosPreviewModelClass
{
  GtkTreeModelFilterClass parent_class;
};


G_DEFINE_TYPE (PhotosPreviewModel, photos_preview_model, GTK_TYPE_TREE_MODEL_FILTER);


static gboolean
photos_preview_model_visible (GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data)
{
  PhotosPreviewModel *self = PHOTOS_PREVIEW_MODEL (user_data);
  PhotosBaseItem *item;
  gboolean ret_val = FALSE;
  gchar *id;

  gtk_tree_model_get (model, iter, PHOTOS_VIEW_MODEL_URN, &id, -1);
  if (id == NULL)
    goto out;

  item = PHOTOS_BASE_ITEM (photos_base_manager_get_object_by_id (self->item_mngr, id));
  ret_val = !photos_base_item_is_collection (item);

 out:
  g_free (id);
  return ret_val;
}


static void
photos_preview_model_dispose (GObject *object)
{
  PhotosPreviewModel *self = PHOTOS_PREVIEW_MODEL (object);

  g_clear_object (&self->item_mngr);

  G_OBJECT_CLASS (photos_preview_model_parent_class)->dispose (object);
}


static void
photos_preview_model_init (PhotosPreviewModel *self)
{
  GApplication *app;
  PhotosSearchContextState *state;

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  self->item_mngr = g_object_ref (state->item_mngr);
  gtk_tree_model_filter_set_visible_func (GTK_TREE_MODEL_FILTER (self), photos_preview_model_visible, self, NULL);
}


static void
photos_preview_model_class_init (PhotosPreviewModelClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->dispose = photos_preview_model_dispose;
}


GtkTreeModel *
photos_preview_model_new (GtkTreeModel *child_model)
{
  return g_object_new (PHOTOS_TYPE_PREVIEW_MODEL, "child-model", child_model, NULL);
}
