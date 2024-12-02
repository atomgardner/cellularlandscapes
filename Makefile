XDG_SHELL_SPEC_PATH = /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

cellularlandscapes: cellularlandscapes.o xdg-shell-protocol.o xdg-shell-protocol.h
	gcc -O2 -Wall -o $@ $@.o xdg-shell-protocol.o -lwayland-client

xdg-shell-protocol.c:
	wayland-scanner public-code $(XDG_SHELL_SPEC_PATH) $@

xdg-shell-protocol.h:
	wayland-scanner client-header $(XDG_SHELL_SPEC_PATH) $@

%.o: %.c
	gcc -O2 -Wall -c -o $@ $^ -Wall

clean:
	rm -f cellularlandscapes xdg-shell-protocol.c xdg-shell-protocol.h *.o

.PHONY: clean
