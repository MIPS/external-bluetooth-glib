/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <glib.h>
#include <gfileoutputstream.h>
#include <gseekable.h>
#include "gsimpleasyncresult.h"
#include "glibintl.h"

#include "gioalias.h"

/**
 * SECTION:gfileoutputstream
 * @short_description: file output streaming operations
 * @see_also: #GOutputStream, #GDataOutputStream, #GSeekable
 * 
 * 
 *
 **/

static void       g_file_output_stream_seekable_iface_init    (GSeekableIface       *iface);
static goffset    g_file_output_stream_seekable_tell          (GSeekable            *seekable);
static gboolean   g_file_output_stream_seekable_can_seek      (GSeekable            *seekable);
static gboolean   g_file_output_stream_seekable_seek          (GSeekable            *seekable,
							       goffset               offset,
							       GSeekType             type,
							       GCancellable         *cancellable,
							       GError              **error);
static gboolean   g_file_output_stream_seekable_can_truncate  (GSeekable            *seekable);
static gboolean   g_file_output_stream_seekable_truncate      (GSeekable            *seekable,
							       goffset               offset,
							       GCancellable         *cancellable,
							       GError              **error);
static void       g_file_output_stream_real_query_info_async  (GFileOutputStream    *stream,
							       char                 *attributes,
							       int                   io_priority,
							       GCancellable         *cancellable,
							       GAsyncReadyCallback   callback,
							       gpointer              user_data);
static GFileInfo *g_file_output_stream_real_query_info_finish (GFileOutputStream    *stream,
							       GAsyncResult         *result,
							       GError              **error);

G_DEFINE_TYPE_WITH_CODE (GFileOutputStream, g_file_output_stream, G_TYPE_OUTPUT_STREAM,
			 G_IMPLEMENT_INTERFACE (G_TYPE_SEEKABLE,
						g_file_output_stream_seekable_iface_init));

struct _GFileOutputStreamPrivate {
  GAsyncReadyCallback outstanding_callback;
};

static void
g_file_output_stream_class_init (GFileOutputStreamClass *klass)
{
  g_type_class_add_private (klass, sizeof (GFileOutputStreamPrivate));

  klass->query_info_async = g_file_output_stream_real_query_info_async;
  klass->query_info_finish = g_file_output_stream_real_query_info_finish;
}

static void
g_file_output_stream_seekable_iface_init (GSeekableIface *iface)
{
  iface->tell = g_file_output_stream_seekable_tell;
  iface->can_seek = g_file_output_stream_seekable_can_seek;
  iface->seek = g_file_output_stream_seekable_seek;
  iface->can_truncate = g_file_output_stream_seekable_can_truncate;
  iface->truncate = g_file_output_stream_seekable_truncate;
}

static void
g_file_output_stream_init (GFileOutputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_FILE_OUTPUT_STREAM,
					      GFileOutputStreamPrivate);
}

/**
 * g_file_output_stream_query_info:
 * @stream: a #GFileOutputStream.
 * @attributes: a file attribute query string.
 * @cancellable: optional #GCancellable object, %NULL to ignore. 
 * @error: a #GError, %NULL to ignore.
 *
 * Queries a file output stream for the given @attributes. 
 * This function blocks while querying the stream. For the asynchronous 
 * version of this function, see g_file_output_stream_query_info_async(). 
 * While the stream is blocked, the stream will set the pending flag 
 * internally, and any other operations on the stream will fail with 
 * %G_IO_ERROR_PENDING.
 * 
 * Can fail if the stream was already closed (with @error being set to 
 * %G_IO_ERROR_CLOSED), the stream has pending operations (with @error being
 * set to %G_IO_ERROR_PENDING), or if querying info is not supported for 
 * the stream's interface (with @error being set to %G_IO_ERROR_NOT_SUPPORTED). In
 * all cases of failure, %NULL will be returned.
 * 
 * If @cancellable is not %NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error %G_IO_ERROR_CANCELLED will be set, and %NULL will 
 * be returned. 
 * 
 * Returns: a #GFileInfo for the @stream, or %NULL on error.
 **/
