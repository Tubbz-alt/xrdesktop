/*
 * xrdesktop
 * Copyright 2019 Collabora Ltd.
 * Author: Christoph Haag <christoph.haag@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#include "xrd-client.h"

#include <openvr-io.h>

#include "graphene-ext.h"

#include "xrd-pointer.h"
#include "xrd-pointer-tip.h"
#include "xrd-desktop-cursor.h"
#include "xrd-settings.h"
#include "xrd-controller.h"

#include "xrd-scene-client.h"
#include "xrd-overlay-client.h"

#include "xrd-container.h"

#include "xrd-math.h"

#include "xrd-button.h"

#define WINDOW_MIN_DIST .05f
#define WINDOW_MAX_DIST 15.f

enum {
  KEYBOARD_PRESS_EVENT,
  CLICK_EVENT,
  MOVE_CURSOR_EVENT,
  REQUEST_QUIT_EVENT,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

typedef struct _XrdClientPrivate
{
  GObject parent;

  OpenVRContext *context;
  XrdWindowManager *manager;
  OpenVRActionSet *wm_actions;
  XrdInputSynth *input_synth;

  XrdWindow *button_reset;
  XrdWindow *button_sphere;

  gboolean pinned_only;
  XrdWindow *pinned_button;

  gboolean selection_mode;
  XrdWindow *select_pinned_button;

  XrdWindow *keyboard_window;

  gulong keyboard_press_signal;
  gulong keyboard_close_signal;

  guint poll_runtime_event_source_id;
  guint poll_input_source_id;
  guint poll_input_rate_ms;

  double analog_threshold;

  double scroll_to_push_ratio;
  double scroll_to_scale_ratio;

  double pixel_per_meter;

  XrdDesktopCursor *cursor;

  VkImageLayout upload_layout;
  GHashTable *controllers;

  XrdContainer *wm_control_container;
} XrdClientPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (XrdClient, xrd_client, G_TYPE_OBJECT)

static void
xrd_client_finalize (GObject *gobject);

gboolean
_init_buttons (XrdClient *self, XrdController *controller);

static void
_destroy_buttons (XrdClient *self);

static void
xrd_client_class_init (XrdClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xrd_client_finalize;

  signals[KEYBOARD_PRESS_EVENT] =
    g_signal_new ("keyboard-press-event",
                   G_TYPE_FROM_CLASS (klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL, NULL, G_TYPE_NONE,
                   1, GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);

  signals[CLICK_EVENT] =
    g_signal_new ("click-event",
                   G_TYPE_FROM_CLASS (klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL, NULL, G_TYPE_NONE,
                   1, GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);

  signals[MOVE_CURSOR_EVENT] =
    g_signal_new ("move-cursor-event",
                   G_TYPE_FROM_CLASS (klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL, NULL, G_TYPE_NONE,
                   1, GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);

  signals[REQUEST_QUIT_EVENT] =
    g_signal_new ("request-quit-event",
                   G_TYPE_FROM_CLASS (klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL, NULL, G_TYPE_NONE,
                   1, GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);
}

void
xrd_client_set_upload_layout (XrdClient *self, VkImageLayout layout)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  priv->upload_layout = layout;
}

VkImageLayout
xrd_client_get_upload_layout (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  return priv->upload_layout;
}

/**
 * xrd_client_add_container:
 * @self: The #XrdClient
 * @container: The #XrdWindowContainer to add
 *
 * For a container to start behaving according to its layout and attachment,
 * it must be added to the client.
 *
 * Note: windows in the container must be added to the client separately with
 * xrd_client_add_window(), preferably with draggable set to false.
 */
void
xrd_client_add_container (XrdClient *self,
                          XrdContainer *container)
{
  XrdWindowManager *manager = xrd_client_get_manager (self);
  xrd_window_manager_add_container (manager, container);
}

void
xrd_client_remove_container (XrdClient *self,
                             XrdContainer *container)
{
  XrdWindowManager *manager = xrd_client_get_manager (self);
  xrd_window_manager_remove_container (manager, container);
}

/**
 * xrd_client_add_window:
 * @self: The #XrdClient
 * @window: The #XrdWindow to add
 * @draggable: Desktop windows should set this to true. This will enable the
 * expected interaction of being able to grab windows and drag them around.
 * It should be set to false for example for
 *  - child windows
 *  - windows in a container that is attached to the FOV, a controller, etc.
 */
void
xrd_client_add_window (XrdClient *self,
                       XrdWindow *window,
                       gboolean   draggable)
{
  XrdWindowFlags flags = XRD_WINDOW_HOVERABLE | XRD_WINDOW_DESTROY_WITH_PARENT;

  /* User can't drag child windows, they are attached to the parent.
   * The child window's position is managed by its parent, not the WM. */
  if (draggable)
    flags |= XRD_WINDOW_DRAGGABLE | XRD_WINDOW_MANAGED;

  XrdWindowManager *manager = xrd_client_get_manager (self);

  xrd_window_manager_add_window (manager, window, flags);
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  if (priv->pinned_only &&
      !(flags & XRD_WINDOW_MANAGER_BUTTON) &&
      !xrd_window_manager_is_pinned (manager, window))
    {
      xrd_window_hide (window);
    }

  xrd_client_add_window_callbacks (self, window);
}

/**
 * xrd_client_button_new_from_text:
 * @self: The #XrdClient
 * @width: Width in meters.
 * @height: Height in meters.
 * @label_count: The number of text lines given in @label
 * @label: One or more lines of text that will be displayed on the button.
 *
 * Creates a button and submits a Cairo rendered text label to it.
 */

XrdWindow*
xrd_client_button_new_from_text (XrdClient *self,
                                 float      width,
                                 float      height,
                                 int        label_count,
                                 gchar    **label)
{
  GString *full_label = g_string_new ("");
  for (int i = 0; i < label_count; i++)
    {
      g_string_append (full_label, label[i]);
      if (i < label_count - 1)
        g_string_append (full_label, " ");
    }

  XrdWindow *button =
    xrd_client_window_new_from_meters (self, full_label->str, width, height);

  g_string_free (full_label, FALSE);

  if (button == NULL)
    {
      g_printerr ("Could not create button.\n");
      return NULL;
    }

  GulkanClient *gc = xrd_client_get_uploader (self);
  VkImageLayout layout = xrd_client_get_upload_layout (self);

  xrd_button_set_text (button, gc, layout, label_count, label);

  return button;
}

XrdWindow*
xrd_client_button_new_from_icon (XrdClient   *self,
                                 float        width,
                                 float        height,
                                 const gchar *url)
{
  XrdWindow *button =
    xrd_client_window_new_from_meters (self, url, width, height);

  if (button == NULL)
    {
      g_printerr ("Could not create button.\n");
      return NULL;
    }

  GulkanClient *gc = xrd_client_get_uploader (self);
  VkImageLayout layout = xrd_client_get_upload_layout (self);

  xrd_button_set_icon (button, gc, layout, url);

  return button;
}

