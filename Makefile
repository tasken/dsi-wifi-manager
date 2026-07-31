# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2023

BLOCKSDS	?= /opt/blocksds/core
BLOCKSDSEXT	?= /opt/blocksds/external

# User config
# ===========

# Lowercase, matching Cart-Flasher's cart_flasher, because the output name follows its
# convention below and half a convention is worse than none.
export TARGET	:= dsi_wifi_manager

GAME_TITLE	:= DSi Wi-Fi Manager

# Line 2 says what the app does, including the half that writes. It used to read
# "milestone 1: read-only", which the DSi System Menu went on showing for three milestones
# after the app gained the ability to overwrite a Wi-Fi slot. It is the last thing anyone
# reads before launching this, so it does not get to be out of date.
GAME_SUBTITLE1 := Back up and restore connections

# Build provenance
# ----------------
#
# Derived from git rather than hand-maintained, because a hand-maintained version drifts:
# this app carried "v0.4" on screen while another screen in the same build said "v0.5".
#
# --dirty is the point of the exercise. This app writes to flash, and hardware testing here
# happens from working trees with uncommitted changes. "fd40e1d" and "fd40e1d-dirty" are
# different claims about what is running, and only one of them can be traced back to a
# commit. Cart-Flasher marks its dev builds by prefix rather than by --dirty; here --dirty is
# the point, because hardware testing happens from trees with uncommitted changes.
#
# Nothing in here is a timestamp. Two builds of one commit must produce the same ROM, or a
# ROM cannot be compared against a hash, and hash comparison is how this project confirmed
# its backups were byte-exact in the first place.
DSIWIFI_COMMIT	?= $(shell git describe --always --dirty --abbrev=7 2>/dev/null || echo nogit)

DSIWIFI_BUILD_KIND ?= Dev

# Debug affordances, on for a Dev build and off for a Release one. Currently one thing:
# the ability to run a restore whose bytes already match, which programs nothing and is
# therefore the safe way to exercise writeFirmware end to end. That used to be the normal
# flow and was the recommended first hardware test; it is not something to leave in front
# of somebody restoring their home network.
# On for every build kind except Release. Nightlies are for testing, so they keep the flash
# layout screen and the forced no-op write; a Release must not put either in front of somebody
# restoring their home network.
#
# Derived from the kind rather than passed separately, so the two cannot disagree. CI asserts
# the resolved value out of build.log rather than trusting this line -- see .github/workflows.
DSIWIFI_DEBUG	?= $(if $(filter Release,$(DSIWIFI_BUILD_KIND)),0,1)
export DSIWIFI_DEBUG

# What the app prints in its banner row, and what line 3 of the ROM banner says.
#
# A release is its tag and nothing else -- "Release v1.2.3" says the same thing twice, and the
# tag is what a user reads back to you. Every other kind names itself, because "a1b2c3d" alone
# would not say whether it came from a nightly or somebody's working tree.
#
# Kept on one line: a backslash continuation inside $(if) becomes a space, and make does not
# strip it from the branches -- the banner would gain a leading space nothing else explains.
DSIWIFI_VERSION	:= $(if $(filter Release,$(DSIWIFI_BUILD_KIND)),$(DSIWIFI_COMMIT),$(DSIWIFI_BUILD_KIND) $(DSIWIFI_COMMIT))

GAME_SUBTITLE2	:= $(DSIWIFI_VERSION)

export DSIWIFI_VERSION

# Indexed 4bpp with palette index 0 reserved for transparency, generated from the artwork and
# checked in, so the build needs nothing but the container. Not in gfx/: GFXDIRS makes grit
# process everything there as a sprite sheet. See docs/BUILD.md for the palette rule.
GAME_ICON	:= resources/icon.png

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

# The output carries its provenance: the build kind and the commit, nothing else.
#
#     dsi_wifi_manager-dev-a1b2c3d.dsi        a local build
#     dsi_wifi_manager-nightly-a1b2c3d.dsi    CI, which overrides DSIWIFI_BUILD_KIND
#     dsi_wifi_manager.dsi                    a release, which overrides ROM entirely so
#                                             /releases/latest/download/ stays permanent
#
# The branch used to be in here too, following Cart-Flasher. It went: the commit identifies a
# build on its own, `git log` gives the branch back, and carrying it cost two variables and a
# character whitelist -- branch names may legally contain shell metacharacters, and this one
# reached an ndstool recipe.
#
# The kind comes from DSIWIFI_BUILD_KIND rather than being spelled again, so "Dev" on the
# banner and "dev" in the filename cannot disagree.
DSIWIFI_KIND_TAG := $(shell printf '%s' '$(DSIWIFI_BUILD_KIND)' | tr 'A-Z' 'a-z' | tr -cd 'a-z0-9')
ROM		:= $(TARGET)-$(DSIWIFI_KIND_TAG)-$(DSIWIFI_COMMIT).dsi

# Targets
# -------

.PHONY: all clean arm9 arm7

all: $(ROM)

clean:
	@echo "  CLEAN"
	$(V)$(MAKE) -f Makefile.arm9 clean --no-print-directory
	$(V)$(MAKE) -f Makefile.arm7 clean --no-print-directory
	$(V)$(RM) $(ROM) $(TARGET)-*.dsi build

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
	@echo "  BUILD   kind=$(DSIWIFI_BUILD_KIND) debug=$(DSIWIFI_DEBUG)"
	@echo "  NDSTOOL $@"
	$(V)$(BLOCKSDS)/tools/ndstool/ndstool -c $@ \
		-7 build/arm7.elf -9 build/arm9.elf \
		-b $(GAME_ICON) "$(GAME_FULL_TITLE)" \
		$(NDSTOOL_ARGS)
