/*
 * xrdesktop
 * Copyright 2018 Collabora Ltd.
 * Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef XRD_OVERLAY_MODEL_H_
#define XRD_OVERLAY_MODEL_H_

#if !defined (XRD_INSIDE) && !defined (XRD_COMPILATION)
#error "Only <xrd.h> can be included directly."
#endif

#include <glib-object.h>

#include <gxr.h>

G_BEGIN_DECLS

#define XRD_TYPE_OVERLAY_MODEL xrd_overlay_model_get_type()
G_DECLARE_DERIVABLE_TYPE (XrdOverlayModel, xrd_overlay_model, XRD,
                          OVERLAY_MODEL, OpenVROverlay)

/**
 * XrdOverlayModelClass:
 * @parent: The object class structure needs to be the first
 *   element in the widget class structure in order for the class mechanism
 *   to work correctly. This allows a XrdOverlayModelClass pointer to be cast to
 *   a OpenVROverlayClass pointer.
 */
struct _XrdOverlayModelClass
{
  OpenVROverlayClass parent;
};

XrdOverlayModel *xrd_overlay_model_new (gchar* key, gchar* name);

gboolean
xrd_overlay_model_set_model (XrdOverlayModel *self, gchar *name,
                             graphene_vec4_t *color);

gboolean
xrd_overlay_model_get_model (XrdOverlayModel *self, gchar *name,
                             graphene_vec4_t *color, uint32_t *id);

gboolean
xrd_overlay_model_initialize (XrdOverlayModel *self, gchar* key, gchar* name);

G_END_DECLS

#endif /* XRD_OVERLAY_MODEL_H_ */