/**
 * xrd_client_add_button:
 * @self: The #XrdClient
 * @button: The button (#XrdWindow) to add.
 * @position: World space position of the button.
 * @press_callback: A function that will be called when the button is grabbed.
 * @press_callback_data: User pointer passed to @press_callback.
 *
 * Buttons are special windows that can not be grabbed and dragged around.
 * Instead a button's press_callback is called on the grab action.
 */
void
xrd_client_add_button (XrdClient          *self,
                       XrdWindow          *button,
                       graphene_point3d_t *position,
                       GCallback           press_callback,
                       gpointer            press_callback_data)
{
  graphene_matrix_t transform;
  graphene_matrix_init_translate (&transform, position);
  xrd_window_set_transformation (button, &transform);

  XrdWindowManager *manager = xrd_client_get_manager (self);
  xrd_window_manager_add_window (manager,
                                 button,
                                 XRD_WINDOW_HOVERABLE |
                                 XRD_WINDOW_DESTROY_WITH_PARENT |
                                 XRD_WINDOW_MANAGER_BUTTON);

  g_signal_connect (button, "grab-start-event",
                    (GCallback) press_callback, press_callback_data);

  xrd_client_add_button_callbacks (self, button);
}

void
xrd_client_set_pin (XrdClient *self,
                    XrdWindow *win,
                    gboolean pin)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  xrd_window_manager_set_pin (priv->manager, win, pin);
}

void
xrd_client_show_pinned_only (XrdClient *self,
                             gboolean pinned_only)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  priv->pinned_only = pinned_only;
  xrd_window_manager_show_pinned_only (priv->manager, pinned_only);

  GulkanClient *client = xrd_client_get_uploader (self);
  VkImageLayout layout = xrd_client_get_upload_layout (self);
  if (pinned_only)
    {
      xrd_button_set_icon (priv->pinned_button, client, layout,
                           "/icons/object-hidden-symbolic.svg");
    }
  else
    {
      xrd_button_set_icon (priv->pinned_button, client, layout,
                           "/icons/object-visible-symbolic.svg");
    }
}

static void
_device_activate_cb (OpenVRContext          *context,
                     OpenVRDeviceIndexEvent *event,
                     gpointer               _self);

static void
_activate_controller (XrdClient *self,
                      guint64 controller_handle)
{
  OpenVRContext *context = openvr_context_get_instance ();
  OpenVRDeviceIndexEvent event = {
    .controller_handle = controller_handle
  };
  _device_activate_cb (context, &event, self);
}

static XrdController *
_lookup_controller (XrdClient *self,
                    guint64    controller_handle)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdController *controller = g_hash_table_lookup (priv->controllers,
                                                   &controller_handle);
  return controller;
}

/**
 * xrd_client_get_keyboard_window
 * @self: The #XrdClient
 *
 * Returns: The window that is currently used for keyboard input. Can be NULL.
 */
XrdWindow *
xrd_client_get_keyboard_window (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  return priv->keyboard_window;
}

GulkanClient *
xrd_client_get_uploader (XrdClient *self)
{
  XrdClientClass *klass = XRD_CLIENT_GET_CLASS (self);
  if (klass->get_uploader == NULL)
      return FALSE;
  return klass->get_uploader (self);
}

void
xrd_client_init_controller (XrdClient *self,
                            XrdController *controller)
{
  XrdClientClass *klass = XRD_CLIENT_GET_CLASS (self);
  if (klass->init_controller == NULL)
      return;
  klass->init_controller (self, controller);
}

/**
 * xrd_client_get_synth_hovered:
 * @self: The #XrdClient
 *
 * Returns: If the controller used for synthesizing input is hovering over an
 * #XrdWindow, return this window, else NULL.
 */
XrdWindow *
xrd_client_get_synth_hovered (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  guint64 controller_handle =
    xrd_input_synth_synthing_controller (priv->input_synth);
  XrdController *controller = _lookup_controller (self, controller_handle);

  /* no controller, nothing hovered */
  if (controller == NULL)
    return NULL;

  XrdWindow *parent = xrd_controller_get_hover_state (controller)->window;
  return parent;
}

/**
 * xrd_client_submit_cursor_texture:
 * @self: The #XrdClient
 * @client: A GulkanClient, for example an OpenVROverlayUploader.
 * @texture: A GulkanTexture that is created and owned by the caller.
 * For performance reasons it is a good idea for the caller to reuse this
 * texture.
 * @hotspot_x: The x component of the hotspot.
 * @hotspot_y: The x component of the hotspot.
 *
 * A hotspot of (x, y) means that the hotspot is at x pixels right, y pixels
 * down from the top left corner of the texture.
 */
void
xrd_client_submit_cursor_texture (XrdClient     *self,
                                  GulkanClient  *client,
                                  GulkanTexture *texture,
                                  int            hotspot_x,
                                  int            hotspot_y)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  xrd_desktop_cursor_submit_texture (priv->cursor, client, texture,
                                     hotspot_x, hotspot_y);
}

void
xrd_client_emit_keyboard_press (XrdClient *self,
                                GdkEventKey *event)
{
  g_signal_emit (self, signals[KEYBOARD_PRESS_EVENT], 0, event);
}

void
xrd_client_emit_click (XrdClient *self,
                       XrdClickEvent *event)
{
  g_signal_emit (self, signals[CLICK_EVENT], 0, event);
}

void
xrd_client_emit_move_cursor (XrdClient *self,
                             XrdMoveCursorEvent *event)
{
  g_signal_emit (self, signals[MOVE_CURSOR_EVENT], 0, event);
}

void
xrd_client_emit_system_quit (XrdClient *self,
                             OpenVRQuitEvent *event)
{
  g_signal_emit (self, signals[REQUEST_QUIT_EVENT], 0, event);
}

XrdClient *
xrd_client_new (void)
{
  return (XrdClient*) g_object_new (XRD_TYPE_CLIENT, 0);
}

GSList *
xrd_client_get_windows (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  return xrd_window_manager_get_windows (priv->manager);
}

static void
xrd_client_finalize (GObject *gobject)
{
  XrdClient *self = XRD_CLIENT (gobject);
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  if (priv->wm_control_container)
    _destroy_buttons (self);

  if (priv->poll_runtime_event_source_id > 0)
    g_source_remove (priv->poll_runtime_event_source_id);
  if (priv->poll_input_source_id > 0)
    g_source_remove (priv->poll_input_source_id);

  g_object_unref (priv->manager);
  g_clear_object (&priv->wm_actions);

  /* hash table unref also destroysControllers */
  g_hash_table_unref (priv->controllers);

  g_clear_object (&priv->cursor);
  g_clear_object (&priv->input_synth);

  g_clear_object (&priv->context);
  g_clear_object (&priv->wm_control_container);

  xrd_settings_destroy_instance ();

  G_OBJECT_CLASS (xrd_client_parent_class)->finalize (gobject);
}

OpenVRContext *
xrd_client_get_openvr_context (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  return priv->context;
}

