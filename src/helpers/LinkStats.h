#pragma once

// Per-neighbour receive statistics: how well this node hears each neighbour that transmits near
// it, from the packets it receives anyway (nothing sent). Exponentially aged weight, weighted SNR
// mean and spread (West's incremental weighted variance), weighted RSSI mean.
// Plain C++ (no Arduino), so it can be tested on a host. Spec: Commsol docs/pces-link-stats.md.

#include <stdint.h>
#include <stddef.h>

#ifndef MAX_LINK_STATS
  #define MAX_LINK_STATS  32
#endif

#define LINK_STATS_HALF_LIFE_SECS  (48UL * 3600UL)

#define LINK_KIND_WIDTH_MASK  0x03   // unresolved: hash width - 1
#define LINK_KIND_RESOLVED    0x04   // key is a public key prefix (else: the hash as heard)
#define LINK_KIND_ADVERT_ONLY 0x08   // only zero-hop adverts so far

struct LinkStatEntry {
  uint8_t  key[4];     // pub key prefix (resolved) or the heard hash, zero-padded
  uint8_t  kind;
  float    w;          // weight at t_last
  float    mean;       // SNR, dB
  float    s;          // weighted sum of squares about the mean
  float    wr;         // RSSI weight at t_last (adverts carry no RSSI here)
  float    rssi;       // dBm
  uint32_t t_last;     // RTC secs of the last sample (0: empty slot)
  uint16_t n;          // samples ever, saturating
};

class LinkStats {
  LinkStatEntry _e[MAX_LINK_STATS];
  bool _was_full;

  LinkStatEntry* find(const uint8_t* key, uint8_t len, uint8_t kind);
  LinkStatEntry* slotFor(uint32_t now);
public:
  LinkStats() { clear(); }
  void clear();

  // One received copy. key: pub key prefix (resolved, >= 4 bytes) or the hash heard (1..3 bytes).
  // rssi_valid false: SNR only (e.g. an advert processed after a receive delay).
  void sample(const uint8_t* key, uint8_t key_len, bool resolved, bool advert,
              float snr, float rssi, bool rssi_valid, uint32_t now);

  static float aged(float w, uint32_t t_last, uint32_t now);   // w * 2^-((now - t_last) / half-life)
  float sd(const LinkStatEntry& e) const;
  int  count() const;
  bool wasFull() const { return _was_full; }

  // Occupied entries, highest weight (aged to now) first. -> number written to out.
  int sorted(const LinkStatEntry** out, int max, uint32_t now) const;

  // Wire entries (docs/pces-link-stats.md): [key prefix_len][kind][W u16 x4][mean int8 x4]
  // [sd u8 x4][rssi int8][age min u16][n u16], from sorted index `offset`, at most `count`, while
  // they fit in dest_max bytes. -> bytes written; *n_out = entries written.
  int encode(uint8_t* dest, int dest_max, uint8_t offset, uint8_t count, uint8_t prefix_len,
             uint32_t now, uint8_t* n_out) const;
  static int entrySize(uint8_t prefix_len) { return prefix_len + 10; }
};
