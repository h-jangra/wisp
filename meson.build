project('widle', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=3'])

wayland_client = dependency('wayland-client')
freetype = dependency('freetype2')
cc = meson.get_compiler('c')
wl_scanner = find_program('wayland-scanner')

layer_shell_xml = files('protocols' / 'zwlr_layer_shell_v1.xml')
headers = generator(wl_scanner,
  output: '@BASENAME@.h',
  arguments: ['client-header', '@INPUT@', '@OUTPUT@'])
code = generator(wl_scanner,
  output: '@BASENAME@.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'])
protocol = []
protocol += headers.process(layer_shell_xml)
protocol += code.process(layer_shell_xml)
add_project_arguments('-I' + meson.current_build_dir(), language: 'c')

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
  add_project_arguments('-DWIDLE_HAS_LUA=1', language: 'c')
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
  lua = static_library('lua', lua_srcs, c_args: '-w')
  lua_link = [lua]
else
  add_project_arguments('-DWIDLE_HAS_LUA=0', language: 'c')
endif

executable('widle', 'src' / 'main.c', protocol,
  include_directories: 'third_party',
  link_with: lua_link,
  dependencies: [wayland_client, freetype],
  install: true)

# Installation data
install_data('widle.conf',
  install_dir: get_option('datadir') / 'widle')
install_data('scripts/clock.lua', 'scripts/sysmon.lua', 'scripts/battery.lua',
  install_dir: get_option('datadir') / 'widle' / 'scripts')
install_data('assets/widle.png', 'assets/widle.svg',
  install_dir: get_option('datadir') / 'widle' / 'assets')
