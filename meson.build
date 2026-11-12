project('wisp', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=3'])

wayland_client = dependency('wayland-client')
wayland_egl = dependency('wayland-egl')
egl = dependency('egl')
glesv2 = dependency('glesv2')
freetype = dependency('freetype2')
xkbcommon = dependency('xkbcommon')
pulse = dependency('libpulse', required: false)
qrencode = dependency('libqrencode', required: false)
zbar = dependency('zbar', required: false)
systemd = dependency('libsystemd', required: false)
zlib = dependency('zlib')
threads = dependency('threads')
cc = meson.get_compiler('c')
m = cc.find_library('m', required: false)
wl_scanner = find_program('wayland-scanner')

layer_shell_xml = files('protocols' / 'zwlr_layer_shell_v1.xml')
cursor_shape_xml = files('protocols' / 'cursor_shape_v1.xml')
xdg_shell_xml = files('protocols' / 'xdg_shell.xml')
data_control_xml = files('protocols' / 'zwlr_data_control_v1.xml')
screencopy_xml = files('protocols' / 'zwlr_screencopy_v1.xml')
headers = generator(wl_scanner,
  output: '@BASENAME@.h',
  arguments: ['client-header', '@INPUT@', '@OUTPUT@'])
code = generator(wl_scanner,
  output: '@BASENAME@.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'])
protocol = []
protocol += headers.process(layer_shell_xml)
protocol += code.process(layer_shell_xml)
protocol += headers.process(cursor_shape_xml)
protocol += code.process(cursor_shape_xml)
protocol += headers.process(xdg_shell_xml)
protocol += code.process(xdg_shell_xml)
protocol += headers.process(data_control_xml)
protocol += code.process(data_control_xml)
protocol += headers.process(screencopy_xml)
protocol += code.process(screencopy_xml)
add_project_arguments('-I' + meson.current_build_dir(), '-ffunction-sections', '-fdata-sections', language: 'c')
add_project_link_arguments('-Wl,--gc-sections', language: 'c')

wisp_datadir = get_option('prefix') / get_option('datadir') / 'wisp'
add_project_arguments('-DWISP_DATA_DIR="' + wisp_datadir + '"', language: 'c')

san_supported = cc.links('__attribute__((weak)) int f(void){return 0;} int main(void){return f();}',
  args: ['-fsanitize=address,undefined'])
if get_option('sanitizers') and not san_supported
  error('ASan/UBSan requested but this toolchain cannot link them. ' +
    'Use clang (bundles the runtime): CC=clang meson setup build, ' +
    'or install the gcc sanitizer runtime (Void: gcc-sanitizers), ' +
    'or pass -Dsanitizers=false')
endif
if get_option('sanitizers')
  add_project_arguments('-fsanitize=address,undefined', language: 'c')
  add_project_link_arguments('-fsanitize=address,undefined', language: 'c')
endif

lua_opt = get_option('lua')
lua_link = []
if lua_opt
  add_project_arguments('-DWISP_HAS_LUA=1', language: 'c')
  lua_srcs = [
    'third_party/lua/lapi.c', 'third_party/lua/lauxlib.c', 'third_party/lua/lbaselib.c',
    'third_party/lua/lcode.c', 'third_party/lua/lcorolib.c', 'third_party/lua/lctype.c',
    'third_party/lua/ldblib.c', 'third_party/lua/ldebug.c', 'third_party/lua/ldo.c',
    'third_party/lua/ldump.c', 'third_party/lua/lfunc.c', 'third_party/lua/lgc.c',
    'third_party/lua/linit.c', 'third_party/lua/liolib.c', 'third_party/lua/llex.c',
    'third_party/lua/lmathlib.c', 'third_party/lua/lmem.c', 'third_party/lua/loadlib.c',
    'third_party/lua/lobject.c', 'third_party/lua/lopcodes.c', 'third_party/lua/loslib.c',
    'third_party/lua/lparser.c', 'third_party/lua/lstate.c', 'third_party/lua/lstring.c',
    'third_party/lua/lstrlib.c', 'third_party/lua/ltable.c', 'third_party/lua/ltablib.c',
    'third_party/lua/ltm.c', 'third_party/lua/lundump.c', 'third_party/lua/lutf8lib.c',
    'third_party/lua/lvm.c', 'third_party/lua/lzio.c'
  ]
  lua = static_library('lua', lua_srcs, c_args: ['-w', '-DLUA_USE_POSIX'])
  lua_link = [lua]
else
  add_project_arguments('-DWISP_HAS_LUA=0', language: 'c')
endif

wisp_sources = [
  'src' / 'core' / 'config.c',
  'src' / 'backends' / 'backend.c',
  'src' / 'backends' / 'generic.c',
  'src' / 'backends' / 'hyprland.c',
  'src' / 'backends' / 'sway.c',
  'src' / 'core' / 'widget.c',
  'src' / 'core' / 'app.c',
  'src' / 'core' / 'main.c',
  'src' / 'core' / 'animation.c',
  'src' / 'core' / 'interaction.c',
  'src' / 'ipc' / 'ipc.c',
  'src' / 'scripting' / 'scripting.c',
  'src' / 'wayland' / 'wayland.c',
  'src' / 'render' / 'buffer.c',
  'src' / 'render' / 'renderer_native.c',
  'src' / 'render' / 'renderer_egl.c',
  'src' / 'render' / 'clay_nanovg.c',
  'src' / 'render' / 'image.c',
  'src' / 'render' / 'text.c',
  'src' / 'render' / 'shapes.c',
  'src' / 'render' / 'vector.c',
  'src' / 'render' / 'clay_render.c',
  'src' / 'layout' / 'layout.c',
  'src' / 'layout' / 'clay_layout.c',
  'src' / 'services' / 'system.c',
  'src' / 'services' / 'clipboard.c',
  'src' / 'services' / 'keyboard.c',
  'src' / 'services' / 'screen.c',
  'src' / 'services' / 'process.c',
  'src' / 'services' / 'fs.c',
  'src' / 'services' / 'audio.c',
  'src' / 'services' / 'qr.c',
  'src' / 'services' / 'notify.c',
  'third_party' / 'nanovg' / 'nanovg.c',
]

executable('wisp', wisp_sources, protocol,
  include_directories: ['third_party', 'third_party/nanovg', 'src'],
  link_with: lua_link,
  dependencies: [wayland_client, wayland_egl, egl, glesv2, freetype, m, xkbcommon, zlib, threads, pulse, qrencode, zbar, systemd],
  install: true)

# Installation data
install_data('shell/config.lua',
  install_dir: get_option('datadir') / 'wisp',
  rename: 'wisp.lua')
install_subdir('shell',
  install_dir: get_option('datadir') / 'wisp',
  strip_directory: false)
install_data(
  'example/scripts/clock.lua',
  'example/scripts/sysmon.lua',
  'example/scripts/battery.lua',
  'example/scripts/panel.lua',
  'example/scripts/calendar.lua',
  'example/scripts/nested_layouts.lua',
  'example/scripts/button_demo.lua',
  install_dir: get_option('datadir') / 'wisp' / 'scripts')
install_data('assets/wisp.png', 'assets/wisp.svg',
  install_dir: get_option('datadir') / 'wisp' / 'assets')
