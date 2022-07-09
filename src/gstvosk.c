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

#define GST_VOSK_LOAD_LOCK(vosk) (g_mutex_lock(&vosk->LoadMut))
#define GST_VOSK_LOAD_UNLOCK(vosk) (g_mutex_unlock(&vosk->LoadMut))

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
};

/*
 * gst-launch-1.0 -m pulsesrc ! queue2 max-size-time=0 max-size-buffers=0 \
 *                   max-size-bytes=4294967294 ! audioconvert ! audiorate ! \
 *                   audioresample ! audio/x-raw,format=S16LE,rate=44100, \
 *                   channels=1! vosk alternatives=0 speech-model=path/to/model \
 *                   ! fakesink
*/

/* the capabilities of the inputs and outputs. */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw,"
                     "format=S16LE,"
                     "rate={ 8000, 11250, 22500, 32000, 44100, 48000, 96000 },"
                     "channels=1")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw,"
                     "format=S16LE,"
                     "rate={ 8000, 11250, 22500, 32000, 44100, 48000, 96000 },"
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

/* Note : audio rate is handled by the application with the use of caps */

/*
 * Needs to be called with the lock.
 */
static inline void
gst_vosk_cancel_current_operation(GstVosk *vosk)
{
  if (vosk->current_operation) {
    /* Cancel current operation */
    g_cancellable_cancel(vosk->current_operation);
    g_object_unref(vosk->current_operation);
    vosk->current_operation = NULL;
  }
}

static void
gst_vosk_reset (GstVosk *vosk)
{
  GST_VOSK_LOCK (vosk);

  if (vosk->prev_partial)
    g_free (vosk->prev_partial);

  vosk->prev_partial = NULL;

  gst_vosk_cancel_current_operation(vosk);

  g_queue_clear_full(&vosk->buffer, (GDestroyNotify) gst_buffer_unref);
  vosk->buffering = FALSE;

  if (vosk->recognizer)
    vosk_recognizer_free (vosk->recognizer);

  vosk->recognizer = NULL;

  vosk->processed_size = 0;

  if (vosk->model != NULL)
    vosk_model_free (vosk->model);

  vosk->model = NULL;

  GST_VOSK_UNLOCK(vosk);
}

static void
gst_vosk_finalize (GObject *object)
{
  GstVosk *vosk = GST_VOSK (object);

  if (vosk->model_path)
    g_free (vosk->model_path);

  vosk->model_path = NULL;

  gst_vosk_reset(vosk);

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
          DEFAULT_SPEECH_MODEL, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_ALTERNATIVES,
      g_param_spec_int ("alternatives", "Alternative Number", _("Number of alternative results returned."),
          0, 100, DEFAULT_ALTERNATIVE_NUM, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_RESULT,
      g_param_spec_string ("final-results", "Get recognizer's final results", _("Force the recognizer to return final results."),
          NULL, G_PARAM_READABLE));

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
  vosk->prev_partial = NULL;
  vosk->model_path = g_strdup(DEFAULT_SPEECH_MODEL);
  vosk->recognizer = NULL;
  vosk->model = NULL;

  vosk->num_loading_threads=0;

  g_queue_init(&vosk->buffer);
}