XrdWindowManager *
xrd_client_get_manager (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  return priv->manager;
}

/**
 * xrd_client_save_reset_transform:
 * @self: The #XrdClient
 * @window: The #XrdWindow to save the current transform for. The reset
 * functionality of #XrdWindowManager will reset the transform of this window
 * to the transform the window has when this function is called.
 */
void
xrd_client_save_reset_transform (XrdClient *self,
                                 XrdWindow *window)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  xrd_window_manager_save_reset_transform (priv->manager, window);
}

/**
 * xrd_client_remove_window:
 * @self: The #XrdClient
 * @window: The #XrdWindow to remove.
 *
 * Removes an #XrdWindow from the management of the #XrdClient and the
 * #XrdWindowManager.
 * Note that the #XrdWindow will not be destroyed by this function.
 */
void
xrd_client_remove_window (XrdClient *self,
                          XrdWindow *window)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  xrd_window_manager_remove_window (priv->manager, window);

  GList *controllers = g_hash_table_get_values (priv->controllers);
  for (GList *l = controllers; l; l = l->next)
    {
      XrdController *controller = XRD_CONTROLLER (l->data);
      if (xrd_controller_get_hover_state (controller)->window ==
          XRD_WINDOW (window))
        {
          XrdControllerIndexEvent *hover_end_event =
              g_malloc (sizeof (XrdControllerIndexEvent));
          hover_end_event->controller_handle =
            xrd_controller_get_handle (controller);
          xrd_window_emit_hover_end (window, hover_end_event);

          xrd_controller_reset_hover_state (controller);
        }

      if (xrd_controller_get_grab_state (controller)->window ==
          XRD_WINDOW (window))
        xrd_controller_reset_grab_state (controller);
    }
  g_list_free(controllers);
}

OpenVRActionSet *
xrd_client_get_wm_actions (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  return priv->wm_actions;
}

XrdInputSynth *
xrd_client_get_input_synth (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  return priv->input_synth;
}

gboolean
xrd_client_poll_runtime_events (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  if (!priv->context)
    return FALSE;

  openvr_context_poll_event (priv->context);
  return TRUE;
}

gboolean
xrd_client_poll_input_events (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  if (!priv->context)
    {
      g_printerr ("Error polling events: No OpenVR Context\n");
      priv->poll_input_source_id = 0;
      return FALSE;
    }

  if (!openvr_action_set_poll (priv->wm_actions))
    {
      g_printerr ("Error polling wm actions\n");
      priv->poll_input_source_id = 0;
      return FALSE;
    }

  if (xrd_client_is_hovering (self) && !xrd_client_is_grabbing (self) &&
      !xrd_input_synth_poll_events (priv->input_synth))
    {
      g_printerr ("Error polling synth actions\n");
      priv->poll_input_source_id = 0;
      return FALSE;
    }

  xrd_window_manager_poll_window_events (priv->manager);

  return TRUE;
}

XrdDesktopCursor *
xrd_client_get_cursor (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  return priv->cursor;
}

static void
_action_hand_pose_cb (OpenVRAction            *action,
                      OpenVRPoseEvent         *event,
                      XrdClient               *self)
{
  (void) action;
  if (!event->device_connected || !event->valid || !event->active)
    {
      g_free (event);
      return;
    }

  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);
  if (controller == NULL)
    {
      g_print ("Pose callback: activating %lu\n", event->controller_handle);
      _activate_controller (self, event->controller_handle);
      controller = _lookup_controller (self, event->controller_handle);
    }


  if (controller == NULL)
    {
      g_free (event);
      return;
    }

  xrd_window_manager_update_pose (priv->manager, &event->pose, controller);

  xrd_pointer_move (xrd_controller_get_pointer (controller), &event->pose);

  XrdWindow *hovered_window =
    xrd_controller_get_hover_state (controller)->window;
  GSList *buttons = xrd_window_manager_get_buttons (priv->manager);

  gboolean hovering_window_for_input =
    hovered_window != NULL &&
    g_slist_find (buttons, hovered_window) == NULL;

  /* show cursor while synth controller hovers window, but doesn't grab */
  if (xrd_controller_get_handle (controller) ==
          xrd_input_synth_synthing_controller (priv->input_synth) &&
      hovering_window_for_input &&
      xrd_controller_get_grab_state (controller)->window == NULL)
    xrd_desktop_cursor_show (priv->cursor);

  g_free (event);
}

static void
_action_hand_pose_hand_grip_cb (OpenVRAction    *action,
                                OpenVRPoseEvent *event,
                                XrdClient       *self)
{
  (void) action;
  if (!event->device_connected || !event->valid || !event->active)
    {
      g_free (event);
      return;
    }

  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);

  if (controller == NULL)
    {
      g_free (event);
      return;
    }

  xrd_controller_update_pose_hand_grip (controller, &event->pose);
  g_free (event);
}


static void
_perform_push_pull (XrdClient *self,
                    XrdController *controller,
                    float push_pull_strength)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  HoverState *hover_state = xrd_controller_get_hover_state (controller);

  float new_dist =
    hover_state->distance +
    (float) priv->scroll_to_push_ratio *
    hover_state->distance *
    push_pull_strength *
    (priv->poll_input_rate_ms / 1000.f);

  if (new_dist < WINDOW_MIN_DIST || new_dist > WINDOW_MAX_DIST)
    return;

  hover_state->distance = new_dist;

  xrd_pointer_set_length (xrd_controller_get_pointer (controller),
                          hover_state->distance);
}

static void
_action_push_pull_scale_cb (OpenVRAction        *action,
                            OpenVRAnalogEvent   *event,
                            XrdClient           *self)
{
  (void) action;
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);
  if (controller == NULL)
    {
      g_free (event);
      return;
    }

  GrabState *grab_state = xrd_controller_get_grab_state (controller);

  double x_state = (double) graphene_vec3_get_x (&event->state);
  double y_state = (double) graphene_vec3_get_y (&event->state);

  /* go back to undecided when "stopping" current action,
   * to allow switching actions without letting go of the window. */
  if (fabs (x_state) < priv->analog_threshold &&
      fabs (y_state) < priv->analog_threshold)
    {
      grab_state->push_pull_scale_lock = LOCKED_NONE;
      g_free (event);
      return;
    }

  if (grab_state->push_pull_scale_lock == LOCKED_NONE)
    {
      if (fabs (x_state) > fabs (y_state) &&
          fabs (x_state) > priv->analog_threshold)
        grab_state->push_pull_scale_lock = LOCKED_SCALE;

      else if (fabs (y_state) > fabs (x_state) &&
          fabs (y_state) > priv->analog_threshold)
        grab_state->push_pull_scale_lock = LOCKED_PUSHPULL;
    }

  if (grab_state->push_pull_scale_lock == LOCKED_SCALE)
    {
      double factor = x_state * priv->scroll_to_scale_ratio;
      xrd_window_manager_scale (priv->manager, grab_state, (float) factor,
                                priv->poll_input_rate_ms);
    }
  else if (grab_state->push_pull_scale_lock == LOCKED_PUSHPULL)
    _perform_push_pull (self, controller, graphene_vec3_get_y (&event->state));

  g_free (event);
}

