# PCES channel load (firmware spec, v1)

A repeater running this firmware keeps a history of how busy it and its channel are: its own TX
airtime, the airtime of packets it decodes, how often the channel is busy, its TX queue and packet
counts, per minute for the last hour and per hour for the last day. Anyone can read it with one
small anonymous request. It sends nothing on its own.

Status: in the `v1.18.0-pces1` images (MeshCore v1.18.0 simple_repeater); first run on hardware
2026-11-21. Request type `0x1D` is provisional until MeshCore allocates one.

## Why

PCES picks paths by link quality. A path over strong links through a repeater whose channel is
busy still loses packets to collisions and waits: LoRa behaves roughly like ALOHA, where the share
of packets that get through falls as e^(−2G) with offered load G. PCES can't make other nodes send
less, but it can route around busy repeaters and hold its own traffic back where and when the
channel is busy.

Stock MeshCore counts much of this already (the status response: airtime totals, packet counts,
flood dups, TX queue length), but only after a login (an ACL slot and a login round trip), only as
totals since boot (a rate needs two polls, and a repeater up for 40 days shows its 40-day average),
and without how busy the **channel** is: RX airtime counts decoded packets only, not collisions,
packets too weak to decode, or other users of the frequency.

## Measurements

`src/helpers/LoadStats.{h,cpp}`, fed from simple_repeater `MyMesh::sampleLoad()` in the main loop:

| | how | from |
|---|---|---|
| TX share | own transmit airtime | `Dispatcher::getTotalAirTime()` differences |
| RX share | airtime of decoded packets (estimated from their length) | `getReceiveAirTime()` |
| busy share | RSSI read 20 times a second while receiving; busy above noise floor + 6 dB | `getCurrentRSSI()`, `getNoiseFloor()` |
| held share | seconds with a packet ready while the airtime budget (`airtime_factor`) holds it back | `getOutboundCount()`, `getRemainingTxBudget()` |
| queue | TX queue length each second: average and maximum | `getOutboundTotal()` |
| counts | packets decoded, sent, flood received, flood dups, receive errors | radio driver, Dispatcher, `SimpleMeshTables` |

- Busy is sampled only while the radio is receiving (so its own TX never counts as busy) and only
  once the noise floor is calibrated.
- The 6 dB threshold misses LoRa packets weaker than about SNR +5 dB (RSSI over the channel
  bandwidth rises by 10·log10(1 + 10^(SNR/10)) dB). Weak packets it decodes are still in the RX
  share. Busy is a lower bound on channel occupancy.
- Interference (another system, a noisy neighbor) shows as busy without RX.

## Battery

No wake-ups, timers or transmissions are added; sampling runs inside the existing main loop.

- `powersaving off` (the default): the MCU loops continuously and the radio receives continuously
  anyway. 20 RSSI reads a second are about 1 ms of SPI work a second.
- `powersaving on`: the MCU sleeps between radio events, so the loop runs mostly right after a
  packet; busy samples would be biased high, and sampling on a timer would add wake-ups. Busy is
  therefore **not sampled** (busy `0xFFFF`, flag bit 3). TX, RX, held, queue and counts come from
  counters and stay exact.

## History

One bucket per minute for the last 60 minutes and one per hour for the last 24 hours (complete
ones; the open bucket is never reported). About 3.4 KB of RAM. Emptied on reboot and by `load
clear`. The first second after start is a baseline only (the counters ran since boot).

## The request

An anonymous request, as link stats (`link-stats.md`): no login, answered only when it arrived
**direct**, under the stock `anon_limiter` (4 per 3 minutes, shared with the other anonymous
requests).

**`0x1D` = `ANON_REQ_TYPE_LOAD`** (provisional).

```
[0x1D][reply_path_len][reply_path…][version = 1]
```

## The answer

`PAYLOAD_TYPE_RESPONSE`, direct on the reply path, little-endian, 128 bytes:

```
[tag u32][clock u32][version = 1][flags u8][noise floor int8 dBm][busy threshold dB u8]
[airtime factor ×10 u8][uptime h u16][queue now u8][pool free u8][peak ago h u8]     18 bytes
+ 5 × window: 1 min, 15 min, 1 h, 24 h, busiest hour                                5 × 22 bytes
window = [covered min u16][tx ‰ u16][rx ‰ u16][busy ‰ u16][held ‰ u16]
         [rx pkts u16][tx pkts u16][flood rx u16][flood dups u16][rx errors u16]
         [queue avg ×10 u8][queue max u8]
```

- `flags`: bit 0, load on (always 1 in an answer); bit 1, busy sampling active (noise floor
  calibrated, power saving off); bit 2, listen-before-talk on (interference threshold > 0 or CAD),
  i.e. it waits while the channel is busy; bit 3, power saving on (busy not sampled).
- A window covers only complete minutes or hours; `covered min` says how many (after a reboot the
  15 min window may cover 3). An empty window is all zero with busy `0xFFFF`; busy `0xFFFF` also
  means no sample was taken.
- Counts are totals in the window, saturating at 65535: rate = count / covered.
- Busiest hour: the complete hour of the last 24 with the highest busy share, `peak ago h` hours
  before the last complete one (`0xFF`: none yet).
- `pool free`: free packet buffers; 0 means received packets are being dropped.

## Advert marker

Feature 1 bit `0x0008` (the `0xC5xx` family, `link-stats.md`): this repeater answers load
requests. With link stats as well: `0xC50C`.

## CLI (admin)

| command | |
|---|---|
| `get pces.load` / `set pces.load on\|off` | default on. Off: no sampling, no answers, no marker bit (from the next advert). Saved with `pces.linkstats` in `/pces_ls` |
| `load` | `tx/rx/busy` % and queue `avg/max` for `1m` `15m` `1h` `24h` and `peak-Nh`. A window shorter than its name says what it covers (`15m(4)`, `24h(3h)`); one that would repeat the window before it is left out; `peak` from 2 complete hours |
| `load clear` | empty the history |

Over USB serial, `load` also prints the noise floor, the held share and the packet counts.

## Requester rules

- Only ask repeaters whose adverts carry the `0x0008` bit.
- Direct only, over a known route; never flood the request.
- At most once an hour per repeater; keep the total low (Commsol: at most 6 requests an hour,
  shared with link stats). The answer holds a day of hourly history, so hourly pulls give a daily
  profile.

## Planned use in PCES

- Best path: a per-relay factor next to the link metric, e.g. delivery odds e^(−2b) for a relay
  with busy share b, or ruling out relays above a busy limit.
- Trace planning: hold a trace back when a relay on its route reported a busy hour.
- Bots: stretch their gaps while the repeaters around the station are busy.

## Open questions

- Is 6 dB the right threshold? A bench check (busy share on a quiet channel, then at a known packet
  rate) should set it.
- 20 samples a second suits packets of ~100 ms and up (SF7 / 62.5 kHz); faster settings may need more.