static gint
gst_vosk_get_rate(GstVosk *vosk)
{
  GstStructure *caps_struct;
  GstCaps *caps;
  gint rate = 0;

  /* When calling this function, the caller must ensure (by taking the lock if
   * need be that the model provided won't be destroyed by another thread */
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
 * MUST be called with LOCK held
 */
static VoskRecognizer *
gst_vosk_recognizer_new (GstVosk *vosk,
                         VoskModel *model,
                         gfloat rate)
{
  VoskRecognizer *recognizer;

  if (rate <= 0.0)
    rate = (gfloat) gst_vosk_get_rate(vosk);

  if (rate <= 0.0) {
    GST_INFO_OBJECT (vosk, "rate == 0");
    return NULL;
  }

  GST_INFO_OBJECT (vosk, "current rate is %f", rate);

  if (model == NULL) {
    GST_INFO_OBJECT (vosk, "no model provided.");
    return NULL;
  }

  vosk->rate = rate;
  vosk->processed_size = 0;

  GST_INFO_OBJECT (vosk, "creating recognizer (rate = %f).", rate);
  recognizer = vosk_recognizer_new (model, rate);
  vosk_recognizer_set_max_alternatives (recognizer, vosk->alternatives);

  return recognizer;
}

/*
 * MUST be called with LOCK held.
 */
static void
gst_vosk_clean_current_model(GstVosk *vosk)
{
  if (vosk->recognizer) {
    vosk_recognizer_free (vosk->recognizer);
    vosk->recognizer = NULL;
    vosk->processed_size = 0;
  }

  if (vosk->model) {
    vosk_model_free (vosk->model);
    vosk->model = NULL;
  }

  vosk->rate=0.0;
}

static gboolean
gst_vosk_load_model_real (GstVosk *vosk,
                          GCancellable *status)
{
  gchar *model_path = NULL;
  gboolean ret = TRUE;
  VoskModel *model;

  /* Ensure the path of the model will not get destroyed */
  GST_VOSK_LOCK (vosk);
  gst_vosk_clean_current_model(vosk);
  model_path = g_strdup (vosk->model_path);

  GST_INFO_OBJECT (vosk, "creating model %s.", model_path);

  GST_VOSK_UNLOCK(vosk);

  /* This is why we do all this. Depending on the model size it can take a long
   * time before it returns. */
  model = vosk_model_new (model_path);

  GST_VOSK_LOCK (vosk);

  if (model == NULL) {
    GST_ERROR_OBJECT(vosk, "could not create model %s.", model_path);
    ret=FALSE;
    goto out;
  }

  if (g_cancellable_is_cancelled (status)) {
    GST_INFO_OBJECT (vosk, "model creation cancelled %s.", model_path);
    vosk_model_free (model);
    ret=FALSE;
    goto out;
  }

  GST_INFO_OBJECT (vosk, "model created %s.", model_path);

  /* That should not happen of there is an error in the code. Nothing but this
   * function should be able to create a model !! And again, one thread at a
   * time. These tests are just to check. */
  if (G_UNLIKELY(vosk->model != NULL))
    GST_ERROR_OBJECT (vosk, "model is not NULL.");

  if (G_UNLIKELY(vosk->recognizer != NULL))
    GST_ERROR_OBJECT (vosk, "recognizer is not NULL.");

  /* This is the only place where vosk->model can be set and only one thread
   * at a time can do it. */
  vosk->model = model;

  vosk->recognizer = gst_vosk_recognizer_new (vosk, model, 0.0);
  if (vosk->recognizer == NULL)
    GST_INFO_OBJECT (vosk, "rate not set yet: no recognizer created.");

out:

  g_free (model_path);

  vosk->num_loading_threads --;
  if (vosk->num_loading_threads == 0)
    vosk->buffering = FALSE;

  GST_VOSK_UNLOCK (vosk);
  return (ret == TRUE && (g_cancellable_is_cancelled(status) == FALSE));
}

static void
gst_vosk_load_model_async (GstElement *element,
                           gpointer user_data)
{
  GCancellable *status = G_CANCELLABLE (user_data);
  GstVosk *vosk = GST_VOSK (element);
  GstMessage *message;

  /* There can be only one model loading at a time. Even when loading has been
   * cancelled for one model, wait for it to notice it was cancelled and leave.
   */
  GST_VOSK_LOAD_LOCK(vosk);

  /* We might have been waiting so check that before starting */
  if (g_cancellable_is_cancelled (status)) {
    GST_VOSK_LOAD_UNLOCK(vosk);
    return;
  }

  if (!gst_vosk_load_model_real (vosk, status)) {
    /* If we failed or were cancelled, move back to READY state
     * if vosk->model == NULL, unless there is another thread waiting to load
     * a new model. */

    GST_INFO_OBJECT (vosk, "creation failed or was cancelled");

    GST_VOSK_LOCK (vosk);

    if (vosk->num_loading_threads == 0) {
      /* vosk->model MUST be NULL since we fail */
      GST_VOSK_UNLOCK (vosk);

      GST_STATE_LOCK (element);
      gst_element_abort_state (element);
      GST_STATE_UNLOCK (element);

      gst_element_set_state(element, GST_STATE_READY);
    }
    else
      GST_VOSK_UNLOCK (vosk);

    GST_VOSK_LOAD_UNLOCK(vosk);
    return;
  }

  GST_STATE_LOCK (element);
  gst_element_continue_state (element, GST_STATE_CHANGE_SUCCESS);
  GST_STATE_UNLOCK (element);

  GST_INFO_OBJECT (vosk, "async state change successfully completed %i.", GST_STATE_RETURN(vosk));

  /* FIXME: do we need abort/continue AND the following message ?? */
  message = gst_message_new_async_done (GST_OBJECT_CAST (vosk),
                                        GST_CLOCK_TIME_NONE);
  gst_element_post_message (element, message);

  GST_VOSK_LOAD_UNLOCK(vosk);
}

/*
 * MUST be called with LOCK held and vosk->model_path != NULL
 */
static GstStateChangeReturn
gst_vosk_load_model (GstVosk *vosk)
{
  GstMessage *message;

  /* Note: case where vosk->model_path == NULL should be handled outside that
   * function when we don't hold the lock and we can set the state */
  if (vosk->model_path == NULL) {
    GST_ERROR_OBJECT (vosk, "model path is NULL.");
    return GST_STATE_CHANGE_FAILURE;
  }

  GST_DEBUG_OBJECT (vosk, "num loading threads %i", vosk->num_loading_threads);

  if (vosk->num_loading_threads == 2) {
    /* Only allow two threads max, at most one running (to be cancelled) and
     * the other one waiting. When the one waiting will wake up, it will pick
     * the new model path.*/
    GST_DEBUG_OBJECT (vosk, "not creating a new thread, waiting one will take care of loading new path");
    return GST_STATE_CHANGE_SUCCESS;
  }

  /* Start buffering */
  vosk->buffering = TRUE;
  vosk->num_loading_threads ++;

  /* Cancel ongoing operation if any and start a new one */
  gst_vosk_cancel_current_operation (vosk);
  vosk->current_operation = g_cancellable_new();

  gst_element_call_async (GST_ELEMENT(vosk),
                          gst_vosk_load_model_async,
                          g_object_ref (vosk->current_operation),
                          g_object_unref);

  message = gst_message_new_async_start (GST_OBJECT_CAST (vosk));
  gst_element_post_message (GST_ELEMENT (vosk), message);
  return GST_STATE_CHANGE_ASYNC;
}

static void
gst_vosk_message_new (GstVosk *vosk, const gchar *text_results)
{
  GstMessage *msg;
  GstStructure *contents;
  GValue value = G_VALUE_INIT;

  contents = gst_structure_new_empty ("vosk");

  g_value_init (&value, G_TYPE_STRING);
  g_value_set_string (&value, text_results);

  gst_structure_set_value (contents, "current-result", &value);
  g_value_unset (&value);

  msg = gst_message_new_element (GST_OBJECT (vosk), contents);
  gst_element_post_message (GST_ELEMENT (vosk), msg);
}

/*
 * MUST be called with LOCK held
 */
static const gchar *
gst_vosk_final_result (GstVosk *vosk)
{
  const gchar *json_txt = NULL;

  GST_INFO_OBJECT(vosk, "getting final result");

#if HAVE_USELOCALE
  // BUG : protect from local formatting errors when fr_ prefix
  // Maybe there are other locales ?
  // Use uselocale () when we can as it is supposed to be safer since it sets
  // the locale only for the thread.

  locale_t current_locale;
	locale_t new_locale;
	locale_t old_locale = NULL;

  current_locale = uselocale (NULL);
  old_locale = duplocale (current_locale);
  new_locale = newlocale (LC_NUMERIC_MASK, "C", old_locale);
  if (new_locale)
    uselocale (new_locale);

#else
  gchar *saved_locale = NULL;
  const gchar *current_locale;

  current_locale = setlocale(LC_NUMERIC, NULL);
  if (current_locale != NULL &&
      g_str_has_prefix (current_locale, "fr_") == TRUE) {
    saved_locale = g_strdup (current_locale);
    setlocale (LC_NUMERIC, "C");
    GST_LOG_OBJECT (vosk, "Changed locale %s", saved_locale);
  }
#endif

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

  #if HAVE_USELOCALE
  if (old_locale) {
    uselocale (current_locale);
    freelocale (new_locale);
  }

#else
  if (saved_locale != NULL) {
    setlocale (LC_NUMERIC, saved_locale);
    GST_LOG_OBJECT (vosk, "Reset locale %s", saved_locale);
    g_free (saved_locale);
  }
#endif

  GST_INFO_OBJECT(vosk, "final results");

  if (g_strcmp0 (json_txt, "") != 0)
    return json_txt;

  return NULL;
}

/*
 * Call with VOSK lock held
 */
static GstStateChangeReturn
gst_vosk_check_model_state(GstElement *element)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstVosk *vosk = GST_VOSK (element);

  if (vosk->model_path == NULL)
    return GST_STATE_CHANGE_FAILURE;

  if (!vosk->model)
    ret = gst_vosk_load_model (vosk);
  else
    ret = GST_STATE_CHANGE_SUCCESS;

  return ret;
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
      GST_VOSK_LOCK (vosk);
      ret=gst_vosk_check_model_state(element);
      GST_VOSK_UNLOCK (vosk);

      if (ret == GST_STATE_CHANGE_FAILURE)
        return GST_STATE_CHANGE_FAILURE;

    default:
      break;
  }

  if (GST_ELEMENT_CLASS (parent_class)->change_state (element, transition) == GST_STATE_CHANGE_FAILURE)
    return GST_STATE_CHANGE_FAILURE;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_NULL:
    case GST_STATE_CHANGE_READY_TO_NULL:
    case GST_STATE_CHANGE_READY_TO_READY:
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_vosk_reset (vosk);
      break;

    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
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

      GST_DEBUG_OBJECT (vosk, "model change %s \n%i %i %i %i %i",
                        model_path,
                        GST_STATE(vosk),
                        GST_STATE_NEXT(vosk),
                        GST_STATE_TARGET(vosk),
                        GST_STATE_PENDING(vosk),
                        GST_STATE_RETURN(vosk));

      GST_VOSK_LOCK (vosk);

      if(!g_strcmp0 (model_path, vosk->model_path)) {
        GST_VOSK_UNLOCK(vosk);
        return;
      }

      if (vosk->model_path)
        g_free (vosk->model_path);

      vosk->model_path = g_strdup (model_path);

      if (vosk->model_path == NULL) {
        gst_vosk_clean_current_model(vosk);
        gst_vosk_cancel_current_operation(vosk);

        /* Deal with the cases when we are supposed to be in a PAUSED or PLAYING
         * states */
        GST_VOSK_UNLOCK (vosk);

        if (GST_STATE(vosk) >= GST_STATE_PAUSED ||
            GST_STATE_PENDING(vosk) >= GST_STATE_PAUSED)
          gst_element_set_state(GST_ELEMENT(vosk), GST_STATE_READY);

        return;
      }

      /* See if we need to update our model. Only load a model without changing
       * state if we have one already of if state/state pending are at least
       * paused.
       * Note: we could have vosk->model == NULL and state pending == PAUSED
       * if we are transitionning from READY and the model is being loaded.
       * But in this case, the call to sync_state_with_parent will do the same
       * as below (which means state == PAUSED or PLAYING). */
      if (vosk->model != NULL ||
          GST_STATE(vosk) >= GST_STATE_PAUSED ||
          GST_STATE_PENDING(vosk) >= GST_STATE_PAUSED) {
        GST_DEBUG_OBJECT(vosk, "model is not NULL");

        /* Should we tell that we are not available ?? gst_element_lost_state
         * move to READY state ? */
        gst_vosk_load_model(vosk);
        GST_VOSK_UNLOCK(vosk);

       return;
      }

      GST_VOSK_UNLOCK(vosk);

      GST_DEBUG_OBJECT(vosk, "sync with parent state after model change (from NULL)");
      gst_element_sync_state_with_parent (GST_ELEMENT(vosk));
      break;
    }

    case PROP_ALTERNATIVES:
      if (vosk->alternatives == g_value_get_int (value))
        return;

      vosk->alternatives = g_value_get_int(value);
      gst_vosk_set_num_alternatives (vosk);
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

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_vosk_set_caps (GstVosk *vosk, GstCaps *caps)
{
  GstStructure *caps_struct;
  gboolean success;
  GstCaps *outcaps;
  gchar *caps_str;
  int rate = 0;

  caps_struct = gst_caps_get_structure (caps, 0);
  if (gst_structure_get_int (caps_struct, "rate", &rate) == FALSE)
    return FALSE;

  GST_VOSK_LOCK (vosk);

  GST_INFO_OBJECT (vosk, "got rate %i", rate);

  /* See that we have a model */
  if (vosk->model) {
    const gchar *json_txt;

    /* We should have both at any moment */
    if (vosk->recognizer != NULL) {
      if ((gfloat) rate != vosk->rate) {
        GST_INFO_OBJECT (vosk, "rate has changed; updating recognizer.");

        /* Signal what we have recognized so far */
        json_txt = gst_vosk_final_result (vosk);
        gst_vosk_message_new (vosk, json_txt);

        vosk_recognizer_free (vosk->recognizer);
        vosk->recognizer = gst_vosk_recognizer_new (vosk, vosk->model, rate);
        vosk->processed_size = 0;
      }
      else
        GST_INFO_OBJECT (vosk, "rate has not changed; keeping current model and recognizer.");
    }
    else {
      GST_INFO_OBJECT (vosk, "no recognizer yet available to set rate ; creating one.");
      vosk->recognizer = gst_vosk_recognizer_new (vosk, vosk->model, rate);
      vosk->processed_size = 0;
    }
  }
  /* See if we are creating a model. If we don't have a model, we don't have a
   * recognizer. */
  else {
    if (vosk->current_operation)
      GST_INFO_OBJECT (vosk, "model and recognizer are being created");
    else
      GST_INFO_OBJECT (vosk, "no model or recognizer ready to set rate yet");
  }

  GST_VOSK_UNLOCK (vosk);

  caps_str = g_strdup_printf ("audio/x-raw,"
                              "format=S16LE,"
                              "rate=%i,"
                              "channels=1", rate);
  outcaps = gst_caps_from_string (caps_str);
  g_free (caps_str);

  success = gst_pad_set_caps (vosk->srcpad, outcaps);
  gst_caps_unref (outcaps);

  return success;
}