static void
_action_push_pull_cb (OpenVRAction        *action,
                      OpenVRAnalogEvent   *event,
                      XrdClient           *self)
{
  (void) action;
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);
  if (controller == NULL)
    {
      g_free (event);
      return;
    }

  GrabState *grab_state = xrd_controller_get_grab_state (controller);

  double y_state = (double) graphene_vec3_get_y (&event->state);
  if (grab_state->window && fabs (y_state) > priv->analog_threshold)
    _perform_push_pull (self, controller, graphene_vec3_get_y (&event->state));

  g_free (event);
}

static void
_action_grab_cb (OpenVRAction        *action,
                 OpenVRDigitalEvent  *event,
                 XrdClient           *self)
{
  (void) action;
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);
  if (controller == NULL)
    return;

  if (event->changed)
    {
      if (event->state == 1)
        xrd_window_manager_check_grab (priv->manager, controller);
      else
        xrd_window_manager_check_release (priv->manager, controller);
    }

  g_free (event);
}

static void
_action_menu_cb (OpenVRAction        *action,
                 OpenVRDigitalEvent  *event,
                 XrdClient           *self)
{
  (void) action;
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);
  if (controller == NULL)
    return;

  if (event->changed && event->state == 1 &&
      !xrd_controller_get_hover_state (controller)->window)
    {
      XrdClientPrivate *priv = xrd_client_get_instance_private (self);
      if (priv->wm_control_container)
        _destroy_buttons (self);
      else
        _init_buttons (self, controller);
    }
  g_free (event);
}

typedef struct OrientationTransition
{
  GrabState *grab_state;
  graphene_quaternion_t from;
  graphene_quaternion_t from_neg;
  graphene_quaternion_t to;
  float interpolate;
} OrientationTransition;

static gboolean
_interpolate_orientation_cb (gpointer _transition)
{
  OrientationTransition *transition = (OrientationTransition *) _transition;

  GrabState *grab_state = transition->grab_state;

  graphene_quaternion_slerp (&transition->from,
                             &transition->to,
                             transition->interpolate,
                             &grab_state->window_rotation);

  graphene_quaternion_slerp (&transition->from_neg,
                             &transition->to,
                             transition->interpolate,
                             &grab_state->window_transformed_rotation_neg);

  transition->interpolate += 0.07f;

  if (transition->interpolate > 1)
    {
      graphene_quaternion_init_identity (&grab_state->window_transformed_rotation_neg);
      graphene_quaternion_init_identity (&grab_state->window_rotation);
      g_free (transition);
      return FALSE;
    }

  return TRUE;
}

static void
_action_reset_orientation_cb (OpenVRAction       *action,
                              OpenVRDigitalEvent *event,
                              XrdClient          *self)
{
  (void) action;

  if (!(event->changed && event->state == 1))
    return;

  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);
  if (controller == NULL)
    return;

  GrabState *grab_state = xrd_controller_get_grab_state (controller);
  if (grab_state->window == NULL)
    return;

  OrientationTransition *transition = g_malloc (sizeof (OrientationTransition));

  /* TODO: Check if animation is already in progress */

  transition->interpolate = 0.;
  transition->grab_state = grab_state;

  graphene_quaternion_init_identity (&transition->to);
  graphene_quaternion_init_from_quaternion (&transition->from,
                                            &grab_state->window_rotation);
  graphene_quaternion_init_from_quaternion (&transition->from_neg,
                                            &grab_state->window_transformed_rotation_neg);

  g_timeout_add (10, _interpolate_orientation_cb, transition);

  g_free (event);
}

static void
_mark_windows_for_selection_mode (XrdClient *self);

static void
_window_grab_start_cb (XrdWindow               *window,
                       XrdControllerIndexEvent *event,
                       gpointer                 _self)
{
  XrdClient *self = XRD_CLIENT (_self);
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);

  /* don't grab if this window is already grabbed */
  if (priv->selection_mode)
    {
      gboolean pinned = xrd_window_manager_is_pinned (priv->manager, window);
      xrd_window_manager_set_pin (priv->manager, window, !pinned);
      _mark_windows_for_selection_mode (self);
      return;
    }

  if (xrd_client_is_grabbed (self, window))
    {
      g_free (event);
      return;
    }

  xrd_window_manager_drag_start (priv->manager, controller);

  if (event->controller_handle ==
      xrd_input_synth_synthing_controller (priv->input_synth))
    xrd_desktop_cursor_hide (priv->cursor);

  g_free (event);
}

static void
_window_grab_cb (XrdWindow    *window,
                 XrdGrabEvent *event,
                 gpointer     _self)
{
  (void) window;
  XrdClient *self = XRD_CLIENT (_self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);

  xrd_pointer_tip_set_transformation (
    xrd_controller_get_pointer_tip (controller), &event->pose);

  xrd_pointer_tip_update_apparent_size (
    xrd_controller_get_pointer_tip (controller));
  g_free (event);
}

static void
_mark_windows_for_selection_mode (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdWindowManager *manager = xrd_client_get_manager (self);
  if (priv->selection_mode)
    {
      GSList *all = xrd_window_manager_get_windows (manager);
      for (GSList *l = all; l != NULL; l = l->next)
        {
          XrdWindow *win = l->data;

          if (xrd_window_manager_is_pinned (manager, win))
            xrd_window_select (win);
          else
            xrd_window_deselect (win);

          xrd_window_show (win);
        }
    }
  else
    {
      GSList *all = xrd_window_manager_get_windows (manager);
      for (GSList *l = all; l != NULL; l = l->next)
        {
          XrdWindow *win = l->data;

          xrd_window_end_selection (win);

          if (priv->pinned_only &&
              !xrd_window_manager_is_pinned (manager, win))
            xrd_window_hide (win);
        }
    }
}

static void
_button_hover_cb (XrdWindow     *window,
                  XrdHoverEvent *event,
                  gpointer       _self)
{
  XrdClient *self = XRD_CLIENT (_self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);

  xrd_window_select (window);

  XrdPointer *pointer = xrd_controller_get_pointer (controller);
  XrdPointerTip *pointer_tip = xrd_controller_get_pointer_tip (controller);

  /* update pointer length and pointer tip */
  graphene_matrix_t window_pose;
  xrd_window_get_transformation (window, &window_pose);

  xrd_pointer_tip_update (pointer_tip, &window_pose, &event->point);
  xrd_pointer_set_length (pointer, event->distance);

  g_free (event);
}