GFileInfo *
g_file_output_stream_query_info (GFileOutputStream      *stream,
				    char                   *attributes,
				    GCancellable           *cancellable,
				    GError                **error)
{
  GFileOutputStreamClass *class;
  GOutputStream *output_stream;
  GFileInfo *info;
  
  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), NULL);
  
  output_stream = G_OUTPUT_STREAM (stream);
  
  if (g_output_stream_is_closed (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Stream is already closed"));
      return NULL;
    }
  
  if (g_output_stream_has_pending (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return NULL;
    }
      
  info = NULL;
  
  g_output_stream_set_pending (output_stream, TRUE);
  
  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);
  if (class->query_info)
    info = class->query_info (stream, attributes, cancellable, error);
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		 _("Stream doesn't support query_info"));
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  g_output_stream_set_pending (output_stream, FALSE);
  
  return info;
}

static void
async_ready_callback_wrapper (GObject *source_object,
			      GAsyncResult *res,
			      gpointer      user_data)
{
  GFileOutputStream *stream = G_FILE_OUTPUT_STREAM (source_object);

  g_output_stream_set_pending (G_OUTPUT_STREAM (stream), FALSE);
  if (stream->priv->outstanding_callback)
    (*stream->priv->outstanding_callback) (source_object, res, user_data);
  g_object_unref (stream);
}

/**
 * g_file_output_stream_query_info_async:
 * @stream: a #GFileOutputStream.
 * @attributes: a file attribute query string.
 * @io_priority: the io priority of the request.
 * @cancellable: optional #GCancellable object, %NULL to ignore. 
 * @callback: callback to call when the request is satisfied
 * @user_data: the data to pass to callback function
 * 
 * Asynchronously queries the @stream for a #GFileInfo. When completed,
 * @callback will be called with a #GAsyncResult which can be used to 
 * finish the operation with g_file_output_stream_query_info_finish().
 * 
 * For the synchronous version of this function, see 
 * g_file_output_stream_query_info().
 *
 **/
void
g_file_output_stream_query_info_async (GFileOutputStream     *stream,
					  char                 *attributes,
					  int                   io_priority,
					  GCancellable         *cancellable,
					  GAsyncReadyCallback   callback,
					  gpointer              user_data)
{
  GFileOutputStreamClass *klass;
  GOutputStream *output_stream;

  g_return_if_fail (G_IS_FILE_OUTPUT_STREAM (stream));

  output_stream = G_OUTPUT_STREAM (stream);
 
  if (g_output_stream_is_closed (output_stream))
    {
      g_simple_async_report_error_in_idle (G_OBJECT (stream),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_CLOSED,
					   _("Stream is already closed"));
      return;
    }
  
  if (g_output_stream_has_pending (output_stream))
    {
      g_simple_async_report_error_in_idle (G_OBJECT (stream),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_PENDING,
					   _("Stream has outstanding operation"));
      return;
    }

  klass = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);

  g_output_stream_set_pending (output_stream, TRUE);
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  klass->query_info_async (stream, attributes, io_priority, cancellable,
			      async_ready_callback_wrapper, user_data);
}

/**
 * g_file_output_stream_query_info_finish:
 * @stream: a #GFileOutputStream.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous query started 
 * by g_file_output_stream_query_info_async().
 * 
 * Returns: A #GFileInfo for the finished query.
 **/
GFileInfo *
g_file_output_stream_query_info_finish (GFileOutputStream     *stream,
					   GAsyncResult         *result,
					   GError              **error)
{
  GSimpleAsyncResult *simple;
  GFileOutputStreamClass *class;

  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), NULL);
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), NULL);
  
  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return NULL;
    }

  class = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);
  return class->query_info_finish (stream, result, error);
}

/**
 * g_file_output_stream_get_etag:
 * @stream: a #GFileOutputStream.
 * 
 * Gets the entity tag for the file output stream.
 * 
 * Returns: the entity tag for the stream.
 **/
