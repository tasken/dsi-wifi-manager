<p align="center">
  <img src="resources/logo.png" alt="DSi Wi-Fi Manager Logo" width="128">
</p>

# DSi Wi-Fi Manager

Back up and restore the Wi-Fi connections stored in a Nintendo DSi's firmware.

## Getting started

Download [`dsi_wifi_manager.dsi`](https://github.com/tasken/dsi-wifi-manager/releases/latest/download/dsi_wifi_manager.dsi) and place it on your SD card. You need a DSi that can run DSi-mode homebrew.

1. Launch DSi Wi-Fi Manager from your homebrew launcher.
1. Pick a connection from the list and press `A` to open it.
1. Select `Back up` to save that connection to `DSIWIFI/<your MAC>/` on the SD card.
1. Select `Restore` to choose a backup, optionally save the current connection as an undo copy, then enter the on-screen key combo.

Connections 1-3 and 4-6 use different formats: a backup from 1-3 restores only to 1-3, and a backup from 4-6 restores only to 4-6. The app shows only compatible backups.

Restore scans every folder directly inside `DSIWIFI/`, so a backup copied from another DSi can be restored too. The app writes one connection per file, and can also restore one compatible connection from a multi-record backup.

> [!CAUTION]
> This writes to your console's firmware flash, the same chip System Settings uses when you set up Wi-Fi. It has been tested on one DSi. There is no warranty of any kind.

> [!WARNING]
> **A backup may contain Wi-Fi passwords in plain text**, because that is how the console stores them. Protect your SD card accordingly. The app says so on screen before writing the file.

> [!NOTE]
> It will not run on a DS, a DS Lite, or a DSi booted in DS mode. Connections 4-6 exist only on a DSi, and the app needs DSi mode to reach the SD card.

## Building

```shell
./build.sh
```

Builds inside Docker with the BlocksDS toolchain included. Releases are named `dsi_wifi_manager.dsi`; local and nightly builds carry their build kind and commit.

The offline test suite runs the app's own portable sources on a PC:

```shell
python3 tools/crosscheck.py <flash images...>
```

## Credits

Developed by [Augusto Daniele](https://github.com/tasken).

### Code and assets

- [SafeNANDManager](https://github.com/DS-Homebrew/SafeNANDManager) supplies the DSi-mode check and original build skeleton. It is DS-Homebrew's fork of [zoogie's dumpTool](https://github.com/zoogie/dumpTool).
- [BlocksDS](https://github.com/blocksds/sdk), by Antonio Niño Díaz, provides the SDK and Makefile templates.
- [Spleen](https://github.com/fcambus/spleen), by Frederic Cambus, provides the 5x8 font.

### Research and inspiration

- [fwTool](https://github.com/ahezard/nintendo-ds-tools), by ahezard, and [GBATEK](https://problemkaputt.de/gbatek-ds-firmware-wifi-internet-access-points.htm), by Martin Korth, informed the firmware layout.
- [dsibiosdumper](https://github.com/Arisotura/dsibiosdumper), by Arisotura, created the firmware dumps used for validation.
- [WifiManager](https://github.com/Zakary2841/WifiManager), Zakary2841's fork of [LiquidFenrir's original](https://github.com/LiquidFenrir/WifiManager), is the 3DS design reference. This is not a port.
- [GodMode9](https://github.com/d0k3/GodMode9), by d0k3, inspired the key-combo confirmation style.

## License

GPL-3.0 - see [LICENSE](LICENSE). Copyright © 2026 Augusto Daniele.

Some files keep their own licence and SPDX tag. [`licenses/README.md`](licenses/README.md) lists
them and reproduces the required notices.

Nintendo, Nintendo DS and Nintendo DSi are trademarks of Nintendo. This project is not affiliated with, endorsed by, or connected to Nintendo in any way, and contains no Nintendo code or data.