static void
gst_vosk_flush(GstVosk *vosk)
{
  GST_INFO_OBJECT (vosk, "flushing");

  /* Take the lock, empty queue and flush our recognizer */
  GST_VOSK_LOCK (vosk);

  g_queue_clear_full(&vosk->buffer, (GDestroyNotify) gst_buffer_unref);
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
  gboolean ret;

  vosk = GST_VOSK (parent);

  GST_INFO_OBJECT (vosk, "Received %s event: %" GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps = NULL;
      GST_DEBUG_OBJECT (vosk, "caps event");
      gst_event_parse_caps (event, &caps);
      ret = gst_vosk_set_caps (vosk, caps);
      break;
    }

    case GST_EVENT_SEEK:
      GST_DEBUG_OBJECT (vosk, "seek event");
      ret = gst_pad_event_default (pad, parent, event);
      break;

    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (vosk, "flush start event");
      gst_vosk_flush(vosk);
      ret = gst_pad_event_default (pad, parent, event);
      break;

    case GST_EVENT_FLUSH_STOP:
      GST_DEBUG_OBJECT (vosk, "flush stop event");
      ret = gst_pad_event_default (pad, parent, event);
      break;

    case GST_EVENT_EOS:
    {
      const gchar *json_txt;

      GST_VOSK_LOCK(vosk);
      json_txt = gst_vosk_final_result (vosk);
      gst_vosk_message_new (vosk, json_txt);
      GST_VOSK_UNLOCK(vosk);

      GST_DEBUG_OBJECT (vosk, "EOS stop event");
      ret = gst_pad_event_default (pad, parent, event);
      break;
    }

    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }
  return ret;
}

