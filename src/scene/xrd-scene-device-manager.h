/*
 * xrdesktop
 * Copyright 2018 Collabora Ltd.
 * Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef XRD_SCENE_DEVICE_MANAGER_H_
#define XRD_SCENE_DEVICE_MANAGER_H_

#if !defined (XRD_INSIDE) && !defined (XRD_COMPILATION)
#error "Only <xrd.h> can be included directly."
#endif

#include <glib-object.h>

#include "xrd-scene-device.h"
#include "xrd-scene-pointer.h"
#include <gulkan.h>

G_BEGIN_DECLS

#define XRD_TYPE_SCENE_DEVICE_MANAGER xrd_scene_device_manager_get_type()
G_DECLARE_FINAL_TYPE (XrdSceneDeviceManager, xrd_scene_device_manager,
                      XRD, SCENE_DEVICE_MANAGER, GObject)

XrdSceneDeviceManager *xrd_scene_device_manager_new (void);

void
xrd_scene_device_manager_add (XrdSceneDeviceManager *self,
                              GulkanClient          *client,
                              TrackedDeviceIndex_t   device_id,
                              VkDescriptorSetLayout *layout);

void
xrd_scene_device_manager_remove (XrdSceneDeviceManager *self,
                                 TrackedDeviceIndex_t   device_id);

void
xrd_scene_device_manager_render (XrdSceneDeviceManager *self,
                                 EVREye                 eye,
                                 VkCommandBuffer        cmd_buffer,
                                 VkPipeline             pipeline,
                                 VkPipelineLayout       layout,
                                 graphene_matrix_t     *vp);

void
xrd_scene_device_manager_update_poses (XrdSceneDeviceManager *self,
                                       graphene_matrix_t     *mat_head_pose);

G_END_DECLS

#endif /* XRD_SCENE_DEVICE_MANAGER_H_ */