static void
_window_hover_end_cb (XrdWindow               *window,
                      XrdControllerIndexEvent *event,
                      gpointer                 _self)
{
  (void) event;
  (void) window;
  XrdClient *self = XRD_CLIENT (_self);
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);

  xrd_pointer_reset_length (xrd_controller_get_pointer (controller));

  /* When leaving this window but now hovering another, the tip should
   * still be active because it is now hovering another window. */
  gboolean active = xrd_controller_get_hover_state (controller)->window != NULL;
  xrd_pointer_tip_set_active (xrd_controller_get_pointer_tip (controller),
                              active);

  XrdInputSynth *input_synth = xrd_client_get_input_synth (self);
  xrd_input_synth_reset_press_state (input_synth);

  if (event->controller_handle ==
      xrd_input_synth_synthing_controller (input_synth))
    xrd_desktop_cursor_hide (priv->cursor);

  g_free (event);
}

static void
_button_hover_end_cb (XrdWindow               *window,
                      XrdControllerIndexEvent *event,
                      gpointer                 _self)
{
  (void) event;
  XrdClient *self = XRD_CLIENT (_self);
  //XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  /* unmark if no controller is hovering over this button */
  if (!xrd_client_is_hovered (self, window))
    xrd_window_end_selection (window);

  _window_hover_end_cb (window, event, _self);

  /* _window_hover_end_cb will free the event */
  /* g_free (event); */
}

static void
_button_sphere_press_cb (XrdWindow               *window,
                         XrdControllerIndexEvent *event,
                         gpointer                 _self)
{
  (void) event;
  (void) window;
  XrdClient *self = XRD_CLIENT (_self);
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  xrd_window_manager_arrange_sphere (priv->manager);
  g_free (event);
}

static void
_button_reset_press_cb (XrdWindow               *window,
                        XrdControllerIndexEvent *event,
                        gpointer                 _self)
{
  (void) event;
  (void) window;
  XrdClient *self = XRD_CLIENT (_self);
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  xrd_window_manager_arrange_reset (priv->manager);
  g_free (event);
}

static void
_button_pinned_press_cb (XrdWindow               *button,
                         XrdControllerIndexEvent *event,
                         gpointer                 _self)
{
  (void) event;
  (void) button;
  XrdClient *self = _self;
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  if (priv->selection_mode)
    return;

  xrd_client_show_pinned_only (self, !priv->pinned_only);
  g_free (event);
}


static void
_button_select_pinned_press_cb (XrdOverlayWindow        *button,
                                XrdControllerIndexEvent *event,
                                gpointer                 _self)
{
  (void) event;
  (void) button;
  XrdClient *self = _self;
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  priv->selection_mode = !priv->selection_mode;
  _mark_windows_for_selection_mode (self);

  VkImageLayout layout = xrd_client_get_upload_layout (self);

  GulkanClient *client = xrd_client_get_uploader (self);
  if (priv->selection_mode)
    {
      xrd_button_set_icon (priv->select_pinned_button, client, layout,
                           "/icons/object-select-symbolic.svg");
    }
  else
    {
      xrd_button_set_icon (priv->select_pinned_button, client, layout,
                           "/icons/view-pin-symbolic.svg");
    }

  g_free (event);
}

gboolean
_init_buttons (XrdClient *self, XrdController *controller)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  /* Use head attached menu if less than 2 controllers are active.
   * Controller attached requires more than 1 controller to use. */
  int attach_controller = g_hash_table_size (priv->controllers) > 1;

  float w;
  float h;
  if (attach_controller)
    {
      w = 0.07f;
      h = 0.07f;
    }
  else
    {
      w = 0.25f;
      h = 0.25f;
    }

  priv->wm_control_container = xrd_container_new ();
  xrd_container_set_attachment (priv->wm_control_container,
                                attach_controller ?
                                    XRD_CONTAINER_ATTACHMENT_HAND :
                                    XRD_CONTAINER_ATTACHMENT_HEAD,
                                controller);
  xrd_container_set_layout (priv->wm_control_container,
                            XRD_CONTAINER_RELATIVE);

  graphene_point3d_t position = { .x =  0, .y = 0, .z = 0 };

  priv->button_sphere =
    xrd_client_button_new_from_icon (self, w, h,
                                     "/icons/align-sphere-symbolic.svg");
  if (!priv->button_sphere)
    return FALSE;

  xrd_client_add_button (self, priv->button_sphere, &position,
                         (GCallback) _button_sphere_press_cb, self);

  float width = xrd_window_get_current_width_meters (priv->button_sphere);
  float height = xrd_window_get_current_height_meters (priv->button_sphere);
  graphene_point3d_t translation = {
      .x = -width / 2.f,
      .y = height / 2.f,
      .z = 0
  };
  graphene_matrix_t relative_transform;
  graphene_matrix_init_translate (&relative_transform, &translation);
  xrd_container_add_window (priv->wm_control_container,
                            priv->button_sphere,
                            &relative_transform);

  priv->button_reset =
    xrd_client_button_new_from_icon (self, w, h,
                                     "/icons/edit-undo-symbolic.svg");
  if (!priv->button_reset)
    return FALSE;

  xrd_client_add_button (self, priv->button_reset, &position,
                         (GCallback) _button_reset_press_cb, self);

  translation.x += width;

  graphene_matrix_init_translate (&relative_transform, &translation);
  xrd_container_add_window (priv->wm_control_container,
                            priv->button_reset,
                            &relative_transform);

  priv->select_pinned_button =
    xrd_client_button_new_from_icon (self, w, h,
                                     "/icons/view-pin-symbolic.svg");
  if (!priv->select_pinned_button)
    return FALSE;

  xrd_client_add_button (self, priv->select_pinned_button, &position,
                         (GCallback) _button_select_pinned_press_cb, self);

  translation.x = - width / 2.f;
  translation.y -= height;
  graphene_matrix_init_translate (&relative_transform, &translation);
  xrd_container_add_window (priv->wm_control_container,
                            priv->select_pinned_button,
                            &relative_transform);

  priv->pinned_button =
    xrd_client_button_new_from_icon (self, w, h,
                                     "/icons/object-visible-symbolic.svg");
  if (!priv->pinned_button)
    return FALSE;

  xrd_client_add_button (self, priv->pinned_button, &position,
                         (GCallback) _button_pinned_press_cb, self);

  translation.x += width;
  graphene_matrix_init_translate (&relative_transform, &translation);
  xrd_container_add_window (priv->wm_control_container,
                            priv->pinned_button,
                            &relative_transform);

  xrd_client_add_container (self, priv->wm_control_container);

  if (!attach_controller)
    {
      const float distance = 2.0f;
      xrd_container_center_view (priv->wm_control_container,
                                 distance);
      xrd_container_set_distance (priv->wm_control_container, distance);
    }

  return TRUE;
}

static void
_destroy_buttons (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  xrd_client_remove_window (self, priv->button_sphere);
  g_clear_object (&priv->button_sphere);
  xrd_client_remove_window (self, priv->button_reset);
  g_clear_object (&priv->button_reset);
  xrd_client_remove_window (self, priv->pinned_button);
  g_clear_object (&priv->pinned_button);
  xrd_client_remove_window (self, priv->select_pinned_button);
  g_clear_object (&priv->select_pinned_button);

  xrd_window_manager_remove_container (priv->manager,
                                       priv->wm_control_container);

  g_clear_object (&priv->wm_control_container);
}

