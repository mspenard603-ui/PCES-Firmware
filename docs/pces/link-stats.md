# PCES link statistics (firmware spec, v1)

A repeater running this firmware measures how well it hears each neighboring repeater, from the
packets it receives anyway, and answers one small anonymous request with the table. It sends
nothing on its own.

Status: in the `v1.18.0-pces1` images (MeshCore v1.18.0 simple_repeater). Request type `0x1C` is
provisional until MeshCore allocates one.

## Why

PCES (Path Computation Element Service, named after RFC 4655) answers "what is the best path from
A to B?" from measured link quality. A PCES server can measure links only near its own station: its
own traces, what it hears, the neighbor tables it can reach. Whereas a repeater hears its own links all
the time. Every flood packet it receives names the hop that just sent it (the last hash in the
path), so it can measure **neighbor → itself** on every packet without transmitting. With the
firmware on both ends, a link is measured both ways.

## Samples

Taken in `logRx()`, which `Dispatcher::checkRecv()` calls for every packet parsed (duplicates
included, before routing), with `snr = pkt->getSNR()` and `rssi = _radio->getLastRSSI()`.
`hops` is the number of hashes already in a flood's path (`getPathHashCount()`, bits 0-5 of the
path-length byte): each repeater that relays a flood appends its hash, so `path[hops-1]` is the
repeater that sent the copy just heard. A flood arriving with path `6E 8B F7` has hops = 3, sent by
F7. A TRACE's path field holds SNR bytes instead (`snrs` of them, one for each hop completed so far).

| packet | sender of this copy | sampled |
|---|---|---|
| flood with hops ≥ 1, not TRACE | `path[hops-1]`, width `getPathHashSize()` | yes |
| ADVERT with path count 0, repeater or room server | the advert's public key | yes (SNR only) |
| ADVERT with path count 0, any other type (companion, sensor) | | **no**: no public list of the phones a repeater hears |
| other flood, path count 0 | the originator, not named | no |
| TRACE (direct) with snrs ≥ 1 SNR bytes in its path | route hop snrs-1 (width `1 << (flags & 3)`) | yes |
| other direct packets | not in the packet (the path holds the hops still ahead) | no |

Last hops are always relays, so apart from zero-hop adverts only repeaters and room servers are
counted. A hash equal to the repeater's own is skipped.

## Keys

A sample is stored against the neighbor's **public key prefix** (4 bytes) when its hash matches
exactly one entry of the legacy neighbor table (filled from zero-hop repeater adverts). Otherwise it
is stored against the **hash as heard** (1 to 3 bytes) and flagged unresolved; the requester places
it, for example by location.

One neighbor can be heard under several keys: its 1-, 2- or 3-byte path hash (each packet's
originator chooses the width) and, once resolved, its key prefix. A key that extends another
(`F7C3` extends `F7`, `F7C3A1B2` extends `F7C3`) is taken as the same neighbor when nothing else in
the table extends the shorter one: the entry is widened, keeping its history, or the shorter entry
is pooled into the longer one. A short hash that two longer entries extend stays a separate entry.

## Statistics

Table `LinkStats`, 32 entries (`MAX_LINK_STATS`), RAM only (about 1 KB), emptied on reboot. The
legacy neighbor table is not touched.

| field | type | |
|---|---|---|
| key | 4 bytes | key prefix (resolved) or the hash as heard, zero-padded |
| kind | u8 | bits 0-1: hash width − 1 (unresolved); bit 2: resolved; bit 3: from adverts only so far |
| W | float | sample weight, exponentially aged |
| mean | float | SNR, dB, weighted |
| S | float | weighted sum of squares about the mean |
| rssi | float | RSSI, dBm, weighted mean (adverts carry no RSSI here) |
| t_last | u32 | RTC time of the last sample |
| n | u16 | samples ever, saturating |

Each sample x at time t, half-life H = 48 h:

```
d     = 2^-((t - t_last) / H)
W'    = W·d + 1
mean' = mean + (x - mean) / W'
S'    = S·d + (x - mean)(x - mean')      # West's incremental weighted variance
sd    = sqrt(S' / W')
```

W is then roughly "the number of recent packets": each one adds 1, and all of it halves every
48 hours. When the table is full, a new neighbor replaces the entry with the lowest weight aged to
now. Pooling two entries: both aged to now, W = W₁ + W₂, the weighted mean, and S = S₁ + S₂ +
W₁W₂/W · (mean₁ − mean₂)².

