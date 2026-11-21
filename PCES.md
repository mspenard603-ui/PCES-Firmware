# PCES link-stats repeater firmware (MeshCore v1.18.0 + PCES)

Branch `pces-linkstats` of this private repo: official MeshCore v1.18.0
(`81dbae592ffad40b4212bc0b37e82b38da08f4bc`) plus the PCES link-stats change, for Commsol's Path
Computation Element Service. Spec: Commsol `docs/pces-link-stats.md`.

A repeater with this change measures how well it hears each neighboring repeater from the
packets it receives anyway (nothing is sent on its own), and answers one anonymous request,
`0x1C`, when the request arrives direct. It also measures the channel
load around it (own TX airtime, decoded RX airtime, how often the channel is busy, its TX queue) and
answers `0x1D` the same way (spec: Commsol `docs/pces-load.md`). Its adverts carry feature 1 = `0xC50C`
(`0xC504` with link stats only, `0xC508` with load only). Everything else
is stock MeshCore v1.18.0 simple_repeater: once it is on air it repeats and adverts like any repeater.

**Status:** link stats running on a SenseCAP Solar since 2026-11-18. Load (2026-11-21): in the release images
since the 2026-11-21 rebuild (host test passes, not yet tested on a device). From that rebuild `ver` answers
`v1.18.0-pces1 (Build: 21 Nov 2026)` (`FIRMWARE_VERSION` in simple_repeater `MyMesh.h`); the first images said `v1.18.0`.

The change: new `src/helpers/LinkStats.{h,cpp}` and `src/helpers/LoadStats.{h,cpp}`; hooks in `examples/simple_repeater/MyMesh.{h,cpp}`;
`CommonCLI::buildAdvertData` gets an optional feature-1 argument. Host tests: `test/pces/test_linkstats.cpp`, `test/pces/test_loadstats.cpp`.
`git diff 81dbae5 pces-linkstats` shows all of it.

## Images (GitHub release `v1.18.0-pces1` of this repo)

The release has one download per board, a zip holding `about.txt` (what PCES and this firmware
are), `install.txt` (the steps below for that board), the images and their `SHA256SUMS`:
`<env>-v1.18.0-pces1.zip`. An nRF52 board's DFU package is inside as its own `-ota.zip`: hand
that one to the DFU tool as it is. Tested on hardware: SenseCAP Solar (2026-11-18). The others are
built from the same source and not yet tested. Inside the bundles:

| board | file | use |
|---|---|---|
| Heltec V4 (ESP32-S3, 16 MB) | `heltec_v4_repeater-v1.18.0-pces1-update.bin` | update an existing MeshCore repeater (OTA web page, or USB at 0x10000) |
| | `heltec_v4_repeater-v1.18.0-pces1-merged.bin` | first install over USB, whole flash at 0x0 (bootloader, partitions, app) |
| Seeed SenseCAP Solar (nRF52840) | `SenseCap_Solar_repeater-v1.18.0-pces1-ota.zip` | DFU package: BLE OTA (`start ota`, iPhone nRF DFU app) or USB serial DFU; needs SoftDevice S140 7.3.0 |
| | `SenseCap_Solar_repeater-v1.18.0-pces1.hex` | SWD / J-Link only |
| Heltec V3 (ESP32-S3, 8 MB) | `Heltec_v3_repeater-v1.18.0-pces1-update.bin`, `-merged.bin` | as the V4 |
| B&Q Station G2 (ESP32-S3, 16 MB) | `Station_G2_repeater-v1.18.0-pces1-update.bin`, `-merged.bin` | as the V4. Default 7 dBm = about 27 dBm out after its PA; MeshCore caps it at 19 |
| RAK WisBlock RAK4631 (nRF52840) | `RAK_4631_repeater-v1.18.0-pces1-ota.zip`, `.uf2`, `.hex` | DFU package (BLE OTA `start ota` as `RAK4631_OTA`, or USB serial DFU); `.uf2` drag and drop onto the bootloader drive; needs SoftDevice S140 6.1.1 |
| Heltec T114, with display (nRF52840) | `Heltec_t114_repeater-v1.18.0-pces1-ota.zip`, `.uf2`, `.hex` | as the RAK4631 (`T114_OTA`) |
| Heltec T114, without display | `Heltec_t114_without_display_repeater-v1.18.0-pces1-ota.zip`, `.uf2`, `.hex` | as the RAK4631 (`T114_OTA`) |
| Heltec MeshSolar (nRF52840) | `Heltec_mesh_solar_repeater-v1.18.0-pces1-ota.zip`, `.uf2`, `.hex` | as the RAK4631 (`MESH_SOLAR_OTA`) |

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

### Seeed SenseCAP Solar: over the air (BLE), from an iPhone

Tested 2026-11-21 on NH-AUBURN (bootloader 0.9.2-OTAFIX2.2-BP1.3): works.

1. On the phone: Nordic's **nRF Device Firmware Update** app. In its Settings: **Packet Receipt
   Notifications on, number of packets 8** (the default, off, can outrun the bootloader). Put
   `SenseCap_Solar_repeater-v1.18.0-pces1-ota.zip` in Files, as a zip. **Set Auto-Lock to Never** for
   the update: a locked phone stalls it.