static void
_keyboard_press_cb (OpenVRContext *context,
                    GdkEventKey   *event,
                    XrdClient     *self)
{
  (void) context;
  xrd_client_emit_keyboard_press (self, event);

  /* TODO: this crashes
  gdk_event_free ((GdkEvent*) event); */
}

static void
_keyboard_close_cb (OpenVRContext *context,
                    XrdClient     *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  priv->keyboard_window = NULL;

  g_signal_handler_disconnect(context, priv->keyboard_press_signal);
  g_signal_handler_disconnect(context, priv->keyboard_close_signal);
  priv->keyboard_press_signal = 0;
  priv->keyboard_close_signal = 0;

  g_print ("Keyboard closed\n");
}

static void
_action_show_keyboard_cb (OpenVRAction       *action,
                          OpenVRDigitalEvent *event,
                          XrdClient          *self)
{
  (void) action;
  if (!event->state && event->changed)
    {
      XrdClientPrivate *priv = xrd_client_get_instance_private (self);

      OpenVRContext *context = openvr_context_get_instance ();
      openvr_context_show_system_keyboard (context);

      guint64 controller_handle =
        xrd_input_synth_synthing_controller (priv->input_synth);
      XrdController *controller = _lookup_controller (self, controller_handle);

      /* window hovered by the synthing controller receives input */
      priv->keyboard_window =
        xrd_controller_get_hover_state (controller)->window;

      priv->keyboard_press_signal =
          g_signal_connect (context, "keyboard-press-event",
                            (GCallback) _keyboard_press_cb, self);
      priv->keyboard_close_signal =
          g_signal_connect (context, "keyboard-close-event",
                            (GCallback) _keyboard_close_cb, self);
    }

  g_free (event);
}

static void
_window_hover_cb (XrdWindow     *window,
                  XrdHoverEvent *event,
                  XrdClient     *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);
  graphene_matrix_t window_pose;
  xrd_window_get_transformation_no_scale (window, &window_pose);
  xrd_pointer_tip_update (xrd_controller_get_pointer_tip (controller),
                          &window_pose, &event->point);

  xrd_pointer_set_length (xrd_controller_get_pointer (controller),
                          event->distance);

  xrd_controller_get_hover_state (controller)->window = window;

  if (event->controller_handle ==
      xrd_input_synth_synthing_controller (priv->input_synth))
    {
      xrd_input_synth_move_cursor (priv->input_synth, window,
                                   &event->pose, &event->point);

      xrd_desktop_cursor_update (priv->cursor, window, &event->point);

      if (xrd_controller_get_hover_state (controller)->window != window)
        xrd_input_synth_reset_scroll (priv->input_synth);
    }

  g_free (event);
}

static void
_window_hover_start_cb (XrdWindow               *window,
                        XrdControllerIndexEvent *event,
                        XrdClient               *self)
{
  (void) window;
  (void) event;

  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);
  xrd_pointer_tip_set_active (xrd_controller_get_pointer_tip (controller),
                              TRUE);

  g_free (event);
}

static void
_manager_no_hover_cb (XrdWindowManager *manager,
                      XrdNoHoverEvent  *event,
                      XrdClient        *self)
{
  (void) manager;

  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);

  XrdPointerTip *pointer_tip = xrd_controller_get_pointer_tip (controller);

  XrdPointer *pointer_ray = xrd_controller_get_pointer (controller);

  graphene_point3d_t distance_translation_point;
  graphene_point3d_init (&distance_translation_point,
                         0.f,
                         0.f,
                         -xrd_pointer_get_default_length (pointer_ray));

  graphene_matrix_t tip_pose;

  graphene_quaternion_t controller_rotation;
  graphene_quaternion_init_from_matrix (&controller_rotation, &event->pose);

  graphene_point3d_t controller_translation_point;
  graphene_matrix_get_translation_point3d (&event->pose,
                                           &controller_translation_point);

  graphene_matrix_init_identity (&tip_pose);
  graphene_matrix_translate (&tip_pose, &distance_translation_point);
  graphene_matrix_rotate_quaternion (&tip_pose, &controller_rotation);
  graphene_matrix_translate (&tip_pose, &controller_translation_point);

  xrd_pointer_tip_set_transformation (pointer_tip, &tip_pose);

  xrd_pointer_tip_update_apparent_size (pointer_tip);

  xrd_pointer_tip_set_active (pointer_tip, FALSE);

  if (xrd_input_synth_synthing_controller (priv->input_synth) ==
      event->controller_handle)
    xrd_input_synth_reset_scroll (priv->input_synth);

  xrd_controller_reset_hover_state (controller);

  g_free (event);
}

static void
_update_input_poll_rate (GSettings *settings, gchar *key, gpointer _self)
{
  XrdClient *self = _self;
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  if (priv->poll_input_source_id != 0)
    g_source_remove (priv->poll_input_source_id);
  priv->poll_input_rate_ms = g_settings_get_uint (settings, key);

  priv->poll_input_source_id =
      g_timeout_add (priv->poll_input_rate_ms,
      (GSourceFunc) xrd_client_poll_input_events,
      self);
}

static void
_synth_click_cb (XrdInputSynth *synth,
                 XrdClickEvent *event,
                 XrdClient     *self)
{
  (void) synth;
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  XrdController *controller = _lookup_controller (self,
                                                  event->controller_handle);
  if (priv->selection_mode)
    return;

  if (xrd_controller_get_hover_state (controller)->window)
    {
      event->window = xrd_controller_get_hover_state (controller)->window;
      xrd_client_emit_click (self, event);

      if (event->button == 1)
        {
          if (xrd_controller_get_hover_state (controller)->window != NULL &&
              event->state)
            {
              xrd_pointer_tip_animate_pulse (
                xrd_controller_get_pointer_tip (controller));
            }
        }
    }

  g_free (event);
}

static void
_synth_move_cursor_cb (XrdInputSynth      *synth,
                       XrdMoveCursorEvent *event,
                       XrdClient          *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  if (priv->selection_mode)
    return;

  (void) synth;
  if (!event->ignore)
    xrd_client_emit_move_cursor (self, event);

  g_free (event);
}

XrdWindow *
xrd_client_window_new_from_meters (XrdClient *client,
                                   const char* title,
                                   float w,
                                   float h)
{
  XrdWindow *window;
  if (XRD_IS_SCENE_CLIENT (client))
    {
      window = XRD_WINDOW (xrd_scene_window_new_from_meters (title, w, h));
      xrd_scene_window_initialize (XRD_SCENE_WINDOW (window));
    }
  else
    {
      window = XRD_WINDOW (xrd_overlay_window_new_from_meters (title, w, h));
    }
  return window;
}

