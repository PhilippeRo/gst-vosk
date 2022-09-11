/*
 * GStreamer Vosk plugin
 * Copyright (C) 2022 Philippe Rouquier <bonfire-app@wanadoo.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __GST_VOSK_H__
#define __GST_VOSK_H__

#include <gio/gio.h>
#include <gst/gst.h>

#include "vosk-api.h"

G_BEGIN_DECLS

#define GST_TYPE_VOSK \
  (gst_vosk_get_type())
#define GST_VOSK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_VOSK,GstVosk))
#define GST_VOSK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_VOSK,GstVoskClass))
#define GST_IS_VOSK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_VOSK))
#define GST_IS_VOSK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_VOSK))

typedef struct _GstVosk      GstVosk;
typedef struct _GstVoskClass GstVoskClass;

struct _GstVosk
{
  GstElement        element;
  GstPad           *sinkpad, *srcpad;

  gchar            *model_path;
  gint              alternatives;
  gboolean          use_signals;

  gfloat            rate;

  GstClockTime      last_processed_time;

  GstClockTime      last_partial;
  gint64            partial_time_interval;

  GThreadPool      *thread_pool;

  GMutex            RecMut;

  /* Access to the following members should be done
   * with GST_VOSK_LOCK held */
  VoskModel        *model;
  VoskRecognizer   *recognizer;
  gchar            *prev_partial;

  GCancellable     *current_operation;
  GCond             wake_stream;
};

struct _GstVoskClass
{
  GstElementClass parent_class;
};

GType gst_vosk_get_type (void);

G_END_DECLS

#endif /* __GST_VOSK_H__ */
