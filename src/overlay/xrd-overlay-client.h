/*
 * xrdesktop
 * Copyright 2018 Collabora Ltd.
 * Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef XRD_OVERLAY_CLIENT_H_
#define XRD_OVERLAY_CLIENT_H_

#if !defined (XRD_INSIDE) && !defined (XRD_COMPILATION)
#error "Only <xrd.h> can be included directly."
#endif

#include <glib-object.h>
#include <gmodule.h>

#include <gxr.h>

#include "xrd-client.h"

#include "xrd-overlay-pointer.h"
#include "xrd-overlay-pointer-tip.h"
#include "xrd-window-manager.h"
#include "xrd-input-synth.h"
#include "xrd-desktop-cursor.h"

G_BEGIN_DECLS

#define XRD_TYPE_OVERLAY_CLIENT xrd_overlay_client_get_type()
G_DECLARE_FINAL_TYPE (XrdOverlayClient, xrd_overlay_client,
                      XRD, OVERLAY_CLIENT, XrdClient)

XrdOverlayClient *xrd_overlay_client_new (void);

G_END_DECLS

#endif /* XRD_OVERLAY_CLIENT_H_ */
