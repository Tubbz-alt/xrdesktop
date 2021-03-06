/*
 * xrdesktop
 * Copyright 2018 Collabora Ltd.
 * Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#include "xrd-scene-model.h"

#include <gxr.h>

#include "xrd-scene-renderer.h"

struct _XrdSceneModel
{
  GObject parent;

  GulkanDevice *device;

  GulkanTexture *texture;
  GulkanVertexBuffer *vbo;
  VkSampler sampler;
};

G_DEFINE_TYPE (XrdSceneModel, xrd_scene_model, G_TYPE_OBJECT)

static void
xrd_scene_model_finalize (GObject *gobject);

static void
xrd_scene_model_class_init (XrdSceneModelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xrd_scene_model_finalize;
}

static void
xrd_scene_model_init (XrdSceneModel *self)
{
  self->sampler = VK_NULL_HANDLE;
  self->vbo = gulkan_vertex_buffer_new ();
}

XrdSceneModel *
xrd_scene_model_new (void)
{
  return (XrdSceneModel*) g_object_new (XRD_TYPE_SCENE_MODEL, 0);
}

static void
xrd_scene_model_finalize (GObject *gobject)
{
  XrdSceneModel *self = XRD_SCENE_MODEL (gobject);
  g_object_unref (self->vbo);
  g_object_unref (self->texture);

  GulkanDevice *device = xrd_scene_renderer_get_device ();

  if (self->sampler != VK_NULL_HANDLE)
    vkDestroySampler (gulkan_device_get_handle (device),
                      self->sampler, NULL);
}

static bool
_load_openvr_mesh (RenderModel_t **model,
                   const char     *name)
{
  EVRRenderModelError error;
  OpenVRContext *context = openvr_context_get_instance ();

  do
    {
      error = context->model->LoadRenderModel_Async ((char *) g_strdup (name),
                                                     model);
      /* Treat async loading synchronously.. */
      usleep (1000);
    }
  while (error == EVRRenderModelError_VRRenderModelError_Loading);

  if (error != EVRRenderModelError_VRRenderModelError_None)
    {
      g_printerr ("Unable to load model %s - %s\n", name,
                  context->model->GetRenderModelErrorNameFromEnum (error));
      return FALSE;
    }

  return TRUE;
}

static bool
_load_openvr_texture (TextureID_t                id,
                      RenderModel_TextureMap_t **texture)
{
  EVRRenderModelError error;
  OpenVRContext *context = openvr_context_get_instance ();

  do
    {
      error = context->model->LoadTexture_Async (id, texture);
      /* Treat async loading synchronously.. */
      usleep (1000);
    }
  while (error == EVRRenderModelError_VRRenderModelError_Loading);

  if (error != EVRRenderModelError_VRRenderModelError_None)
    {
      g_printerr ("Unable to load OpenVR texture id: %d\n", id);
      return FALSE;
    }

  return TRUE;
}

static gboolean
_load_mesh (XrdSceneModel *self,
            GulkanDevice  *device,
            RenderModel_t *vr_model)
{
  if (!gulkan_vertex_buffer_alloc_data (
      self->vbo, device, vr_model->rVertexData,
      sizeof (RenderModel_Vertex_t) * vr_model->unVertexCount))
    return FALSE;

  if (!gulkan_vertex_buffer_alloc_index_data (
      self->vbo, device, vr_model->rIndexData,
      sizeof (uint16_t), vr_model->unTriangleCount * 3))
    return FALSE;

  return TRUE;
}

static gboolean
_load_texture (XrdSceneModel            *self,
               GulkanClient             *gc,
               RenderModel_TextureMap_t *texture)
{
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data (
      texture->rubTextureMapData, GDK_COLORSPACE_RGB, TRUE, 8,
      texture->unWidth, texture->unHeight,
      4 * texture->unWidth, NULL, NULL);

  self->texture =
    gulkan_client_texture_new_from_pixbuf (gc, pixbuf,
                                           VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                           true);

  guint mip_levels = gulkan_texture_get_mip_levels (self->texture);

  VkSamplerCreateInfo sampler_info = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .magFilter = VK_FILTER_LINEAR,
    .minFilter = VK_FILTER_LINEAR,
    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .anisotropyEnable = VK_TRUE,
    .maxAnisotropy = 16.0f,
    .minLod = 0.0f,
    .maxLod = (float) mip_levels,
  };

  GulkanDevice *device = gulkan_client_get_device (gc);
  vkCreateSampler (gulkan_device_get_handle (device), &sampler_info, NULL,
                   &self->sampler);

  return TRUE;
}

gboolean
xrd_scene_model_load (XrdSceneModel *self,
                      GulkanClient  *gc,
                      const char    *model_name)
{
  RenderModel_t *vr_model;
  if (!_load_openvr_mesh (&vr_model, model_name))
    return FALSE;

  OpenVRContext *context = openvr_context_get_instance ();

  RenderModel_TextureMap_t *vr_diffuse_texture;
  if (!_load_openvr_texture (vr_model->diffuseTextureId, &vr_diffuse_texture))
    {
      context->model->FreeRenderModel (vr_model);
      return FALSE;
    }

  GulkanDevice *device = gulkan_client_get_device (gc);
  if (!_load_mesh (self, device, vr_model))
    return FALSE;

  if (!_load_texture (self, gc, vr_diffuse_texture))
    return FALSE;

  context->model->FreeRenderModel (vr_model);
  context->model->FreeTexture (vr_diffuse_texture);

  return TRUE;
}

VkSampler
xrd_scene_model_get_sampler (XrdSceneModel *self)
{
  return self->sampler;
}

GulkanVertexBuffer*
xrd_scene_model_get_vbo (XrdSceneModel *self)
{
  return self->vbo;
}

GulkanTexture*
xrd_scene_model_get_texture (XrdSceneModel *self)
{
  return self->texture;
}
