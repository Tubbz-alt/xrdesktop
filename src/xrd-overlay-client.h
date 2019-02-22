/*
 * Xrd GLib
 * Copyright 2018 Collabora Ltd.
 * Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef XRD_GLIB_OVERLAY_CLIENT_H_
#define XRD_GLIB_OVERLAY_CLIENT_H_

#include <glib-object.h>
#include <gmodule.h>

#include <openvr-action-set.h>

#include "xrd-overlay-pointer.h"
#include "xrd-overlay-pointer-tip.h"
#include "xrd-overlay-window-manager.h"
#include "xrd-overlay-button.h"
#include "xrd-input-synth.h"

G_BEGIN_DECLS

#define XRD_TYPE_OVERLAY_CLIENT xrd_overlay_client_get_type()
G_DECLARE_FINAL_TYPE (XrdOverlayClient, xrd_overlay_client,
                      XRD, OVERLAY_CLIENT, GObject)

typedef struct XrdClientController
{
  XrdOverlayClient *self;
  int               index;
} XrdClientController;

struct _XrdOverlayClient
{
  GObject parent;

  OpenVRContext *context;

  XrdOverlayPointer *pointer_ray[OPENVR_CONTROLLER_COUNT];
  XrdOverlayPointerTip *pointer_tip[OPENVR_CONTROLLER_COUNT];

  XrdOverlayWindowManager *manager;

  XrdClientController left;
  XrdClientController right;

  XrdOverlayButton *button_reset;
  XrdOverlayButton *button_sphere;

  OpenVROverlayUploader *uploader;

  OpenVRActionSet *wm_actions;

  XrdOverlayWindow *hover_window[OPENVR_CONTROLLER_COUNT];
  XrdOverlayWindow *keyboard_window;
  guint keyboard_press_signal;
  guint keyboard_close_signal;

  int poll_rate_ms;
  guint poll_event_source_id;

  double analog_threshold;
  
  double scroll_to_push_ratio;
  double scroll_to_scale_ratio;
  
  double pixel_per_meter;

  XrdInputSynth *input_synth;

  XrdOverlayDesktopCursor *cursor;
};

XrdOverlayClient *xrd_overlay_client_new (void);

XrdOverlayWindow *
xrd_overlay_client_add_window (XrdOverlayClient *self,
                               const char       *title,
                               gpointer          native,
                               uint32_t          width,
                               uint32_t          height,
                               gboolean          is_child);

void
xrd_overlay_client_remove_window (XrdOverlayClient *self,
                                  XrdOverlayWindow *window);

G_END_DECLS

#endif /* XRD_GLIB_OVERLAY_CLIENT_H_ */