XrdWindow *
xrd_client_window_new_from_ppm (XrdClient *client,
                                const char* title,
                                uint32_t w,
                                uint32_t h,
                                float ppm)
{
  XrdWindow *window;
  if (XRD_IS_SCENE_CLIENT (client))
    {
      window = XRD_WINDOW (xrd_scene_window_new_from_ppm (title, w, h, ppm));
      xrd_scene_window_initialize (XRD_SCENE_WINDOW (window));
    }
  else
    {
      window = XRD_WINDOW (xrd_overlay_window_new_from_ppm (title, w, h, ppm));
    }
  return window;
}

void
xrd_client_add_button_callbacks (XrdClient *self,
                                 XrdWindow *button)
{
  g_signal_connect (button, "hover-event",
                    (GCallback) _button_hover_cb, self);

  g_signal_connect (button, "hover-start-event",
                    (GCallback) _window_hover_start_cb, self);

  g_signal_connect (button, "hover-end-event",
                    (GCallback) _button_hover_end_cb, self);
}

void
xrd_client_add_window_callbacks (XrdClient *self,
                                 XrdWindow *window)
{
  g_signal_connect (window, "grab-start-event",
                    (GCallback) _window_grab_start_cb, self);
  g_signal_connect (window, "grab-event",
                    (GCallback) _window_grab_cb, self);
  // g_signal_connect (window, "release-event",
  //                   (GCallback) _window_release_cb, self);
  g_signal_connect (window, "hover-start-event",
                    (GCallback) _window_hover_start_cb, self);
  g_signal_connect (window, "hover-event",
                    (GCallback) _window_hover_cb, self);
  g_signal_connect (window, "hover-end-event",
                    (GCallback) _window_hover_end_cb, self);
}

void
xrd_client_set_desktop_cursor (XrdClient        *self,
                               XrdDesktopCursor *cursor)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  priv->cursor = cursor;
  xrd_desktop_cursor_hide (priv->cursor);
}

XrdDesktopCursor*
xrd_client_get_desktop_cursor (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  return priv->cursor;
}

GHashTable *
xrd_client_get_controllers (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  return priv->controllers;
}

static void
_device_activate_cb (OpenVRContext          *context,
                     OpenVRDeviceIndexEvent *event,
                     gpointer               _self)
{
  (void) context;
  XrdClient *self = _self;
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  guint64 handle = event->controller_handle;

  if (g_hash_table_contains (priv->controllers, &handle))
    {
      g_print ("Controller %lu already active\n", handle);
      return;
    }

  g_print ("Controller %lu activated.\n", handle);
  XrdController *controller = xrd_controller_new (handle);

  guint64 *key = g_malloc (sizeof (guint64));
  *key = handle;
  g_hash_table_insert (priv->controllers, key, controller);

  xrd_client_init_controller (self, controller);

  if (g_hash_table_size (priv->controllers) == 1)
    xrd_input_synth_hand_off_to_controller (priv->input_synth, handle);
}

static void
_device_deactivate_cb (OpenVRContext          *context,
                       OpenVRDeviceIndexEvent *event,
                       gpointer               _self)
{
  (void) context;
  XrdClient *self = _self;
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  guint64 handle = event->controller_handle;
  g_print ("Controller %lu deactivated..\n", handle);

  /* hashmap destroys key & val
  g_object_unref (controller); */

  g_hash_table_remove (priv->controllers, &handle);

  if (xrd_input_synth_synthing_controller (priv->input_synth) == handle &&
      g_hash_table_size (priv->controllers) > 0)
    {
      GList *controllers = g_hash_table_get_values (priv->controllers);
      XrdController *controller = XRD_CONTROLLER (controllers->data);
      xrd_input_synth_hand_off_to_controller (
        priv->input_synth, xrd_controller_get_handle (controller));
      g_list_free (controllers);
    }

}

static void
xrd_client_init (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  priv->controllers = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                             g_free, g_object_unref);

  xrd_settings_connect_and_apply (G_CALLBACK (xrd_settings_update_double_val),
                                  "scroll-to-push-ratio",
                                  &priv->scroll_to_push_ratio);
  xrd_settings_connect_and_apply (G_CALLBACK (xrd_settings_update_double_val),
                                  "scroll-to-scale-ratio",
                                  &priv->scroll_to_scale_ratio);
  xrd_settings_connect_and_apply (G_CALLBACK (xrd_settings_update_double_val),
                                  "analog-threshold", &priv->analog_threshold);

  priv->poll_runtime_event_source_id = 0;
  priv->poll_input_source_id = 0;
  priv->keyboard_window = NULL;
  priv->keyboard_press_signal = 0;
  priv->keyboard_close_signal = 0;
  priv->pinned_only = FALSE;
  priv->selection_mode = FALSE;
  priv->wm_actions = NULL;
  priv->cursor = NULL;
  priv->wm_control_container = NULL;

  priv->context = openvr_context_get_instance ();
  priv->manager = xrd_window_manager_new ();

  OpenVRContext *context = openvr_context_get_instance ();

  g_signal_connect (context, "device-activate-event",
                    (GCallback) _device_activate_cb, self);
  g_signal_connect (context, "device-deactivate-event",
                    (GCallback) _device_deactivate_cb, self);
}

static void
_system_quit_cb (OpenVRContext *context,
                 OpenVRQuitEvent *event,
                 XrdClient     *self)
{
  (void) event;
  (void) self;
  /* g_print("Handling VR quit event %d\n", event->reason); */
  openvr_context_acknowledge_quit (context);

  xrd_client_emit_system_quit (self, event);
  g_free (event);
}

