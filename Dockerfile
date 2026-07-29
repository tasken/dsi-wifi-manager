# SPDX-License-Identifier: CC0-1.0
#
# Build container for SafeNANDManager (NDS/DSi homebrew, BlocksDS toolchain).
# The official image ships the SDK fully configured (BLOCKSDS, BLOCKSDSEXT,
# WONDERFUL_TOOLCHAIN already set) — no local toolchain install needed.
FROM skylyrac/blocksds:slim-latest

RUN git config --global --add safe.directory /work

WORKDIR /work
