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

#include <libintl.h>
#include <locale.h>

#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>

#include "gstvosk.h"
#include "vosk-api.h"
#include "../gst-vosk-config.h"

GST_DEBUG_CATEGORY_STATIC (gst_vosk_debug);
#define GST_CAT_DEFAULT gst_vosk_debug

#define DEFAULT_SPEECH_MODEL "/usr/share/vosk/model"
#define DEFAULT_ALTERNATIVE_NUM 0

#define _(STRING) gettext(STRING)

#define GST_VOSK_LOCK(vosk) (g_mutex_lock(&vosk->RecMut))
#define GST_VOSK_UNLOCK(vosk) (g_mutex_unlock(&vosk->RecMut))

enum
{
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_SPEECH_MODEL,
  PROP_ALTERNATIVES,
  PROP_RESULT,
  PROP_PARTIAL_RESULTS,
};

/*
 * gst-launch-1.0 -m pulsesrc  buffer-time=9223372036854775807 ! \
 *                   audio/x-raw,format=S16LE,rate=16000, channels=1 ! \
 *                   vosk speech-model=path/to/model ! fakesink
*/

#define VOSK_EMPTY_PARTIAL_RESULT  "{\n  \"partial\" : \"\"\n}"
#define VOSK_EMPTY_TEXT_RESULT     "{\n  \"text\" : \"\"\n}"
#define VOSK_EMPTY_TEXT_RESULT_ALT "{\"text\": \"\"}"

/* BUG : protect from local formatting errors when fr_ prefix
   Maybe there are other locales ?
   Use uselocale () when we can as it is supposed to be safer since it sets
   the locale only for the thread. */
#if HAVE_USELOCALE

#define PROTECT_FROM_LOCALE_BUG_START                         \
  locale_t current_locale;                                    \
  locale_t new_locale;                                        \
  locale_t old_locale = NULL;                                 \
                                                              \
  current_locale = uselocale (NULL);                          \
  old_locale = duplocale (current_locale);                    \
  new_locale = newlocale (LC_NUMERIC_MASK, "C", old_locale);  \
  if (new_locale)                                             \
    uselocale (new_locale);

#else

#define PROTECT_FROM_LOCALE_BUG_START                         \
  gchar *saved_locale = NULL;                                 \
  const gchar *current_locale;                                \
  current_locale = setlocale(LC_NUMERIC, NULL);               \
  if (current_locale != NULL &&                               \
      g_str_has_prefix (current_locale, "fr_") == TRUE) {     \
    saved_locale = g_strdup (current_locale);                 \
    setlocale (LC_NUMERIC, "C");                              \
    GST_LOG_OBJECT (vosk, "Changed locale %s", saved_locale); \
  }

#endif

#if HAVE_USELOCALE

#define PROTECT_FROM_LOCALE_BUG_END                           \
  if (old_locale) {                                           \
    uselocale (current_locale);                               \
    freelocale (new_locale);                                  \
  }

#else

#define PROTECT_FROM_LOCALE_BUG_END                           \
  if (saved_locale != NULL) {                                 \
    setlocale (LC_NUMERIC, saved_locale);                     \
    GST_LOG_OBJECT (vosk, "Reset locale %s", saved_locale);   \
    g_free (saved_locale);                                    \
  }

#endif

/* the capabilities of the inputs and outputs. */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw,"
                     "format=S16LE,"
                     "rate=[1, MAX],"
                     "channels=1")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw,"
                     "format=S16LE,"
                     "rate=[1, MAX],"
                     "channels=1")
    );

#define gst_vosk_parent_class parent_class
G_DEFINE_TYPE (GstVosk, gst_vosk, GST_TYPE_ELEMENT);

static void
gst_vosk_set_property (GObject * object, guint prop_id,
                       const GValue * value, GParamSpec * pspec);
static void
gst_vosk_get_property (GObject * object, guint prop_id,
                       GValue * value, GParamSpec * pspec);

static GstStateChangeReturn
gst_vosk_change_state (GstElement *element,
                       GstStateChange transition);

static gboolean
gst_vosk_sink_event (GstPad * pad, GstObject * parent, GstEvent * event);

static GstFlowReturn
gst_vosk_chain (GstPad * pad, GstObject * parent, GstBuffer * buf);

static void
gst_vosk_result (GstVosk *vosk);

static void
gst_vosk_partial_result (GstVosk *vosk);

static void
gst_vosk_load_model_async (gpointer thread_data,
                           gpointer element);

