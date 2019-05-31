/*
 * xrdesktop
 * Copyright 2018 Collabora Ltd.
 * Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef XRD_OVERLAY_POINTER_TIP_H_
#define XRD_OVERLAY_POINTER_TIP_H_

#if !defined (XRD_INSIDE) && !defined (XRD_COMPILATION)
#error "Only <xrd.h> can be included directly."
#endif

#include <glib-object.h>

#include <gulkan.h>
#include <openvr-glib.h>

G_BEGIN_DECLS

#define XRD_TYPE_OVERLAY_POINTER_TIP xrd_overlay_pointer_tip_get_type()
G_DECLARE_FINAL_TYPE (XrdOverlayPointerTip, xrd_overlay_pointer_tip, XRD,
                      OVERLAY_POINTER_TIP, OpenVROverlay)


struct _XrdOverlayPointerTip;

XrdOverlayPointerTip *
xrd_overlay_pointer_tip_new (guint64 controller_index, GulkanClient *gc);

G_END_DECLS

#endif /* XRD_OVERLAY_POINTER_TIP_H_ */
