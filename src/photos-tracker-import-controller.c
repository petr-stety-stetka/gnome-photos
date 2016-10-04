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


#include "config.h"

#include <gio/gio.h>

#include "photos-item-manager.h"
#include "photos-offset-import-controller.h"
#include "photos-query-builder.h"
#include "photos-search-context.h"
#include "photos-tracker-import-controller.h"
#include "photos-utils.h"


struct _PhotosTrackerImportController
{
  PhotosTrackerController parent_instance;
  PhotosOffsetController *offset_cntrlr;
};

struct _PhotosTrackerImportControllerClass
{
  PhotosTrackerControllerClass parent_class;
};


G_DEFINE_TYPE_WITH_CODE (PhotosTrackerImportController,
                         photos_tracker_import_controller,
                         PHOTOS_TYPE_TRACKER_CONTROLLER,
                         photos_utils_ensure_extension_points ();
                         g_io_extension_point_implement (PHOTOS_TRACKER_CONTROLLER_EXTENSION_POINT_NAME,
                                                         g_define_type_id,
                                                         "import",
                                                         0));


static PhotosOffsetController *
photos_tracker_import_controller_get_offset_controller (PhotosTrackerController *trk_cntrlr)
{
  PhotosTrackerImportController *self = PHOTOS_TRACKER_IMPORT_CONTROLLER (trk_cntrlr);
  return g_object_ref (self->offset_cntrlr);
}


static PhotosQuery *
photos_tracker_import_controller_get_query (PhotosTrackerController *trk_cntrlr)
{
  PhotosTrackerImportController *self = PHOTOS_TRACKER_IMPORT_CONTROLLER (trk_cntrlr);
  GApplication *app;
  PhotosSearchContextState *state;

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  return photos_query_builder_global_query (state, PHOTOS_QUERY_FLAGS_IMPORT, self->offset_cntrlr);
}


static GObject *
photos_tracker_import_controller_constructor (GType type,
                                                guint n_construct_params,
                                                GObjectConstructParam *construct_params)
{
  static GObject *self = NULL;

  if (self == NULL)
    {
      self = G_OBJECT_CLASS (photos_tracker_import_controller_parent_class)->constructor (type,
                                                                                             n_construct_params,
                                                                                             construct_params);
      g_object_add_weak_pointer (self, (gpointer) &self);
      return self;
    }

  return g_object_ref (self);
}


static void
photos_tracker_import_controller_dispose (GObject *object)
{
  PhotosTrackerImportController *self = PHOTOS_TRACKER_IMPORT_CONTROLLER (object);

  g_clear_object (&self->offset_cntrlr);

  G_OBJECT_CLASS (photos_tracker_import_controller_parent_class)->dispose (object);
}


static void
photos_tracker_import_controller_init (PhotosTrackerImportController *self)
{
  self->offset_cntrlr = photos_offset_import_controller_dup_singleton ();
}


static void
photos_tracker_import_controller_class_init (PhotosTrackerImportControllerClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  PhotosTrackerControllerClass *tracker_controller_class = PHOTOS_TRACKER_CONTROLLER_CLASS (class);

  object_class->constructor = photos_tracker_import_controller_constructor;
  object_class->dispose = photos_tracker_import_controller_dispose;
  tracker_controller_class->get_offset_controller = photos_tracker_import_controller_get_offset_controller;
  tracker_controller_class->get_query = photos_tracker_import_controller_get_query;
}


PhotosTrackerController *
photos_tracker_import_controller_dup_singleton (void)
{
  return g_object_new (PHOTOS_TYPE_TRACKER_IMPORT_CONTROLLER, "mode", PHOTOS_WINDOW_MODE_IMPORT, NULL);
}