/*
 * OpenVR GLib
 * Copyright 2018 Collabora Ltd.
 * Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#include <glib.h>

#include "graphene-ext.h"

void
_print_translation (graphene_matrix_t *m)
{
  graphene_vec3_t t;
  graphene_matrix_get_translation (m, &t);

  float f[3];
  graphene_vec3_to_float (&t, f);

  g_print ("Translation: [%f %f %f]\n", f[0], f[1], f[2]);
}

void
_print_scale (graphene_matrix_t *m)
{
  graphene_vec3_t s;
  graphene_matrix_get_scale (m, &s);

  float f[3];
  graphene_vec3_to_float (&s, f);

  g_print ("Scale: [%f %f %f]\n", f[0], f[1], f[2]);
}

void
_print_rotation (graphene_matrix_t *m)
{
  float x, y, z;
  graphene_matrix_get_rotation_angles (m, &x, &y, &z);

  g_print ("Angles: [%f %f %f]\n", x, y, z);
}

int
main ()
{
  graphene_matrix_t mat;
  graphene_matrix_init_identity (&mat);

  g_print ("Identity:\n");
  graphene_matrix_print (&mat);
  _print_translation (&mat);
  _print_scale (&mat);
  _print_rotation (&mat);

  graphene_matrix_init_scale (&mat, 1, 2, 3);
  g_print ("Scale:\n");
  graphene_matrix_print (&mat);
  _print_translation (&mat);
  _print_scale (&mat);
  _print_rotation (&mat);

  graphene_point3d_t point = {
    .x = 1, .y = 2, .z = 3
  };
  graphene_matrix_init_translate (&mat, &point);
  g_print ("Translation:\n");
  graphene_matrix_print (&mat);
  _print_translation (&mat);
  _print_scale (&mat);
  _print_rotation (&mat);

  graphene_quaternion_t orientation;

  graphene_quaternion_init_from_angles (&orientation, 1, 2, 3);
  graphene_matrix_init_identity (&mat);
  graphene_matrix_rotate_quaternion (&mat, &orientation);
  g_print ("Rotation:\n");

  graphene_matrix_print (&mat);
  _print_translation (&mat);
  _print_scale (&mat);
  _print_rotation (&mat);

  g_print ("Rotation quat: ");
  graphene_quaternion_print (&orientation);
  graphene_quaternion_t q;
  graphene_matrix_get_rotation_quaternion (&mat, &q);
  g_print ("Result quat: ");
  graphene_quaternion_print (&q);

  return 0;
}