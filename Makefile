# xp-craft — cross-compiled from Linux for Windows XP (32-bit).
# Needs: gcc-mingw-w64-i686. D3D9 import lib ships with mingw-w64.

CC      = i686-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99
LDFLAGS = -ld3d9 -lgdi32 -luser32 -mwindows -static-libgcc

BUILD   = build
DEPLOY  = /media/Acer_Notebook/xp-craft

SRC     = $(wildcard src/*.c)

all: $(BUILD)/xp-craft.exe

$(BUILD)/xp-craft.exe: $(SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# Copy onto the XP box via the SMB share (the share is the box's local disk).
# Kills a running instance first: XP holds the exe locked while running.
deploy: all kill
	mkdir -p $(DEPLOY)
	cp $(BUILD)/xp-craft.exe $(DEPLOY)/

# Launch on the box (lands on the TV) and grab a screenshot to verify.
# FPS is in the window title, so the screenshot captures it.
run:
	~/bin/xprun 'start "" "C:\XP_Share\xp-craft\xp-craft.exe"'
	sleep 5 && ~/bin/xpshot /tmp/xp-craft-shot.png

# Scripted benchmark: cycles view ranges, writes bench.txt next to the exe
# (= on the share), which we then read straight from Linux.
bench:
	~/bin/xprun 'cd /d C:\XP_Share\xp-craft && start "" xp-craft.exe bench'
	@echo "benchmark running (~60s)..." && sleep 75
	cat $(DEPLOY)/bench.txt

kill:
	-~/bin/xprun 'taskkill /f /im xp-craft.exe' >/dev/null 2>&1

clean:
	rm -rf $(BUILD)

.PHONY: all deploy run bench kill clean
