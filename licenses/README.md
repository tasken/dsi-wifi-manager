# Licences

The project is **GPL-3.0**, in [`../LICENSE`](../LICENSE). That governs every file that does not
say otherwise, which is how Cart-Flasher does it too: an `SPDX-License-Identifier` appears only
on a file whose terms differ from the root licence.

Four files differ:

| file | licence | why | notice |
|---|---|---|---|
| `arm9/source/dsi_only.c`, `gfx/dsiOnly_*` | MIT, (c) 2019 zoogie | from [SafeNANDManager](https://github.com/DS-Homebrew/SafeNANDManager) | [SafeNANDManager-MIT.txt](SafeNANDManager-MIT.txt) |
| `arm9/source/font_spleen.h`, `thirdparty/spleen/` | BSD-2-Clause, (c) 2018-2026 Frederic Cambus | glyph data from [Spleen](https://github.com/fcambus/spleen) | [Spleen-BSD-2-Clause.txt](Spleen-BSD-2-Clause.txt) |
| `arm9/source/fb_render.c`, `fb_render.h` | CC0-1.0 | dedicated to the public domain on purpose: the renderer has already been ported into Cart-Flasher as `fb.h`/`fb_text.h`/`fb_font.h`, and CC0 lets it keep flowing into projects under any licence | none required |

MIT and BSD-2-Clause both require their notice to travel with the code, so those two `.txt` files
must ship with any copy of this repository. CC0 requires nothing.

The build files — the three Makefiles, `Dockerfile`, `docker-compose.yml`, `build.sh`,
`.dockerignore` — derive from the [BlocksDS](https://github.com/blocksds/sdk) templates and
SafeNANDManager's skeleton. BlocksDS's are CC0-1.0;
[BlocksDS-CC0.txt](BlocksDS-CC0.txt) is credit, not an obligation.

Spleen is vendored as its original `.bdf` next to the generated C, so the notice sits beside what
it covers and `tools/make_font.py` can rebuild the table offline.

Being GPL-3.0 means code can now move both ways between this and Cart-Flasher, which is
GPL-3.0 by way of `flashcart_core`, `libncgc`, its ntrboot-derived `ui.cpp` and the Linux kernel's
`font_6x10.c`. Before this it could only move outward, via the CC0 renderer.
