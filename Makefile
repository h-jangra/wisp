CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g -Ibuild -isystem third_party -isystem third_party/lua -isystem third_party/nanovg -Isrc -DWISP_HAS_LUA=1 -DWISP_DATA_DIR='"/usr/share/wisp"' -DLUA_USE_POSIX
LDFLAGS ?= -lm -lz -pthread
PKG_DEPS = wayland-client wayland-egl egl glesv2 freetype2 xkbcommon libsystemd libpulse libqrencode zbar

CFLAGS += $(shell pkg-config --cflags $(PKG_DEPS))
LDFLAGS += $(shell pkg-config --libs $(PKG_DEPS))

BUILD_DIR = build

SOURCES = \
	src/core/config.c \
	src/backends/backend.c \
	src/backends/generic.c \
	src/backends/hyprland.c \
	src/backends/sway.c \
	src/widget/widget.c \
	src/widget/lua_widget.c \
	src/ui/lua_ui.c \
	src/core/app.c \
	src/core/main.c \
	src/core/selftest.c \
	src/core/animation.c \
	src/core/interaction.c \
	src/ipc/ipc.c \
	src/ipc/frame.c \
	src/scripting/scripting.c \
	src/wayland/wayland.c \
	src/render/buffer.c \
	src/render/renderer_native.c \
	src/render/renderer_egl.c \
	src/render/clay_nanovg.c \
	src/render/image.c \
	src/render/text.c \
	src/render/shapes.c \
	src/render/icon.c \
	src/render/vector.c \
	src/render/clay_render.c \
	src/layout/layout.c \
	src/layout/clay_layout.c \
	src/services/system.c \
	src/services/clipboard.c \
	src/services/keyboard.c \
	src/services/screen.c \
	src/services/process.c \
	src/services/fs.c \
	src/services/audio.c \
	src/services/qr.c \
	src/services/service.c \
	src/services/notify.c \
	third_party/nanovg/nanovg.c \
	$(BUILD_DIR)/zwlr_layer_shell_v1.c \
	$(BUILD_DIR)/cursor_shape_v1.c \
	$(BUILD_DIR)/xdg_shell.c \
	$(BUILD_DIR)/zwlr_data_control_v1.c \
	$(BUILD_DIR)/zwlr_screencopy_v1.c

LUA_SOURCES = \
	third_party/lua/lapi.c third_party/lua/lauxlib.c third_party/lua/lbaselib.c \
	third_party/lua/lcode.c third_party/lua/lcorolib.c third_party/lua/lctype.c \
	third_party/lua/ldblib.c third_party/lua/ldebug.c third_party/lua/ldo.c \
	third_party/lua/ldump.c third_party/lua/lfunc.c third_party/lua/lgc.c \
	third_party/lua/linit.c third_party/lua/liolib.c third_party/lua/llex.c \
	third_party/lua/lmathlib.c third_party/lua/lmem.c third_party/lua/loadlib.c \
	third_party/lua/lobject.c third_party/lua/lopcodes.c third_party/lua/loslib.c \
	third_party/lua/lparser.c third_party/lua/lstate.c third_party/lua/lstring.c \
	third_party/lua/lstrlib.c third_party/lua/ltable.c third_party/lua/ltablib.c \
	third_party/lua/ltm.c third_party/lua/lundump.c third_party/lua/lutf8lib.c \
	third_party/lua/lvm.c third_party/lua/lzio.c

all: $(BUILD_DIR)/wisp

$(BUILD_DIR)/zwlr_layer_shell_v1.h: protocols/zwlr_layer_shell_v1.xml
	mkdir -p $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/zwlr_layer_shell_v1.c: protocols/zwlr_layer_shell_v1.xml $(BUILD_DIR)/zwlr_layer_shell_v1.h
	mkdir -p $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/cursor_shape_v1.h: protocols/cursor_shape_v1.xml
	mkdir -p $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/cursor_shape_v1.c: protocols/cursor_shape_v1.xml $(BUILD_DIR)/cursor_shape_v1.h
	mkdir -p $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/xdg_shell.h: protocols/xdg_shell.xml
	mkdir -p $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/xdg_shell.c: protocols/xdg_shell.xml $(BUILD_DIR)/xdg_shell.h
	mkdir -p $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/zwlr_data_control_v1.h: protocols/zwlr_data_control_v1.xml
	mkdir -p $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/zwlr_data_control_v1.c: protocols/zwlr_data_control_v1.xml $(BUILD_DIR)/zwlr_data_control_v1.h
	mkdir -p $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/zwlr_screencopy_v1.h: protocols/zwlr_screencopy_v1.xml
	mkdir -p $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/zwlr_screencopy_v1.c: protocols/zwlr_screencopy_v1.xml $(BUILD_DIR)/zwlr_screencopy_v1.h
	mkdir -p $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/wisp: $(BUILD_DIR)/zwlr_layer_shell_v1.c $(BUILD_DIR)/cursor_shape_v1.c $(BUILD_DIR)/xdg_shell.c $(BUILD_DIR)/zwlr_data_control_v1.c $(BUILD_DIR)/zwlr_screencopy_v1.c $(SOURCES) $(LUA_SOURCES)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SOURCES) $(LUA_SOURCES) $(LDFLAGS) -o $@

run: all
	./$(BUILD_DIR)/wisp

test: all
	./$(BUILD_DIR)/wisp --selftest

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean all

# Installation directories (overridable; DESTDIR for staged installs)
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
WISP_DATADIR ?= $(DATADIR)/wisp
LSPDIR ?= $(WISP_DATADIR)/lsp

DESTDIR ?=

LSP_FILES := $(wildcard docs/lsp/*)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BUILD_DIR)/wisp $(DESTDIR)$(BINDIR)/wisp
	install -d $(DESTDIR)$(LSPDIR)
	for f in $(LSP_FILES); do \
		install -m 0644 $$f $(DESTDIR)$(LSPDIR)/$$(basename $$f); \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/wisp
	for f in $(LSP_FILES); do \
		rm -f $(DESTDIR)$(LSPDIR)/$$(basename $$f); \
	done
	rmdir $(DESTDIR)$(LSPDIR) 2>/dev/null || true

.PHONY: all run test clean rebuild install uninstall

# M1 IPC args integration test
test-m1-ipc: $(BUILD_DIR)/wisp
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(PKG_DEPS)) tests/m1_ipc_args.c $(filter-out src/core/main.c,$(SOURCES)) $(LUA_SOURCES) $(LDFLAGS) -o $(BUILD_DIR)/m1_ipc_args && $(BUILD_DIR)/m1_ipc_args