/* Note : audio rate is handled by the application with the use of caps */

static void
gst_vosk_finalize (GObject *object)
{
  GstVosk *vosk = GST_VOSK (object);

  if (vosk->model_path) {
    g_free (vosk->model_path);
    vosk->model_path = NULL;
  }

  g_thread_pool_free(vosk->thread_pool, TRUE, TRUE);
  vosk->thread_pool=NULL;

  GST_DEBUG_OBJECT (vosk, "finalizing.");
}

static void
gst_vosk_set_num_alternatives(GstVosk *vosk)
{
  GST_VOSK_LOCK(vosk);

  if (vosk->recognizer)
    vosk_recognizer_set_max_alternatives (vosk->recognizer, vosk->alternatives);
  else
    GST_LOG_OBJECT (vosk, "No recognizer to set num alternatives.");

  GST_VOSK_UNLOCK(vosk);
}

static void
gst_vosk_class_init (GstVoskClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_vosk_set_property;
  gobject_class->get_property = gst_vosk_get_property;
  gobject_class->finalize = gst_vosk_finalize;

  g_object_class_install_property (gobject_class, PROP_SPEECH_MODEL,
      g_param_spec_string ("speech-model", "Speech Model", _("Location (path) of the speech model."),
          DEFAULT_SPEECH_MODEL, G_PARAM_READWRITE|GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_ALTERNATIVES,
      g_param_spec_int ("alternatives", "Alternative Number", _("Number of alternative results returned."),
          0, 100, DEFAULT_ALTERNATIVE_NUM, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_RESULT,
      g_param_spec_string ("final-results", "Get recognizer's final results", _("Force the recognizer to return final results."),
          NULL, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class, PROP_PARTIAL_RESULTS,
      g_param_spec_int64 ("partial-results", _("Minimum time interval between partial results"), _("Set the minimum time interval between partial results (in milliseconds). Set -1 to disable partial results."),
          -1,G_MAXINT64, 0, G_PARAM_READWRITE));

  gst_element_class_set_details_simple(gstelement_class,
    "vosk",
    "Filter/Audio",
    _("Performs speech recognition using libvosk"),
    "Philippe Rouquier <bonfire-app@wanadoo.fr>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));

  gstelement_class->change_state = gst_vosk_change_state;
}

static void
gst_vosk_init (GstVosk * vosk)
{
  vosk->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (vosk->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_vosk_sink_event));
  gst_pad_set_chain_function (vosk->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_vosk_chain));
  GST_PAD_SET_PROXY_CAPS (vosk->sinkpad);
  gst_element_add_pad (GST_ELEMENT (vosk), vosk->sinkpad);

  vosk->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  GST_PAD_SET_PROXY_CAPS (vosk->srcpad);
  gst_element_add_pad (GST_ELEMENT (vosk), vosk->srcpad);

  if (!gst_debug_is_active())
    vosk_set_log_level (-1);

  vosk->rate = 0.0;
  vosk->processed_size = 0;
  vosk->alternatives = DEFAULT_ALTERNATIVE_NUM;
  vosk->model_path = g_strdup(DEFAULT_SPEECH_MODEL);

  vosk->thread_pool=g_thread_pool_new((GFunc) gst_vosk_load_model_async,
                                      vosk,
                                      1,
                                      FALSE,
                                      NULL);
}

static gint
gst_vosk_get_rate(GstVosk *vosk)
{
  GstStructure *caps_struct;
  GstCaps *caps;
  gint rate = 0;

  caps=gst_pad_get_current_caps(vosk->sinkpad);
  if (caps == NULL) {
    GST_INFO_OBJECT (vosk, "no capabilities set on sink pad.");
    return 0;
  }

  caps_struct = gst_caps_get_structure (caps, 0);
  if (caps_struct == NULL) {
    GST_INFO_OBJECT (vosk, "no capabilities structure.");
    return 0;
  }

  if (gst_structure_get_int (caps_struct, "rate", &rate) == FALSE) {
    GST_INFO_OBJECT (vosk, "no rate set in the capabilities");
    return 0;
  }

  return rate;
}

/*
 * MUST be called with lock held
 */
static gboolean
gst_vosk_recognizer_new (GstVosk *vosk)
{
  vosk->rate = gst_vosk_get_rate(vosk);
  if (vosk->rate <= 0.0) {
    GST_INFO_OBJECT (vosk, "rate not set yet: no recognizer created.");
    return FALSE;
  }

  GST_INFO_OBJECT (vosk, "current rate is %f", vosk->rate);

  if (vosk->model == NULL) {
    GST_INFO_OBJECT (vosk, "no model provided.");
    return FALSE;
  }

  vosk->processed_size = 0;

  GST_INFO_OBJECT (vosk, "creating recognizer (rate = %f).", vosk->rate);
  vosk->recognizer = vosk_recognizer_new (vosk->model, vosk->rate);
  vosk_recognizer_set_max_alternatives (vosk->recognizer, vosk->alternatives);
  return TRUE;
}

static void
gst_vosk_message_new (GstVosk *vosk, const gchar *text_results)
{
  GstMessage *msg;
  GstStructure *contents;
  GValue value = G_VALUE_INIT;

  if (!text_results)
    return;

  contents = gst_structure_new_empty ("vosk");

  g_value_init (&value, G_TYPE_STRING);
  g_value_set_string (&value, text_results);

  gst_structure_set_value (contents, "current-result", &value);
  g_value_unset (&value);

  msg = gst_message_new_element (GST_OBJECT (vosk), contents);
  gst_element_post_message (GST_ELEMENT (vosk), msg);
}

/*
 * MUST be called with lock held
 */
static const gchar *
gst_vosk_final_result (GstVosk *vosk)
{
  const gchar *json_txt = NULL;

  GST_INFO_OBJECT(vosk, "getting final result");

  PROTECT_FROM_LOCALE_BUG_START

  if (G_UNLIKELY(vosk->recognizer == NULL)) {
    GST_DEBUG_OBJECT(vosk, "no recognizer available");
    goto end;
  }

  /* Avoid unnecessary operation if there is no data that have been processed.
   * We could even raise the threshold as a tenth of second would probably
   * yield no result either. */
  if (vosk->processed_size == 0) {
    GST_DEBUG_OBJECT(vosk, "no data processed");
    goto end;
  }

  if (vosk->prev_partial) {
    g_free (vosk->prev_partial);
    vosk->prev_partial = NULL;
  }

  json_txt = vosk_recognizer_final_result (vosk->recognizer);
  vosk->processed_size = 0;

end:

  PROTECT_FROM_LOCALE_BUG_END

  GST_INFO_OBJECT(vosk, "final results");

  if (!json_txt || !strcmp(json_txt, VOSK_EMPTY_TEXT_RESULT))
    return json_txt;

  return NULL;
}

static void
gst_vosk_reset (GstVosk *vosk)
{
  if (vosk->recognizer) {
    vosk_recognizer_free (vosk->recognizer);
    vosk->recognizer = NULL;
    vosk->processed_size = 0;
  }

  if (vosk->model != NULL) {
    vosk_model_free (vosk->model);
    vosk->model = NULL;
  }

  if (vosk->prev_partial) {
    g_free (vosk->prev_partial);
    vosk->prev_partial = NULL;
  }

  vosk->rate=0.0;
}

static void
gst_vosk_cancel_model_loading(GstVosk *vosk)
{
  GST_VOSK_LOCK(vosk);
  if (!vosk->current_operation)
    return;

  g_cancellable_cancel(vosk->current_operation);
  g_object_unref(vosk->current_operation);
  vosk->current_operation=NULL;

  g_cond_signal(&vosk->wake_stream);
  GST_VOSK_UNLOCK(vosk);
}

static GstStateChangeReturn
gst_vosk_check_model_path(GstElement *element)
{
  GstVosk *vosk = GST_VOSK (element);
  GstMessage *message;

  if (!vosk->model_path) {
      GST_ELEMENT_ERROR(vosk,
                        RESOURCE,
                        NOT_FOUND,
                        ("model could not be loaded"),
                        ("there is not model set"));
    return GST_STATE_CHANGE_FAILURE;
  }

  message = gst_message_new_async_start (GST_OBJECT_CAST (element));
  gst_element_post_message (element, message);
  return GST_STATE_CHANGE_ASYNC;
}

static GstStateChangeReturn
gst_vosk_change_state (GstElement *element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstVosk *vosk = GST_VOSK (element);

  GST_INFO_OBJECT (vosk, "State changed %s", gst_state_change_get_name(transition));
  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
    case GST_STATE_CHANGE_PAUSED_TO_PAUSED:
      ret=gst_vosk_check_model_path(element);
      if (ret == GST_STATE_CHANGE_FAILURE)
        return GST_STATE_CHANGE_FAILURE;
      break;

    case GST_STATE_CHANGE_PAUSED_TO_READY:
      /* This must be before GstElement's change_state default function. */
      gst_vosk_cancel_model_loading(vosk);

    default:
      break;
  }

  if (GST_ELEMENT_CLASS (parent_class)->change_state (element, transition) == GST_STATE_CHANGE_FAILURE){
    GST_DEBUG_OBJECT (vosk, "State change failure");
    return GST_STATE_CHANGE_FAILURE;
  }

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_READY:
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      /* Take the stream lock and wait for it to end */
      GST_PAD_STREAM_LOCK(vosk->sinkpad);
      gst_vosk_reset (vosk);
      GST_PAD_STREAM_UNLOCK(vosk->sinkpad);
      break;

    default:
      break;
  }

  GST_DEBUG_OBJECT (vosk, "State change completed");
  return ret;
}

