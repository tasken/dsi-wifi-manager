# DSi Wi-Fi Manager

A DSi homebrew application to back up and restore the console's six Wi-Fi connections to/from the SD card.

## Getting started

Build the app (see below) and place the resulting `.dsi` on your SD card. You need a DSi that can launch DSi-mode homebrew, which usually means Unlaunch.

1. Launch DSi Wi-Fi Manager from your homebrew launcher.
1. Pick a connection from the list and press `A` to open it.
1. To save a copy, select `Back up`, then confirm. Your backup is saved to `DSIWIFI/<your MAC>` on the SD card.
1. To put one back, select `Restore`, pick a backup, choose whether to keep an undo copy, then enter the key combo to proceed. Get the combo wrong and it just starts over, so a mistyped press does not lose the restore.

Connections 1-3 and 4-6 store settings differently, so a backup only fits the group it came from. The app offers the ones that fit and refuses the rest.

> [!CAUTION]
> This writes to your console's firmware flash, the same chip System Settings uses when you set up Wi-Fi. It has been tested on one DSi. There is no warranty of any kind.

> [!WARNING]
> **A backup contains your Wi-Fi password in plain text**, because that is how the console stores it. Anything that can read your SD card can read the password. The app says so on screen before writing the file.

> [!NOTE]
> It will not run on a DS, a DS Lite, or a DSi booted in DS mode. C

## Building

```shell
./build.sh
```

Builds inside Docker with the BlocksDS toolchain included. [docs/BUILD.md](docs/BUILD.md) covers the output naming and versioning; [docs/TESTING.md](docs/TESTING.md) covers the offline test suite.

## Documentation

*   [HARDWARE.md](docs/HARDWARE.md) — where the settings live, the record layout, and what the firmware rewrites behind your back
*   [ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the app is put together, and the invariants
*   [TESTING.md](docs/TESTING.md) — the test suite and its inputs
*   [SECURITY.md](docs/SECURITY.md) — what is sensitive in a dump or a backup
*   [BUILD.md](docs/BUILD.md) — building, versioning and the banner

## Credits

*   Developed by `Tasken`
*   Prior art and references:
    *   [fwTool](https://github.com/ahezard/nintendo-ds-tools) by `ahezard`, which reads these settings. Its `Restore Wifi Settings` was never implemented, which is why this exists.
    *   [WifiManager](https://github.com/Zakary2841/WifiManager) by `Zakary2841`, the 3DS equivalent and the inspiration. Different platform, different storage; a reference, not a port.
    *   [dsibiosdumper](https://github.com/Arisotura/dsibiosdumper) by `Arisotura`, for making the firmware dumps this was built against.
    *   [GBATEK](https://problemkaputt.de/gbatek-ds-firmware-wifi-internet-access-points.htm) by `Martin Korth`, for the record layout.
*   Borrowed code:
    *   [SafeNANDManager](https://github.com/DS-Homebrew/SafeNANDManager) by `zoogie`, for the DSi-mode gate and the Docker build skeleton
    *   [BlocksDS](https://github.com/blocksds/sdk) by `Antonio Niño Díaz`, for the SDK and the Makefile templates
    *   [Spleen](https://github.com/fcambus/spleen) by `Frederic Cambus`, for the 5x8 font

The key combo confirmation before writing is styled after `d0k3`'s [GodMode9](https://github.com/d0k3/GodMode9) unlock sequence prompt, by way of [Cart-Flasher](https://github.com/tasken/Cart-Flasher).

## License

GPL-3.0 - see [LICENSE](LICENSE). Copyright © 2026 Augusto Daniele.

Four files keep their own licence and say so in their SPDX tag. [`licenses/README.md`](licenses/README.md) lists which, and reproduces every notice those licences require.

Nintendo, Nintendo DS and Nintendo DSi are trademarks of Nintendo. This project is not affiliated with, endorsed by, or connected to Nintendo in any way, and contains no Nintendo code or data.