char *
g_file_output_stream_get_etag (GFileOutputStream  *stream)
{
  GFileOutputStreamClass *class;
  GOutputStream *output_stream;
  char *etag;
  
  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), NULL);
  
  output_stream = G_OUTPUT_STREAM (stream);
  
  if (!g_output_stream_is_closed (output_stream))
    {
      g_warning ("stream is not closed yet, can't get etag");
      return NULL;
    }

  etag = NULL;
  
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);
  if (class->get_etag)
    etag = class->get_etag (stream);
  
  return etag;
}

/**
 * g_file_output_stream_tell:
 * @stream: a #GFileOutputStream.
 * 
 * Gets the current location within the stream.
 * 
 * Returns: a #goffset of the location within the stream.
 **/
goffset
g_file_output_stream_tell (GFileOutputStream  *stream)
{
  GFileOutputStreamClass *class;
  goffset offset;
  
  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), 0);  

  class = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);

  offset = 0;
  if (class->tell)
    offset = class->tell (stream);

  return offset;
}

static goffset
g_file_output_stream_seekable_tell (GSeekable *seekable)
{
  return g_file_output_stream_tell (G_FILE_OUTPUT_STREAM (seekable));
}

/**
 * g_file_output_stream_can_seek:
 * @stream: a #GFileOutputStream.
 * 
 * Checks if the stream can be seeked.
 * 
 * Returns: %TRUE if seeking is supported by the stream.
 **/
gboolean
g_file_output_stream_can_seek (GFileOutputStream  *stream)
{
  GFileOutputStreamClass *class;
  gboolean can_seek;

  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), FALSE);

  class = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);

  can_seek = FALSE;
  if (class->seek)
    {
      can_seek = TRUE;
      if (class->can_seek)
	can_seek = class->can_seek (stream);
    }
  
  return can_seek;
}

static gboolean
g_file_output_stream_seekable_can_seek (GSeekable *seekable)
{
  return g_file_output_stream_can_seek (G_FILE_OUTPUT_STREAM (seekable));
}

/**
 * g_file_output_stream_seek:
 * @stream: a #GFileOutputStream.
 * @offset: a #goffset to seek.
 * @type: a #GSeekType.
 * @cancellable: optional #GCancellable object, %NULL to ignore. 
 * @error: a #GError, %NULL to ignore.
 * 
 * Seeks to a location in a file output stream.
 * 
 * Returns: %TRUE if the seek was successful. %FALSE otherwise.
 **/
gboolean
g_file_output_stream_seek (GFileOutputStream  *stream,
			   goffset             offset,
			   GSeekType           type,
			   GCancellable       *cancellable,
			   GError            **error)
{
  GFileOutputStreamClass *class;
  GOutputStream *output_stream;
  gboolean res;

  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), FALSE);

  output_stream = G_OUTPUT_STREAM (stream);
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);

  if (g_output_stream_is_closed (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Stream is already closed"));
      return FALSE;
    }
  
  if (g_output_stream_has_pending (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }
  
  if (!class->seek)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		   _("Seek not supported on stream"));
      return FALSE;
    }

  g_output_stream_set_pending (output_stream, TRUE);
  
  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  res = class->seek (stream, offset, type, cancellable, error);
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);

  g_output_stream_set_pending (output_stream, FALSE);
  
  return res;
}

static gboolean
g_file_output_stream_seekable_seek (GSeekable  *seekable,
				    goffset     offset,
				    GSeekType   type,
				    GCancellable  *cancellable,
				    GError    **error)
{
  return g_file_output_stream_seek (G_FILE_OUTPUT_STREAM (seekable),
				    offset, type, cancellable, error);
}

/**
 * g_file_output_stream_can_truncate:
 * @stream: a #GFileOutputStream.
 * 
 * Checks if the stream can be truncated.
 * 
 * Returns: %TRUE if stream can be truncated.
 **/
gboolean
g_file_output_stream_can_truncate (GFileOutputStream  *stream)
{
  GFileOutputStreamClass *class;
  gboolean can_truncate;

  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), FALSE);

  class = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);

  can_truncate = FALSE;
  if (class->truncate)
    {
      can_truncate = TRUE;
      if (class->can_truncate)
	can_truncate = class->can_truncate (stream);
    }
  
  return can_truncate;
}