static void
gst_vosk_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVosk *vosk = GST_VOSK (object);

  switch (prop_id) {
    case PROP_SPEECH_MODEL:
    {
      const gchar *model_path;

      model_path = g_value_get_string (value);
      GST_INFO_OBJECT (vosk, "new path for model %s", model_path);

      if(!g_strcmp0 (model_path, vosk->model_path))
        return;

      if (vosk->model_path)
        g_free (vosk->model_path);

      vosk->model_path = g_strdup (model_path);

      /* This property can only be changed in the READY state */
      break;
    }

    case PROP_ALTERNATIVES:
      if (vosk->alternatives == g_value_get_int (value))
        return;

      vosk->alternatives = g_value_get_int(value);
      gst_vosk_set_num_alternatives (vosk);
      break;

    case PROP_PARTIAL_RESULTS:
      vosk->partial_time_interval=g_value_get_int64(value) * GST_MSECOND;
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vosk_get_property (GObject *object,
                       guint prop_id,
                       GValue *prop_value,
                       GParamSpec *pspec)
{
  GstVosk *vosk = GST_VOSK (object);

  switch (prop_id) {
    case PROP_SPEECH_MODEL:
      g_value_set_string (prop_value, vosk->model_path);
      break;

    case PROP_ALTERNATIVES:
      g_value_set_int(prop_value, vosk->alternatives);
      break;

    /* FIXME: This should a function (+ GObject Introspection) */
    case PROP_RESULT:
      {
        const gchar *json_txt = NULL;

        /* Note : we are certain that json_txt is valid while we have the lock */
        GST_VOSK_LOCK(vosk);
        json_txt = gst_vosk_final_result(vosk);
        g_value_set_string (prop_value, json_txt);
        GST_VOSK_UNLOCK(vosk);
      }
      break;

    case PROP_PARTIAL_RESULTS:
      g_value_set_int64(prop_value, vosk->partial_time_interval / GST_MSECOND);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vosk_flush(GstVosk *vosk)
{
  GST_INFO_OBJECT (vosk, "flushing");

  GST_VOSK_LOCK(vosk);

  if (vosk->recognizer) {
    vosk_recognizer_reset(vosk->recognizer);
    vosk->processed_size = 0;
  }
  else
    GST_DEBUG_OBJECT (vosk, "no recognizer to flush");

  GST_VOSK_UNLOCK(vosk);
}

static gboolean
gst_vosk_sink_event (GstPad *pad,
                     GstObject *parent,
                     GstEvent * event)
{
  GstVosk *vosk;

  vosk = GST_VOSK (parent);

  GST_LOG_OBJECT (vosk, "Received %s event: %" GST_PTR_FORMAT,
                  GST_EVENT_TYPE_NAME (event), event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      gst_vosk_flush(vosk);
      break;

    case GST_EVENT_EOS:
    {
      const gchar *json_txt;

      /* Cancel any ongoing loading */
      gst_vosk_cancel_model_loading(vosk);

      /* Wait for the stream to complete */
      GST_PAD_STREAM_LOCK(vosk->sinkpad);
      json_txt = gst_vosk_final_result (vosk);
      gst_vosk_message_new (vosk, json_txt);
      GST_PAD_STREAM_UNLOCK(vosk->sinkpad);

      GST_DEBUG_OBJECT (vosk, "EOS stop event");
      break;
    }

    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

/*
 * The following functions are only called by gst_vosk_chain().
 * Which means that lock is held.
 */

static void
gst_vosk_result (GstVosk *vosk)
{
  const char *json_txt;

  PROTECT_FROM_LOCALE_BUG_START

  json_txt = vosk_recognizer_result (vosk->recognizer);

  PROTECT_FROM_LOCALE_BUG_END

  if (vosk->prev_partial) {
    g_free (vosk->prev_partial);
    vosk->prev_partial = NULL;
  }

  /* Don't send message if empty */
  if (!json_txt || !strcmp(json_txt, VOSK_EMPTY_TEXT_RESULT))
    return;

  gst_vosk_message_new (vosk, json_txt);
  vosk->processed_size = 0;
}

static void
gst_vosk_partial_result (GstVosk *vosk)
{
  const char *json_txt;

  /* NOTE: surprisingly this function can return "text" results. Mute them if
   * empty. */
  json_txt = vosk_recognizer_partial_result (vosk->recognizer);
  if (!json_txt ||
      !strcmp(json_txt, VOSK_EMPTY_PARTIAL_RESULT) ||
      !strcmp(json_txt, VOSK_EMPTY_TEXT_RESULT_ALT))
    return;

  /* To avoid posting message unnecessarily, make sure there is a change. */
  if (g_strcmp0 (json_txt, vosk->prev_partial) == 0)
      return;

  g_free (vosk->prev_partial);
  vosk->prev_partial = g_strdup (json_txt);

  gst_vosk_message_new (vosk, json_txt);
}

static void
gst_vosk_handle_buffer(GstVosk *vosk, GstBuffer *buf)
{
  GstClockTimeDiff diff_time;
  GstClockTime current_time;
  GstMapInfo info;
  int result;

  gst_buffer_map(buf, &info, GST_MAP_READ);
  if (G_UNLIKELY(info.size == 0))
    return;

  result = vosk_recognizer_accept_waveform (vosk->recognizer,
                                            (gchar*) info.data,
                                            info.size);
  if (result == -1) {
    GST_ERROR_OBJECT (vosk, "accept_waveform error");
    return;
  }
  vosk->processed_size += info.size;

  current_time = gst_element_get_current_running_time(GST_ELEMENT(vosk));
  diff_time = GST_CLOCK_DIFF(GST_BUFFER_PTS (buf), current_time);

  GST_LOG_OBJECT (vosk, "buffer time=%"GST_TIME_FORMAT" current time=%"GST_TIME_FORMAT" diff=%li " \
                  "(buffer size %lu)",
                  GST_TIME_ARGS(GST_BUFFER_PTS (buf)),
                  GST_TIME_ARGS(current_time),
                  diff_time,
                  info.size);

  /* We want to catch up when we are behind (500 milliseconds) but also try
   * to get a result now and again (every half second) at least.
   * Reminder : number of bytes per second = 16 bits * rate / 8 bits
   * so 1/100 of a second = number of bytes / 100.
   * It means 5 buffers approx. */
  if (diff_time > (GST_SECOND / 2)) {
    GST_DEBUG_OBJECT (vosk, "we are late %"GST_TIME_FORMAT", catching up (%lu)",
                     GST_TIME_ARGS(diff_time),
                     (vosk->processed_size % (guint64) vosk->rate));

    /* FIXME: this does not always work when the buffer size does not match
     * the size in bytes of a frame. */
    if ((vosk->processed_size % (guint64) vosk->rate) >= info.size)
      return;

    GST_INFO_OBJECT (vosk, "forcing result checking (consumed one second of data)");
  }

  if (result == 1) {
    GST_LOG_OBJECT (vosk, "checking result");
    gst_vosk_result(vosk);
    vosk->last_partial=GST_BUFFER_PTS (buf);
    return;
  }

  if (vosk->partial_time_interval < 0)
    return;

  diff_time=GST_CLOCK_DIFF(vosk->last_partial, GST_BUFFER_PTS (buf));
  if (vosk->partial_time_interval < diff_time) {
    GST_LOG_OBJECT (vosk, "checking partial result");
    gst_vosk_partial_result(vosk);
    vosk->last_partial=GST_BUFFER_PTS (buf);
  }
}

typedef struct {
  gchar *path;
  GCancellable *cancellable;
} GstVoskThreadData;

static void
gst_vosk_load_model_async (gpointer thread_data,
                           gpointer element)
{
  GstVoskThreadData *status = thread_data;
  GstVosk *vosk = GST_VOSK (element);
  GstMessage *message;
  VoskModel *model;

  /* There can be only one model loading at a time. Even when loading has been
   * cancelled for one model while it is waiting to be loaded.
   * In this latter case, wait for it to start, notice it was cancelled and
   * leave.*/

  /* Thread might have been cancelled while waiting, so check that */
  if (g_cancellable_is_cancelled (status->cancellable)) {
    GST_INFO_OBJECT (vosk, "model creation cancelled before even trying.");
    /* Note: don't use condition, it's not our problem any more */
    goto clean;
  }

  GST_INFO_OBJECT (vosk, "creating model %s.", status->path);

  /* This is why we do all this. Depending on the model size it can take a long
   * time before it returns. */
  model = vosk_model_new (status->path);

  GST_VOSK_LOCK(vosk);

  /* The next statement should be protected with a lock so that we are not
   * cancelled right after checking cancel status and before setting model.
   * Otherwise, a stale model could remain. */
  if (g_cancellable_is_cancelled (status->cancellable)) {
    GST_VOSK_UNLOCK(vosk);

    GST_INFO_OBJECT (vosk, "model creation cancelled %s.", status->path);
    vosk_model_free (model);

    /* Note: don't use condition, not our problem anymore */
    goto clean;
  }

  /* Do this here to make sure the following is still relevant */
  if (!model) {
    GST_VOSK_UNLOCK(vosk);

    GST_ERROR_OBJECT(vosk, "could not create model object for %s.", status->path);
    GST_ELEMENT_ERROR(GST_ELEMENT(vosk),
                      RESOURCE,
                      NOT_FOUND,
                      ("model could not be loaded"),
                      ("an error was encountered while loading model (%s)", status->path));

    GST_STATE_LOCK(vosk);
    gst_element_abort_state (GST_ELEMENT(vosk));
    GST_STATE_UNLOCK(vosk);

    goto out_condition;
  }

  GST_INFO_OBJECT (vosk, "model ready (%s).", status->path);

  /* This is the only place where vosk->model can be set and only one thread
   * at a time can do it. Same for recognizer. */
  vosk->model = model;
  gst_vosk_recognizer_new(vosk);

  GST_VOSK_UNLOCK(vosk);

  GST_INFO_OBJECT (vosk, "async state change successfully completed.");
  message = gst_message_new_async_done (GST_OBJECT_CAST (vosk),
                                        GST_CLOCK_TIME_NONE);
  gst_element_post_message (element, message);

  /* This is needed to change the state of the element (and the pipeline).*/
  GST_STATE_LOCK (element);
  gst_element_continue_state (element, GST_STATE_CHANGE_SUCCESS);
  GST_STATE_UNLOCK (element);

out_condition:

  /* Wake streaming thread, unless we were cancelled */
  g_cond_signal(&vosk->wake_stream);

clean:

  g_cancellable_cancel(status->cancellable);
  g_object_unref(status->cancellable);
  g_free(status->path);
  g_free(status);
}

static GstFlowReturn
gst_vosk_chain (GstPad *sinkpad,
                GstObject *parent,
                GstBuffer *buf)
{
  GstVosk *vosk = GST_VOSK (parent);

  GST_LOG_OBJECT (vosk, "data received");

  GST_VOSK_LOCK(vosk);

  if (G_UNLIKELY(!vosk->recognizer)) {
    GstVoskThreadData *thread_data;

    /* Start loading a new model */
    vosk->current_operation=g_cancellable_new();

    thread_data=g_new0(GstVoskThreadData, 1);
    thread_data->cancellable=g_object_ref(vosk->current_operation);
    thread_data->path=g_strdup(vosk->model_path);
    g_thread_pool_push(vosk->thread_pool,
                       thread_data,
                       NULL);

    /* Block until the thread finishes or until it is cancelled */
    g_cond_wait(&vosk->wake_stream, &vosk->RecMut);
    GST_INFO_OBJECT (vosk, "woken up, model should be ready");
    if (!vosk->model || !vosk->recognizer) {
      GST_VOSK_UNLOCK(vosk);
      return GST_FLOW_OK;
    }
  }

  gst_vosk_handle_buffer(vosk, buf);

  GST_VOSK_UNLOCK(vosk);

  GST_LOG_OBJECT (vosk, "chaining data");
  gst_buffer_ref(buf);
  return gst_pad_push (vosk->srcpad, buf);
}

gboolean
gst_vosk_plugin_init (GstPlugin *vosk_plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_vosk_debug, "vosk",
      0, "Performs speech recognition using libvosk");

  return gst_element_register (vosk_plugin, "vosk", GST_RANK_NONE, GST_TYPE_VOSK);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    vosk,
    "Performs speech recognition using libvosk",
    gst_vosk_plugin_init,
    PACKAGE_VERSION,
    PACKAGE_LICENSE,
    PACKAGE,
    "http://gstreamer.net/"
)