/*
 * Must be called with lock held
 */
static void
gst_vosk_result (GstVosk *vosk)
{
  const char *json_txt;

#if HAVE_USELOCALE
  // BUG : protect from local formatting errors when fr_ prefix
  // Maybe there are other locales ?
  // Use uselocale () when we can as it is supposed to be safer since it sets
  // the locale only for the thread.

  locale_t current_locale;
	locale_t new_locale;
	locale_t old_locale = NULL;

  current_locale = uselocale (NULL);
  old_locale = duplocale (current_locale);
  new_locale = newlocale (LC_NUMERIC_MASK, "C", old_locale);
  if (new_locale)
    uselocale (new_locale);

#else
  gchar *saved_locale = NULL;
  const gchar *current_locale;

  current_locale = setlocale(LC_NUMERIC, NULL);
  if (current_locale != NULL &&
      g_str_has_prefix (current_locale, "fr_") == TRUE) {
    saved_locale = g_strdup (current_locale);
    setlocale (LC_NUMERIC, "C");
    GST_LOG_OBJECT (vosk, "Changed locale %s", saved_locale);
  }
#endif

  json_txt = vosk_recognizer_result (vosk->recognizer);

#if HAVE_USELOCALE
  if (old_locale) {
    uselocale (current_locale);
    freelocale (new_locale);
  }

#else
  if (saved_locale != NULL) {
    setlocale (LC_NUMERIC, saved_locale);
    GST_LOG_OBJECT (vosk, "Reset locale %s", saved_locale);
    g_free (saved_locale);
  }
#endif

  if (vosk->prev_partial) {
    g_free (vosk->prev_partial);
    vosk->prev_partial = NULL;
  }

  gst_vosk_message_new (vosk, json_txt);
  vosk->processed_size = 0;
}

