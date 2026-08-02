# flickrfree - invisible VRR keep-alive for KWin/Wayland
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PKGS    := wayland-client
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS ?= /usr/share/wayland-protocols

PROTO   := protocols/wlr-layer-shell-v1-protocol.c \
           protocols/single-pixel-buffer-v1-protocol.c \
           protocols/xdg-shell-protocol.c
HDRS    := protocols/wlr-layer-shell-v1-client.h \
           protocols/single-pixel-buffer-v1-client.h \
           protocols/xdg-shell-client.h

all: flickrfree

flickrfree: flickrfree.c sni.c sni.h $(PROTO) $(HDRS)
	$(CC) $(CFLAGS) -o $@ flickrfree.c sni.c $(PROTO) $(shell pkg-config --cflags --libs $(PKGS)) -lsystemd

# --- regenerate protocol bindings (only needed if the XML versions change) ---
protocols/wlr-layer-shell-v1-client.h: protocols/wlr-layer-shell-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@
protocols/wlr-layer-shell-v1-protocol.c: protocols/wlr-layer-shell-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

protocols/single-pixel-buffer-v1-client.h: protocols/single-pixel-buffer-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@
protocols/single-pixel-buffer-v1-protocol.c: protocols/single-pixel-buffer-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

protocols/xdg-shell-client.h: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	$(WAYLAND_SCANNER) client-header $< $@
protocols/xdg-shell-protocol.c: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	$(WAYLAND_SCANNER) private-code $< $@

clean:
	rm -f flickrfree

.PHONY: all clean