2. On the node, as admin (serial, remote CLI or Commsol > Repeaters > Console): `start ota`
   (reply `OK - mac: …`; advertises as **`SENSECAP_SOLAR_OTA`**).
3. In the app: the zip (Distribution packet), the `SENSECAP_SOLAR_OTA` device, **Upload**; phone within
   a metre, app on screen, about 4 minutes. The LED blinks blue very fast during the transfer.
4. If the app loses the node right after it starts (the bootloader may come up on another BLE address,
   OTAFIX issue #63), or the transfer stops: press RST once. With the old image partly erased the
   bootloader comes up in BLE mode by itself (fast blue blink, **`SCAP_DFU`**); pick that device and
   Upload again.
5. Afterwards: slow blue blink, then check (below).

### Seeed SenseCAP Solar: BLE from a Windows PC (failed here, not recommended)

[nrf_dfu_py](https://github.com/recrof/nrf_dfu_py) / `NordicDFU-Windows-x64.exe` failed twice on
NH-AUBURN (2026-11-18 and 2026-11-21, PRN 8): the tool reports success, the image in flash is
byte-identical to `firmware.bin` (read back from `CURRENT.UF2`), yet the node stays in the bootloader
(fast blue blink, `SCAP_DFU`). The bootloader's "valid app" flag in its settings page (0xFF000) was
not written. In OTA mode OTAFIX 2.2 erases that page and writes it again as two SoftDevice flash
operations after the disconnect; a reset between them leaves it blank. Suspected, not proven: the
bootloader's disconnect handler (`dfu_transport_ble.c`, `sd_ble_gatts_sys_attr_get` under
`APP_ERROR_CHECK`) resets when the Windows client left no system attributes. Recovery: USB (below);
settings and identity are kept.

Holding the user button while pressing RST did not enter BLE DFU mode on this node (the bootloader
reads P1.01); use `start ota`.

### Over USB with the MeshCore web flasher (both boards, easiest)

Chrome or Edge on a computer (Web Serial). At [flasher.meshcore.co.uk](https://flasher.meshcore.co.uk) pick
**Custom Firmware** and choose the file; keep its name as it is:

- SenseCAP Solar: `SenseCap_Solar_repeater-v1.18.0-pces1-ota.zip`, as it is (serial DFU; the flasher's
  **Enter DFU mode** button, or double-press reset).
- Heltec V4: `…-update.bin` to update (written at 0x10000, keeps identity and settings), or `…-merged.bin`
  for a first install. The flasher goes by the name: a file ending in `-merged.bin` is written at 0x0 after
  erasing the whole flash (it warns first).

Don't use **Erase Flash** unless you mean to wipe the node's identity and settings.

### Seeed SenseCAP Solar: over USB, from the command line

1. Double-press its reset button: the bootloader starts (a USB drive and serial port appear).
2. `adafruit-nrfutil dfu serial --package SenseCap_Solar_repeater-v1.18.0-pces1-ota.zip -p <port> -b 115200`
   (`pip install adafruit-nrfutil`; `<port>` is COMx on Windows, `/dev/ttyACM0` on Linux).
   Windows: find the port after the double press (Device Manager > Ports (COM & LPT), or PowerShell
   `[System.IO.Ports.SerialPort]::GetPortNames()`); the bootloader's COM number can differ from the firmware's.

### Heltec V4: over the air (Wi-Fi)

For a V4 already running a MeshCore repeater:

1. Log in as admin, send `start ota`. It answers `Started: http://192.168.4.1/update` and opens a
   Wi-Fi access point **`MeshCore-OTA`**.
2. Join `MeshCore-OTA` from a phone or laptop, open `http://192.168.4.1/update`, upload
   `heltec_v4_repeater-v1.18.0-pces1-update.bin`. It restarts when done.

### Heltec V4: over USB, from the command line

- First install, or replacing other firmware:
  `esptool.py --chip esp32s3 write_flash 0x0 heltec_v4_repeater-v1.18.0-pces1-merged.bin`
  (hold BOOT while plugging in if it doesn't connect). This keeps the saved prefs only if the
  file system partition is untouched; treat it as a first install.
- Update only: `esptool.py --chip esp32s3 write_flash 0x10000 heltec_v4_repeater-v1.18.0-pces1-update.bin`.
- Windows: `pip install esptool`, then the same with `python -m esptool --chip esp32s3 --port COM5 …`
  (the port from Device Manager > Ports (COM & LPT); no driver needed on Windows 10/11).
- Checksums on Windows: `certutil -hashfile <file> SHA256`, compared with `SHA256SUMS`.

### After installing: check

| command | expect |
|---|---|
| `ver` | `v1.18.0-pces1 (Build: 21 Nov 2026)` (images before 2026-11-21: `v1.18.0`) |
| `get pces.linkstats` | `> on` |
| `get pces.load` | `> on` |
| `get freq`, `get radio`, `get tx` | your mesh's settings (set them if a first install) |
| `linkstats` | `0 links` at first; neighbours appear as packets are heard (relayed floods, zero-hop repeater adverts) |

## CLI (admin, on the repeater)

| command | |
|---|---|
| `get pces.linkstats` / `set pces.linkstats on\|off` | default on; off: no samples, no answers, no marker (from the next advert). Saved in `/pces_ls` |
| `linkstats [N]` | the table, highest weight first, from entry N: `key snr/sd wW age`; `key?` = a hash not resolved yet (the neighbor not heard advertising since the restart, or a hash several share). Over the air the first line is a legend (`N links: key SNR/sd dB, w=weight, age`) and about 4 rows fit; a last line `more: linkstats N` gives the command for the rest |
| `linkstats clear` | empty the table |
| `get pces.load` / `set pces.load on\|off` | default on; off: no sampling, no answers to `0x1D`, marker bit `0x0008` off (from the next advert). Saved in `/pces_ls` (2nd byte; a 1-byte file from a build without load reads as on) |
| `load` | percent of time spent on TX / RX (decoded) / channel busy, then queue avg/max, for `1m` `15m` `1h` `24h` and `peak-Nh` (the busiest complete hour of the day, N hours ago) |
| `load clear` | empty the load history |

Over USB serial, `linkstats` prints every entry with RSSI and sample count, and `load` adds the
held-by-airtime-budget share and the packet counts (rx, tx, flood, dup, err) per window.

Load sampling: the RSSI is read 20 times a second while the radio is receiving (never while it
transmits, nor before the noise floor is calibrated) and counts as busy above the noise floor + 6 dB.
A LoRa packet weaker than that (SNR below about +5 dB) doesn't register as busy; the decoded RX share
covers those it decodes. One minute is kept per bucket for the last hour, one hour for the last day:
about 3.4 kB of RAM. Nothing is sent.

Battery: no wake-ups are added. With `powersaving` off (the default) the MCU runs its loop
continuously and the radio receives continuously anyway; the RSSI reads add about 1 ms of SPI work a
second. With `powersaving on` the MCU sleeps between radio events: busy is then not sampled (its
share reads "-", answer flag bit 3), the counter-based shares (TX, RX, queue, counts) still are.

## Build

```
pio run -e heltec_v4_repeater
pio run -e SenseCap_Solar_repeater
pio run -e RAK_4631_repeater          # and Heltec_v3_repeater, Heltec_t114_repeater,
                                      # Heltec_t114_without_display_repeater,
                                      # Heltec_mesh_solar_repeater, Station_G2_repeater
```

Output in `.pio/build/<env>/`: `firmware.bin` (ESP32) or `firmware.zip` / `.hex` (nRF52).
Building one env wipes the others' output there. The V4 merged image is made with:

```
esptool.py --chip esp32s3 merge_bin -o firmware-merged.bin --flash_mode keep --flash_freq keep --flash_size keep \
  0x0 bootloader.bin 0x8000 partitions.bin \
  0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin 0x10000 firmware.bin
```

Builds 2026-11-18, batch 1 (RAM / flash used): RAK_4631_repeater 16.3 % / 54.7 %, Heltec_v3_repeater
19.4 % / 35.3 %, Heltec_t114_repeater 15.5 % / 49.3 %, Heltec_t114_without_display_repeater 15.4 % / 45.4 %,
Heltec_mesh_solar_repeater 15.0 % / 45.3 %, Station_G2_repeater 23.2 % / 92.6 % (its 1.25 MB app partition).
nRF52 `.uf2`: `uf2conv.py -c -f 0xADA52840 firmware.hex` (Adafruit nRF52 package); ESP32 merged:
`esptool merge_bin --flash_mode keep --flash_freq keep --flash_size keep` as above.
Builds checked 2026-11-18: `heltec_v4_repeater` (flash 18.0 %, RAM 3.0 %), `SenseCap_Solar_repeater`
(flash 45.6 %, RAM 15.3 %). No warnings from the changed files.
With load (2026-11-21): `heltec_v4_repeater` flash 18.0 %, RAM 3.3 %; `SenseCap_Solar_repeater` flash 46.3 %,
RAM 17.2 %; RAK_4631_repeater 18.2 % / 55.3 % (RAM / flash), Heltec_v3_repeater 20.8 % / 35.4 %,
Heltec_t114_repeater 17.4 % / 50.0 %, Heltec_t114_without_display_repeater 17.3 % / 46.1 %,
Heltec_mesh_solar_repeater 17.0 % / 46.0 %, Station_G2_repeater 24.5 % / 92.9 %. No warnings.

## New MeshCore release

`git fetch upstream --tags && git rebase --onto <new tag> 81dbae5 pces-linkstats`, build, run the
host test, check on a bench node, then a new release (`<version>-pces1`).

## Test

```
g++ -std=c++17 -I src/helpers -o /tmp/t test/pces/test_linkstats.cpp src/helpers/LinkStats.cpp && /tmp/t
g++ -std=c++17 -I src/helpers -o /tmp/tl test/pces/test_loadstats.cpp src/helpers/LoadStats.cpp && /tmp/tl
```
