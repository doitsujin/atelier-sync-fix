project('atelier-sync-fix', ['c', 'cpp'], version : 'v0.5', meson_version : '>= 0.47', default_options : [ 'cpp_std=c++17' ])

cpp = meson.get_compiler('cpp')

compiler_args = [
  '-DNOMINMAX',
  '-D_WIN32_WINNT=0xa00',
  '-Wimplicit-fallthrough',
]

link_args = [
  '-static',
  '-static-libgcc',
  '-static-libstdc++',
  # We need to set the section alignment for debug symbols to
  # work properly as well as avoiding a memcpy from the Wine loader.
  '-Wl,--file-alignment=4096',
]

# Wine's built-in back traces only work with dwarf2 symbols
if get_option('debug') and target_machine.system() == 'windows'
  compiler_args += [
    '-gstrict-dwarf',
    '-gdwarf-2',
  ]
endif

add_project_arguments(cpp.get_supported_arguments(compiler_args), language: 'cpp')
add_project_arguments(cpp.get_supported_arguments(compiler_args), language: 'c')

add_project_link_arguments(cpp.get_supported_link_arguments(link_args), language: 'cpp')
add_project_link_arguments(cpp.get_supported_link_arguments(link_args), language: 'c')

d3d11_src = files([
  'impl.cpp',
  'main.cpp',
])

minhook_src = files([
  'minhook/src/hde/hde64.c',
  'minhook/src/hook.c',
  'minhook/src/buffer.c',
  'minhook/src/trampoline.c',
])

d3d11_dll = shared_library('d3d11', d3d11_src, minhook_src,
  name_prefix         : '',
  install             : true,
  vs_module_defs      : 'd3d11.def',
)