static void
gst_vosk_partial_result (GstVosk *vosk)
{
  const char *json_txt;

  json_txt = vosk_recognizer_partial_result (vosk->recognizer);
  if (json_txt == NULL || g_strcmp0 (json_txt, "") == 0)
    return;

  // To avoid posting message unnecessarily, make sure there is a change.
  if (g_strcmp0 (json_txt, vosk->prev_partial) == 0)
      return;

  g_free (vosk->prev_partial);
  vosk->prev_partial = g_strdup (json_txt);

  gst_vosk_message_new (vosk, json_txt);
}

static void
gst_vosk_handle_buffer(GstVosk *vosk, GstBuffer *buf)
{
  GstMapInfo info;
  gchar *data;

  gst_buffer_map(buf, &info, GST_MAP_READ);
  data = (gchar *)info.data;

  if (info.size > 0) {
    int result;
    GstClockTimeDiff diff_time;

    diff_time = GST_CLOCK_DIFF(GST_BUFFER_PTS (buf), gst_element_get_current_running_time(GST_ELEMENT(vosk)));
    GST_DEBUG_OBJECT (vosk, "buffer time=%"GST_TIME_FORMAT" current time=%"GST_TIME_FORMAT" diff=%li " \
                      "(buffer size %lu)",
                      GST_TIME_ARGS(GST_BUFFER_PTS (buf)),
                      GST_TIME_ARGS(gst_element_get_current_running_time(GST_ELEMENT(vosk))),
                      diff_time,
                      info.size);

    result = vosk_recognizer_accept_waveform (vosk->recognizer, data, info.size);

    vosk->processed_size += info.size;

    /* We want to catch up when we are behind (500 milliseconds) but also try
     * to get a result now and again (every half second) at least.
     * See reminder below to calculate number of bytes for a second. */
    if (diff_time > (GST_SECOND / 2)) {
      GST_INFO_OBJECT (vosk, "we are late %"GST_TIME_FORMAT", catching up (%lu)",
                       GST_TIME_ARGS(diff_time),
                       (vosk->processed_size % (guint64) vosk->rate));

      if ((vosk->processed_size % (guint64) vosk->rate) >= info.size)
        return;

      GST_INFO_OBJECT (vosk, "forcing result checking (consumed one second of data)");
    }
    /* Make sure it's not underfed either (50 milliseconds).
     * Reminder : number of bytes / per seconds = 16 bits * rate / 8 bits
     * so 1/100 of a second = number of bytes / 100.
     * It means 5 buffers approx. */
    else if (vosk->processed_size <= vosk->rate / 10) {
      GST_DEBUG_OBJECT (vosk, "we haven't processed enough before trying to get a result");
      return;
    }

    switch (result) {
      case 1 :
        GST_DEBUG_OBJECT (vosk, "checking result");
        gst_vosk_result(vosk);
        break;

      case 0 :
        GST_DEBUG_OBJECT (vosk, "checking partial result");
        gst_vosk_partial_result(vosk);
        break;

      default :
        GST_ERROR_OBJECT (vosk, "accept_waveform error");
        break;
    }
  }
}