static gboolean
g_file_output_stream_seekable_can_truncate (GSeekable  *seekable)
{
  return g_file_output_stream_can_truncate (G_FILE_OUTPUT_STREAM (seekable));
}

/**
 * g_file_output_stream_truncate:
 * @stream: a #GFileOutputStream.
 * @size: a #goffset to truncate the stream at.
 * @cancellable: optional #GCancellable object, %NULL to ignore. 
 * @error: a #GError, %NULL to ignore.
 * 
 * Truncates a file output stream.
 * 
 * Returns: %TRUE if @stream is truncated successfully.
 **/
gboolean
g_file_output_stream_truncate (GFileOutputStream  *stream,
			       goffset             size,
			       GCancellable       *cancellable,
			       GError            **error)
{
  GFileOutputStreamClass *class;
  GOutputStream *output_stream;
  gboolean res;

  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), FALSE);

  output_stream = G_OUTPUT_STREAM (stream);
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);

  if (g_output_stream_is_closed (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Stream is already closed"));
      return FALSE;
    }
  
  if (g_output_stream_has_pending (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }
  
  if (!class->truncate)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		   _("Truncate not supported on stream"));
      return FALSE;
    }

  g_output_stream_set_pending (output_stream, TRUE);
  
  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  res = class->truncate (stream, size, cancellable, error);
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);

  g_output_stream_set_pending (output_stream, FALSE);
  
  return res;
}

static gboolean
g_file_output_stream_seekable_truncate (GSeekable     *seekable,
					goffset        size,
					GCancellable  *cancellable,
					GError       **error)
{
  return g_file_output_stream_truncate (G_FILE_OUTPUT_STREAM (seekable),
					size, cancellable, error);
}
/********************************************
 *   Default implementation of async ops    *
 ********************************************/

typedef struct {
  char *attributes;
  GFileInfo *info;
} QueryInfoAsyncData;

static void
query_info_data_free (QueryInfoAsyncData *data)
{
  if (data->info)
    g_object_unref (data->info);
  g_free (data->attributes);
  g_free (data);
}

static void
query_info_async_thread (GSimpleAsyncResult *res,
		       GObject *object,
		       GCancellable *cancellable)
{
  GFileOutputStreamClass *class;
  GError *error = NULL;
  QueryInfoAsyncData *data;
  GFileInfo *info;
  
  data = g_simple_async_result_get_op_res_gpointer (res);

  info = NULL;
  
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (object);
  if (class->query_info)
    info = class->query_info (G_FILE_OUTPUT_STREAM (object), data->attributes, cancellable, &error);
  else
    g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		 _("Stream doesn't support query_info"));

  if (info == NULL)
    {
      g_simple_async_result_set_from_error (res, error);
      g_error_free (error);
    }
  else
    data->info = info;
}

static void
g_file_output_stream_real_query_info_async (GFileOutputStream     *stream,
					       char                 *attributes,
					       int                   io_priority,
					       GCancellable         *cancellable,
					       GAsyncReadyCallback   callback,
					       gpointer              user_data)
{
  GSimpleAsyncResult *res;
  QueryInfoAsyncData *data;

  data = g_new0 (QueryInfoAsyncData, 1);
  data->attributes = g_strdup (attributes);
  
  res = g_simple_async_result_new (G_OBJECT (stream), callback, user_data, g_file_output_stream_real_query_info_async);
  g_simple_async_result_set_op_res_gpointer (res, data, (GDestroyNotify)query_info_data_free);
  
  g_simple_async_result_run_in_thread (res, query_info_async_thread, io_priority, cancellable);
  g_object_unref (res);
}

static GFileInfo *
g_file_output_stream_real_query_info_finish (GFileOutputStream     *stream,
						GAsyncResult         *res,
						GError              **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  QueryInfoAsyncData *data;

  g_assert (g_simple_async_result_get_source_tag (simple) == g_file_output_stream_real_query_info_async);

  data = g_simple_async_result_get_op_res_gpointer (simple);
  if (data->info)
    return g_object_ref (data->info);
  
  return NULL;
}

#define __G_FILE_OUTPUT_STREAM_C__
#include "gioaliasdef.c"