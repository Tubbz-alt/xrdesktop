executable(
  'overlay_models', ['overlay_models.c', test_resources],
  dependencies: xrdesktop_deps,
  link_with: xrdesktop_lib,
  include_directories: xrdesktop_inc,
  install: false)

executable(
  'client', ['client.c', test_resources, shader_resources],
  dependencies: xrdesktop_deps,
  link_with: xrdesktop_lib,
  include_directories: xrdesktop_inc,
  install: false)

executable(
  'hand-shake', ['hand-shake.c', test_resources, shader_resources],
  dependencies: xrdesktop_deps,
  link_with: xrdesktop_lib,
  include_directories: xrdesktop_inc,
  install: false)

executable(
  'settings', ['settings.c'],
  dependencies: xrdesktop_deps,
  link_with: xrdesktop_lib,
  include_directories: xrdesktop_inc,
  install: false)