/*
 * MUST be called with LOCK held. The function will be releasing it from time
 * to time.
 */
static void
gst_vosk_chain_empty_buffer (GstVosk *vosk)
{
  GstBuffer *buf;
  gint num_buf=0;

  GST_DEBUG_OBJECT (vosk, "emptying queue buffer.");

  /* Try to catch up, feed it everything and only check if we were cancelled
   * every two seconds worth of data (approx 200 buffers). It empties the queue.
   * We do this with hold of the mutex otherwise the queue will fill while we
   * empty it (see note below for a small exception to that). */

  buf = g_queue_pop_head (&vosk->buffer);
  while (buf != NULL) {
    gst_vosk_handle_buffer(vosk, buf);
    gst_buffer_unref(buf);

    if (num_buf++ > 10)
      break;

    buf = g_queue_pop_head(&vosk->buffer);
  }

  GST_INFO_OBJECT (vosk, "processed all buffers in the queue");
}

static GstFlowReturn
gst_vosk_chain (GstPad *sinkpad,
                GstObject *parent,
                GstBuffer *buf)
{
  GstVosk *vosk = GST_VOSK (parent);

  GST_LOG_OBJECT (vosk, "data received");

  GST_VOSK_LOCK (vosk);

  if (vosk->buffering == TRUE) {
    GST_DEBUG_OBJECT (vosk, "buffering");

    gst_buffer_ref(buf);
    g_queue_push_tail(&vosk->buffer, buf);
  }
  else if (vosk->recognizer != NULL) {
    /* Empty or buffer if it needs to */
    if (g_queue_is_empty(&vosk->buffer) == FALSE) {
      gst_buffer_ref(buf);
      g_queue_push_tail(&vosk->buffer, buf);

      gst_vosk_chain_empty_buffer(vosk);
    }
    else
      gst_vosk_handle_buffer(vosk, buf);
  }
  else
    GST_WARNING_OBJECT (vosk, "buffer could not be handled");

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
