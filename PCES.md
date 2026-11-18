# PCES link-stats repeater firmware (MeshCore v1.18.0 + PCES)

Branch `pces-linkstats` of this private repo: official MeshCore v1.18.0
(`81dbae592ffad40b4212bc0b37e82b38da08f4bc`) plus the PCES link-stats change, for Commsol's Path
Computation Element Service. Spec: Commsol `docs/pces-link-stats.md`.

A repeater with this change measures how well it hears each neighbouring repeater from the
packets it receives anyway (nothing is sent on its own), and answers one anonymous request,
`0x1C`, when the request arrives direct. Its adverts carry feature 1 = `0xC504`. Everything else
is stock MeshCore v1.18.0 simple_repeater: once it is on air it repeats and adverts like any repeater.

**Status (2026-11-18): built, not flashed or tested on any device yet.**

The change: new `src/helpers/LinkStats.{h,cpp}`; hooks in `examples/simple_repeater/MyMesh.{h,cpp}`;
`CommonCLI::buildAdvertData` gets an optional feature-1 argument. Host test: `test/pces/test_linkstats.cpp`.
`git diff 81dbae5 pces-linkstats` shows all of it.

## Images (GitHub release `v1.18.0-pces1` of this repo)

The release has one download per board, a zip holding `about.txt` (what PCES and this firmware
are), `install.txt` (the steps below for that board), the images and their `SHA256SUMS`:
`heltec_v4_repeater-v1.18.0-pces1.zip`, `SenseCap_Solar_repeater-v1.18.0-pces1.zip`. The
SenseCAP's DFU package is inside as its own `.zip`: hand that one to the DFU app as it is.
Inside the bundles:

| board | file | use |
|---|---|---|
| Heltec V4 (ESP32-S3, 16 MB) | `heltec_v4_repeater-v1.18.0-pces1-update.bin` | update an existing MeshCore repeater (OTA web page, or USB at 0x10000) |
| | `heltec_v4_repeater-v1.18.0-pces1-merged.bin` | first install over USB, whole flash at 0x0 (bootloader, partitions, app) |
| Seeed SenseCAP Solar (nRF52840) | `SenseCap_Solar_repeater-v1.18.0-pces1-ota.zip` | DFU package: BLE OTA (`start ota`) or USB serial DFU; needs SoftDevice S140 7.3.0 |
| | `SenseCap_Solar_repeater-v1.18.0-pces1.hex` | SWD / J-Link only |

Check a download: `sha256sum -c SHA256SUMS` next to the downloaded files.

## Install

### Before you start

- **Settings.** An update over a running MeshCore repeater keeps its saved settings (name,
  passwords, radio, power: prefs on the internal file system). A first install, or a full
  erase, starts from MeshCore's compile-time defaults: **869.618 MHz / 62.5 kHz / SF8**, the
  board's power (**SenseCAP Solar 22 dBm**, Heltec V4 its default), name "`<board>` Repeater",
  admin password **`password`**. Set them before it goes on air.
- **Keep a stock image** for the board, to go back to: MeshCore v1.18.0 from
  [flasher.meshcore.co.uk](https://flasher.meshcore.co.uk) or the MeshCore releases.

### Seeed SenseCAP Solar: over the air (BLE)

For a node already running a MeshCore repeater (with the OTA-capable bootloader):

1. Log in to it as admin (any MeshCore app with its remote CLI, or Commsol > Repeaters > Console).
2. Send `start ota`. The node starts advertising over Bluetooth as **`SENSECAP_SOLAR_OTA`**.
3. On a phone, open Nordic's **nRF DFU** app (or **nRF Connect** > DFU), pick
   `SenseCap_Solar_repeater-v1.18.0-pces1-ota.zip`, then the `SENSECAP_SOLAR_OTA` device. Wait for
   it to finish and restart (a minute or two). Stay within a few metres: it is Bluetooth.
4. Log in again and check (below).

### Seeed SenseCAP Solar: over USB

1. Double-press its reset button: the bootloader starts (a USB drive and serial port appear).
2. `adafruit-nrfutil dfu serial --package SenseCap_Solar_repeater-v1.18.0-pces1-ota.zip -p <port> -b 115200`
   (`pip install adafruit-nrfutil`; `<port>` is COMx on Windows, `/dev/ttyACM0` on Linux).

### Heltec V4: over the air (Wi-Fi)

For a V4 already running a MeshCore repeater:

1. Log in as admin, send `start ota`. It answers `Started: http://192.168.4.1/update` and opens a
   Wi-Fi access point **`MeshCore-OTA`**.
2. Join `MeshCore-OTA` from a phone or laptop, open `http://192.168.4.1/update`, upload
   `heltec_v4_repeater-v1.18.0-pces1-update.bin`. It restarts when done.

### Heltec V4: over USB

- First install, or replacing other firmware:
  `esptool.py --chip esp32s3 write_flash 0x0 heltec_v4_repeater-v1.18.0-pces1-merged.bin`
  (hold BOOT while plugging in if it doesn't connect). This keeps the saved prefs only if the
  file system partition is untouched; treat it as a first install.
- Update only: `esptool.py --chip esp32s3 write_flash 0x10000 heltec_v4_repeater-v1.18.0-pces1-update.bin`.

### After installing: check

| command | expect |
|---|---|
| `ver` | `v1.18.0` |
| `get pces.linkstats` | `> on` |
| `get freq`, `get radio`, `get tx` | your mesh's settings (set them if a first install) |
| `linkstats` | `0 links` at first; neighbours appear as packets are heard (relayed floods, zero-hop repeater adverts) |

## CLI (admin, on the repeater)

| command | |
|---|---|
| `get pces.linkstats` / `set pces.linkstats on\|off` | default on; off: no samples, no answers, no marker (from the next advert). Saved in `/pces_ls` |
| `linkstats [N]` | the table, highest weight first, from entry N: `key snr/sd wW age`; `key?` = a hash shared by several neighbours, or not heard advertising |
| `linkstats clear` | empty the table |

Over USB serial, `linkstats` prints every entry with RSSI and sample count.

## Build

```
pio run -e heltec_v4_repeater
pio run -e SenseCap_Solar_repeater
```

Output in `.pio/build/<env>/`: `firmware.bin` (ESP32) or `firmware.zip` / `.hex` (nRF52).
Building one env wipes the others' output there. The V4 merged image is made with:

```
esptool.py --chip esp32s3 merge_bin -o firmware-merged.bin --flash_mode keep --flash_freq keep --flash_size keep \
  0x0 bootloader.bin 0x8000 partitions.bin \
  0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin 0x10000 firmware.bin
```

Builds checked 2026-11-18: `heltec_v4_repeater` (flash 18.0 %, RAM 3.0 %), `SenseCap_Solar_repeater`
(flash 45.6 %, RAM 15.3 %). No warnings from the changed files.

## New MeshCore release

`git fetch upstream --tags && git rebase --onto <new tag> 81dbae5 pces-linkstats`, build, run the
host test, check on a bench node, then a new release (`<version>-pces1`).

## Test

```
g++ -std=c++17 -I src/helpers -o /tmp/t test/pces/test_linkstats.cpp src/helpers/LinkStats.cpp && /tmp/t
```