void
xrd_client_post_openvr_init (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  if (!openvr_io_load_cached_action_manifest (
        "xrdesktop",
        "/res/bindings",
        "actions.json",
        "bindings_vive_controller.json",
        "bindings_knuckles_controller.json",
        NULL))
    {
      g_print ("Failed to load action bindings!\n");
      return;
    }

  priv->wm_actions = openvr_action_set_new_from_url ("/actions/wm");

  priv->input_synth = xrd_input_synth_new ();

  g_signal_connect (priv->context, "quit-event",
                    (GCallback) _system_quit_cb, self);

  priv->button_sphere = NULL;
  priv->button_reset = NULL;
  priv->pinned_button = NULL;
  priv->select_pinned_button = NULL;
  priv->wm_control_container = NULL;

  openvr_action_set_connect (priv->wm_actions, OPENVR_ACTION_POSE,
                             "/actions/wm/in/hand_pose",
                             (GCallback) _action_hand_pose_cb, self);
  openvr_action_set_connect (priv->wm_actions, OPENVR_ACTION_POSE,
                             "/actions/wm/in/hand_pose_hand_grip",
                             (GCallback) _action_hand_pose_hand_grip_cb, self);
  openvr_action_set_connect (priv->wm_actions, OPENVR_ACTION_DIGITAL,
                             "/actions/wm/in/grab_window",
                             (GCallback) _action_grab_cb, self);
  openvr_action_set_connect (priv->wm_actions, OPENVR_ACTION_DIGITAL,
                             "/actions/wm/in/reset_orientation",
                             (GCallback) _action_reset_orientation_cb, self);
  openvr_action_set_connect (priv->wm_actions, OPENVR_ACTION_DIGITAL,
                             "/actions/wm/in/menu",
                             (GCallback) _action_menu_cb, self);
  openvr_action_set_connect (priv->wm_actions, OPENVR_ACTION_ANALOG,
                             "/actions/wm/in/push_pull_scale",
                             (GCallback) _action_push_pull_scale_cb, self);
  openvr_action_set_connect (priv->wm_actions, OPENVR_ACTION_ANALOG,
                             "/actions/wm/in/push_pull",
                             (GCallback) _action_push_pull_cb, self);
  openvr_action_set_connect (priv->wm_actions, OPENVR_ACTION_DIGITAL,
                             "/actions/wm/in/show_keyboard",
                             (GCallback) _action_show_keyboard_cb, self);

  g_signal_connect (priv->manager, "no-hover-event",
                    (GCallback) _manager_no_hover_cb, self);

  xrd_settings_connect_and_apply (G_CALLBACK (_update_input_poll_rate),
                                  "input-poll-rate-ms", self);

  priv->poll_runtime_event_source_id =
      g_timeout_add (20,
                     (GSourceFunc) xrd_client_poll_runtime_events,
                     self);


  g_signal_connect (priv->input_synth, "click-event",
                    (GCallback) _synth_click_cb, self);
  g_signal_connect (priv->input_synth, "move-cursor-event",
                    (GCallback) _synth_move_cursor_cb, self);
}


gboolean
xrd_client_is_hovering (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  GList *controllers = g_hash_table_get_values (priv->controllers);
  for (GList *l = controllers; l; l = l->next)
    {
      XrdController *controller = XRD_CONTROLLER (l->data);
      if (xrd_controller_get_hover_state (controller)->window != NULL)
        {
          g_list_free (controllers);
          return TRUE;
        }
    }
  g_list_free (controllers);
  return FALSE;
}

gboolean
xrd_client_is_grabbing (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  GList *controllers = g_hash_table_get_values (priv->controllers);
  for (GList *l = controllers; l; l = l->next)
    {
      XrdController *controller = XRD_CONTROLLER (l->data);
      if (xrd_controller_get_grab_state (controller)->window != NULL)
        {
          g_list_free (controllers);
          return TRUE;
        }
    }
  g_list_free (controllers);
  return FALSE;
}

gboolean
xrd_client_is_grabbed (XrdClient *self,
                       XrdWindow *window)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  GList *controllers = g_hash_table_get_values (priv->controllers);
  for (GList *l = controllers; l; l = l->next)
    {
      XrdController *controller = XRD_CONTROLLER (l->data);
      if (xrd_controller_get_grab_state (controller)->window == window)
        {
          g_list_free (controllers);
          return TRUE;
        }
    }
  g_list_free (controllers);
  return FALSE;
}

gboolean
xrd_client_is_hovered (XrdClient *self,
                       XrdWindow *window)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);
  GList *controllers = g_hash_table_get_values (priv->controllers);
  for (GList *l = controllers; l; l = l->next)
    {
      XrdController *controller = XRD_CONTROLLER (l->data);
      if (xrd_controller_get_hover_state (controller)->window == window)
        {
          g_list_free (controllers);
          return TRUE;
        }
    }
  g_list_free (controllers);
  return FALSE;
}

static XrdClient *
_replace_client (XrdClient *self)
{
  XrdClient *ret = NULL;
  gboolean to_scene = XRD_IS_OVERLAY_CLIENT (self);
  if (to_scene)
    {
      g_object_unref (self);
      ret = XRD_CLIENT (xrd_scene_client_new ());
      if (XRD_IS_SCENE_CLIENT (ret))
        xrd_scene_client_initialize (XRD_SCENE_CLIENT (ret));
    }
  else
    {
      g_object_unref (self);
      ret = XRD_CLIENT (xrd_overlay_client_new ());
    }

  return ret;
}

/** xrd_client_switch_mode:
 * @self: current #XrdClient to be destroyed.
 * Returns: A new #XrdClient of the opposite mode than the passed one.
 *
 * References to gulkan, openvr-glib and xrdesktop objects (like #XrdWindow)
 * will be invalid after calling this function.
 *
 * xrd_client_switch_mode() replaces each #XrdWindow with an appropriate new
 * one, preserving its transformation matrix, scaling, pinned status, etc.
 * For details about preserved state, see #_XrdWindowState.
 *
 * The caller is responsible for reconnecting callbacks to #XrdClient signals.
 * The caller is responsible to not use references to any previous #XrdWindow.
 * The caller may use xrd_client_get_windows() to get the list of new windows
 * and may use the "native" property to recognize each window.
 */
struct _XrdClient *
xrd_client_switch_mode (XrdClient *self)
{
  XrdClientPrivate *priv = xrd_client_get_instance_private (self);

  gboolean show_only_pinned = priv->pinned_only;

  XrdWindowManager *manager = xrd_client_get_manager (self);
  int window_count =
    (int)g_slist_length (xrd_window_manager_get_windows (manager));

  XrdWindowState *state = g_malloc (sizeof (XrdWindowState) *
                                    (guint)window_count);

  xrd_window_manager_save_state (manager, state, window_count);

  XrdClient *ret = _replace_client (self);
  manager = xrd_client_get_manager (ret);

  for (int i = 0; i < window_count; i++)
    {
      XrdWindow *window =
        xrd_client_window_new_from_meters (ret,
                                           state[i].title,
                                           state[i].current_width,
                                           state[i].current_height);

      g_object_set (window,
                    "native", state[i].native,
                    "scale", (double)state[i].scale,
                    "initial-width-meters", (double)state[i].initial_width,
                    "initial-height-meters", (double)state[i].initial_height,
                    "texture-width", state[i].texture_width,
                    "texture-height", state[i].texture_height,
                    NULL);

      /* xrd_client_add_window saves the reset transform */
      xrd_window_set_transformation (window, &state[i].reset_transform);
      xrd_client_add_window (ret, window, state[i].is_draggable);

      xrd_window_set_transformation (window, &state[i].transform);
      xrd_window_manager_set_pin (manager, window, state[i].pinned);
    }

  /* Only after all windows are recreated do we search for child windows */
  GSList *windows = xrd_window_manager_get_windows (manager);
  for (int i = 0; i < window_count; i++)
    {
      XrdWindow *window = g_slist_nth_data (windows, (guint)i);
      if (state[i].child_index >= 0)
        {
          int child_index = state[i].child_index;

          XrdWindow *child_window =
            g_slist_nth_data (windows, (guint)child_index);

          xrd_window_add_child (window, child_window,
                                &state[i].child_offset_center);
          xrd_window_set_transformation (child_window,
                                         &state[child_index].transform);
        }
    }

  xrd_client_show_pinned_only (ret, show_only_pinned);

  g_free (state);
  return ret;
}


