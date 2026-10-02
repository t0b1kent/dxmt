lib_d3d9 = cpp.find_library('d3d9')
lib_user32 = cpp.find_library('user32')
lib_gdi32 = cpp.find_library('gdi32')

executable('dx9_present_probe', ['dx9_present_probe.cpp'],
  dependencies: [lib_d3d9, lib_user32, lib_gdi32]
)

