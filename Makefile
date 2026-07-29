# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2023

BLOCKSDS	?= /opt/blocksds/core
BLOCKSDSEXT	?= /opt/blocksds/external

# User config
# ===========

NAME		:= DSiWifiManager

GAME_TITLE	:= DSi WiFi Slot Manager

# Line 2 says what the app does, including the half that writes. It used to read
# "milestone 1: read-only", which the DSi System Menu went on showing for three milestones
# after the app gained the ability to overwrite a WiFi slot. It is the last thing anyone
# reads before launching this, so it does not get to be out of date.
GAME_SUBTITLE1 := Back up and restore WiFi slots

# Build provenance
# ----------------
#
# Derived from git rather than hand-maintained, because a hand-maintained version drifts:
# this app carried "v0.4" on screen while another screen in the same build said "v0.5".
#
# --dirty is the point of the exercise. This app writes to flash, and hardware testing here
# happens from working trees with uncommitted changes. "fd40e1d" and "fd40e1d-dirty" are
# different claims about what is running, and only one of them can be traced back to a
# commit. Cart-Flasher's version of this, which docs/BUILD_VERSIONING_AND_BANNER.md
# describes, does not mark it; here it is the reason to bother.
#
# Nothing in here is a timestamp. Two builds of one commit must produce the same ROM, or a
# ROM cannot be compared against a hash, and hash comparison is how this project confirmed
# its backups were byte-exact in the first place.
DSIWIFI_COMMIT	?= $(shell git describe --always --dirty --abbrev=7 2>/dev/null || echo nogit)

# The branch reaches the ROM, and this repository is meant to stay safe to publish, so it
# gets a character whitelist rather than whatever someone happened to name a branch.
#
# Sanitised into a second variable rather than in place. A command-line override
# (make DSIWIFI_BRANCH=...) beats every assignment in the makefile, including a := that
# tried to clean the value afterwards, so cleaning in place silently does nothing for
# exactly the case where an unexpected value is most likely to arrive.
DSIWIFI_BRANCH	?= $(shell B=$$(git rev-parse --abbrev-ref HEAD 2>/dev/null); \
		     if [ -z "$$B" ]; then echo nogit; else printf '%s' "$$B"; fi)
DSIWIFI_BRANCH_SAFE := $(subst /,-,$(shell printf '%s' '$(DSIWIFI_BRANCH)' \
		         | tr -cd 'A-Za-z0-9._/-'))

DSIWIFI_BUILD_KIND ?= Dev

# Debug affordances, on for a Dev build and off for a Release one. Currently one thing:
# the ability to run a restore whose bytes already match, which programs nothing and is
# therefore the safe way to exercise writeFirmware end to end. That used to be the normal
# flow and was the recommended first hardware test; it is not something to leave in front
# of somebody restoring their home network.
DSIWIFI_DEBUG	?= $(if $(filter Release,$(DSIWIFI_BUILD_KIND)),0,1)
export DSIWIFI_DEBUG

# What the app prints in its banner row, and what line 3 of the ROM banner says. On main it
# is just the commit; on a branch the branch is worth knowing too.
ifeq ($(DSIWIFI_BRANCH_SAFE),main)
DSIWIFI_VERSION	:= $(DSIWIFI_BUILD_KIND) $(DSIWIFI_COMMIT)
else
DSIWIFI_VERSION	:= $(DSIWIFI_BUILD_KIND) $(DSIWIFI_BRANCH_SAFE)-$(DSIWIFI_COMMIT)
endif

GAME_SUBTITLE2	:= $(DSIWIFI_VERSION)

export DSIWIFI_VERSION

# Generated from resources/icon_32x32.png by tools/make_icon.py and checked in, so the
# build needs nothing but the container. It lives at the root rather than in gfx/, because
# GFXDIRS makes grit process everything in gfx/ as a sprite sheet.
#
# Re-run the tool if the artwork changes; it fails rather than shipping an icon the console
# would render with holes in it. The default $(BLOCKSDS)/sys/icon.bmp was a generic DS and
# shipped for four milestones.
GAME_ICON	:= icon.png

# Tools
# -----

MAKE		:= make
RM		:= rm -rf

# Verbose flag
# ------------

ifeq ($(VERBOSE),1)
V		:=
else
V		:= @
endif

# Directories
# -----------

ARM9DIR		:= arm9
ARM7DIR		:= arm7

# Build artfacts
# --------------

ROM		:= $(NAME).dsi

# Targets
# -------

.PHONY: all clean arm9 arm7

all: $(ROM)

clean:
	@echo "  CLEAN"
	$(V)$(MAKE) -f Makefile.arm9 clean --no-print-directory
	$(V)$(MAKE) -f Makefile.arm7 clean --no-print-directory
	$(V)$(RM) $(ROM) build

arm9:
	$(V)+$(MAKE) -f Makefile.arm9 --no-print-directory

arm7:
	$(V)+$(MAKE) -f Makefile.arm7 --no-print-directory

dump:
	$(V)+$(MAKE) -f Makefile.arm7 --no-print-directory dump
	$(V)+$(MAKE) -f Makefile.arm9 --no-print-directory dump

# Combine the title strings
GAME_FULL_TITLE := $(GAME_TITLE);$(GAME_SUBTITLE1);$(GAME_SUBTITLE2)

$(ROM): arm9 arm7
	@echo "  NDSTOOL $@"
	$(V)$(BLOCKSDS)/tools/ndstool/ndstool -c $@ \
		-7 build/arm7.elf -9 build/arm9.elf \
		-b $(GAME_ICON) "$(GAME_FULL_TITLE)" \
		$(NDSTOOL_ARGS)
