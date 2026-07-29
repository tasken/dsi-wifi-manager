# Third-party licences

The project's own licence is `../LICENSE` (MIT, Copyright (c) 2026 Augusto
Daniele). It covers the code written for this project and nothing else.

A few files came from elsewhere and keep their original licence. Those licences
are reproduced here, in their own files, so it is obvious which terms apply to
what.

| file | project | licence | applies to |
|---|---|---|---|
| `SafeNANDManager-MIT.txt` | [DS-Homebrew/SafeNANDManager](https://github.com/DS-Homebrew/SafeNANDManager) | MIT, (c) 2019 zoogie | `arm9/source/dsi_only.c`, `gfx/dsiOnly_*.png`, `gfx/dsiOnly_*.grit`, `Dockerfile`, `docker-compose.yml`, `run.sh`, `.dockerignore` |
| `BlocksDS-CC0.txt` | [blocksds/sdk](https://github.com/blocksds/sdk) | CC0-1.0, Antonio Niño Díaz 2023 | `Makefile`, `Makefile.arm7`, `Makefile.arm9` |
| `Spleen-BSD-2-Clause.txt` | [fcambus/spleen](https://github.com/fcambus/spleen) | BSD-2-Clause, (c) 2018-2026 Frederic Cambus | `thirdparty/spleen/spleen-5x8.bdf` and the glyph table generated from it, `arm9/source/font_spleen.h` |

MIT and BSD-2-Clause both require their notice to be included wherever the code
goes, so `SafeNANDManager-MIT.txt` and `Spleen-BSD-2-Clause.txt` have to ship
with any copy of this repository. CC0 requires nothing; `BlocksDS-CC0.txt` is
credit, not an obligation.

The Spleen font is vendored as its original `.bdf` rather than only as generated
C, so the notice sits next to the thing it covers and the table can be rebuilt
with `tools/make_font.py` without going back to the network.

Anything not listed above is covered by `../LICENSE`.
