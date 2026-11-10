# swas — Makefile
#
#   make            build                               -> ./swas
#   make strict     build with -Wall -Wextra (for hacking on it)
#   make run        build and run
#   make install    install to $(PREFIX)/bin            (default: /usr)
#                   the example goes to $(PREFIX)/share/swas
#   make config     write a default config to ~/.config/swas/config
#   make link       symlink ~/.config/swas/config at the installed example
#   make uninstall
#   make clean
#
# Requires: a C compiler and the libwayland-client dev files. Nothing else —
# fonts, icons and SVGs are decoded in-tree, and the window is a Wayland
# layer-shell surface drawn by swas itself.
#
# Written/modified by: Claude Opus 4.1, Claude Sonnet 4.5, Claude Opus 5

CC          ?= cc
PKG_CONFIG  ?= pkg-config
CFLAGS      ?= -O2
PREFIX      ?= /usr
bindir      ?= $(PREFIX)/bin
datadir     ?= $(PREFIX)/share/swas

SRC         := swas.c
BIN         := swas
HDRS        := stb_truetype.h stb_image.h nanosvg.h nanosvgrast.h \
               sw_theme.h sw_draw.h sw_wl.h sw_keymap.h

# the build id covers the headers too: a fix in the shared drawing layer is
# a different build even when swas.c has not moved
BUILD       := $(shell cat $(SRC) $(HDRS) 2>/dev/null | md5sum | cut -c1-8)

WL_CFLAGS   := $(shell $(PKG_CONFIG) --cflags wayland-client 2>/dev/null)
WL_LIBS     := $(shell $(PKG_CONFIG) --libs wayland-client 2>/dev/null || echo -lwayland-client)

.PHONY: all strict run install config link uninstall clean

all: $(BIN)

$(BIN): $(SRC) $(HDRS)
	$(CC) $(CFLAGS) -DSWAS_BUILD='"$(BUILD)"' $(WL_CFLAGS) $(SRC) -o $(BIN) $(WL_LIBS) -lm
	@./$(BIN) --version

strict: $(SRC) $(HDRS)
	$(CC) -O2 -Wall -Wextra -Wno-misleading-indentation -DSWAS_BUILD='"$(BUILD)"' \
	  $(WL_CFLAGS) $(SRC) -o $(BIN) $(WL_LIBS) -lm

run: all
	./$(BIN)

install: all
	install -Dm755 $(BIN) $(DESTDIR)$(bindir)/$(BIN)
	install -Dm644 config.example $(DESTDIR)$(datadir)/config.example
	@echo "installed to $(DESTDIR)$(bindir)/$(BIN)"
	@echo "example      $(DESTDIR)$(datadir)/config.example"
	@./$(BIN) --version

config: all
	@mkdir -p $(HOME)/.config/swas
	./$(BIN) --dump-config > $(HOME)/.config/swas/config
	@echo "wrote $(HOME)/.config/swas/config"

# The shipped example, live: every install updates what you are running.
link:
	@mkdir -p $(HOME)/.config/swas
	ln -sfn $(datadir)/config.example $(HOME)/.config/swas/config
	@echo "$(HOME)/.config/swas/config -> $(datadir)/config.example"

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(BIN)
	rm -f $(DESTDIR)$(datadir)/config.example
	-rmdir $(DESTDIR)$(datadir) 2>/dev/null || true

clean:
	rm -f $(BIN)