Ages are differences of the repeater's clock. Setting the clock after samples were taken shifts them:
a jump forward ages the whole table at once (set the clock before, or `linkstats clear` after).

## The request

An anonymous request (`PAYLOAD_TYPE_ANON_REQ`, the mechanism of the stock regions / owner / clock
requests): no login, no ACL slot. Its sub-type must be below `0x20` (`onAnonDataRecv` treats
`data[4] == 0 || data[4] >= ' '` as a login password); stock uses `0x01`–`0x03`.

**`0x1C` = `ANON_REQ_TYPE_LINK_STATS`** (provisional).

It is answered only when it arrived **direct**, and only while the stock `anon_limiter` allows it
(4 per 3 minutes, shared with the other anonymous requests). A flood request gets no answer, so no
one can make a repeater answer by flood.

Plaintext after MeshCore's 4-byte timestamp:

```
[0x1C][reply_path_len][reply_path…][version = 1][offset u8][count u8][prefix_len u8]
```

- `reply_path_len`, `reply_path`: as in the stock anonymous requests (path-length encoding: bits
  0-5 hop count, bits 6-7 hash size − 1). The answer goes back on it, direct.
- `offset`, `count`: the entries wanted, sorted by W (aged to now), highest first.
- `prefix_len`: key bytes per entry, 1 to 4.

## The answer

`PAYLOAD_TYPE_RESPONSE`, direct on the reply path, little-endian:

```
[tag u32][clock u32][version = 1][flags u8][noise floor int8 dBm][half-life h u8]
[uptime h u16][entries total u8][offset u8][count u8]          17 bytes
+ count × [key prefix_len bytes][kind u8][W×4 u16][mean×4 int8][sd×4 u8][rssi int8 dBm]
          [age min u16][n u16]                                  prefix_len + 10 bytes
```

- `tag`: the request's timestamp, as in the stock answers.
- `W` aged to the moment of the answer, × 4, saturating at 65535. `rssi` −128: none yet.
- `flags`: bit 0, link stats on (always 1 in an answer); bit 1, the table has been full.
- A response carries at most 167 bytes: 10 entries with 4-byte prefixes, 12 with 2-byte ones. A
  full table takes 3 or 4 requests (`offset`).

## Advert marker

With link stats on, the repeater's adverts carry feature 1 (`AdvertDataBuilder` feature field,
flag `0x20`, 2 of the 32 appdata bytes). Stock firmware parses and skips it. The PCES marker
family is `0xC5xx`:

| feature 1 | meaning |
|---|---|
| `0xC500` family | PCES |
| `f & 0x0001` | room server answers slash commands (Commsol rooms) |
| `f & 0x0002` | a PCES server |
| `f & 0x0004` | answers the link-stats request (this document) |
| `f & 0x0008` | answers the channel-load request (`load.md`) |

This firmware sets `0xC504` (link stats), `0xC508` (load) or `0xC50C` (both).

## CLI (admin)

| command | |
|---|---|
| `get pces.linkstats` / `set pces.linkstats on\|off` | default on. Off: no samples, no answers, no marker bit (from the next advert). Saved in `/pces_ls` |
| `linkstats [N]` | the table, highest weight first, from entry N. Over the air: a legend line (`N links: key SNR/sd dB, w=weight, age`), about 4 rows, then `more: linkstats N` if rows were left out. `key?`: not resolved yet |
| `linkstats clear` | empty the table |

Over USB serial, `linkstats` lists every entry with RSSI and sample count.

## Requester rules

What a client asking for this data should do (PCES in Commsol follows these):

- Only ask repeaters whose adverts carry the `0x0004` bit.
- Send **direct** only, over a known route; never flood the request (it would not be answered,
  and floods cost every repeater airtime).
- At most once a day per repeater for a full table, pages a minute apart; keep the total request
  rate low (Commsol: at most 6 an hour, shared with the load requests).
- Copies of one packet heard a few seconds apart are not independent samples: cap how much W counts.

## Open questions

- 32 entries: enough for a hilltop repeater?
- Should TRACE samples count for more than flood copies? (Same measurement, so for now the same.)
- Reboots empty the table; saving it would wear the flash, so not in v1.
