#pragma once

// Receive statistics per neighbor: how well this node hears each neighbor that transmits near it,
// taken from the packets it receives anyway (nothing is sent). For each neighbor it keeps an
// exponentially aged sample weight, the weighted mean and spread of the SNR (West's incremental
// weighted variance) and the weighted mean RSSI.
//
// Plain C++ without Arduino, so it can be tested on a host (test/pces/test_linkstats.cpp).
// Spec: docs/pces/link-stats.md.

#include <stdint.h>
#include <stddef.h>

#ifndef MAX_LINK_STATS
  #define MAX_LINK_STATS  32
#endif

#define LINK_STATS_HALF_LIFE_SECS  (48UL * 3600UL)

// Bits of LinkStatEntry::kind (also sent on the wire)
#define LINK_KIND_WIDTH_MASK   0x03   // unresolved entries: hash width - 1
#define LINK_KIND_RESOLVED     0x04   // the key is a public key prefix, not a hash as heard
#define LINK_KIND_ADVERT_ONLY  0x08   // only zero-hop adverts so far, no relayed packets

#define LINK_KEY_SIZE          4      // bytes of public key kept for a resolved neighbor
#define LINK_MAX_HASH_SIZE     3      // longest path hash MeshCore uses

struct LinkStatEntry {
  uint8_t  key[LINK_KEY_SIZE];   // public key prefix, or the hash as heard (zero padded)
  uint8_t  kind;                 // LINK_KIND_* bits
  float    w;                    // sample weight at t_last
  float    mean;                 // SNR in dB
  float    s;                    // weighted sum of squares about the mean
  float    wr;                   // RSSI weight at t_last (adverts add no RSSI)
  float    rssi;                 // dBm
  uint32_t t_last;               // clock time of the last sample; 0 marks an empty slot
  uint16_t n;                    // samples ever taken, saturating
};

class LinkStats {
  LinkStatEntry _e[MAX_LINK_STATS];
  bool _was_full;

  void addSample(const uint8_t* key, uint8_t key_len, bool resolved, bool from_advert,
                 float snr, bool has_rssi, float rssi, uint32_t now);
  LinkStatEntry* findEntryFor(const uint8_t* key, uint8_t len, bool resolved);
  void mergeShorterEntriesInto(LinkStatEntry* e, uint32_t now);
  LinkStatEntry* freeSlot(uint32_t now);

public:
  LinkStats() { clear(); }
  void clear();

  // A relayed packet heard from a neighbor. `key` is the neighbor's public key prefix when
  // `resolved` (4 bytes used), otherwise the path hash as heard (1 to 3 bytes).
  void addPacket(const uint8_t* key, uint8_t key_len, bool resolved, float snr, float rssi, uint32_t now);

  // A zero-hop advert from a neighbor (repeater or room server). SNR only: the advert may be
  // handled after a receive delay, when the radio's last RSSI belongs to another packet.
  void addAdvert(const uint8_t* pub_key, float snr, uint32_t now);

  // The weight w, last updated at t_last, aged to `now`: w * 2^-((now - t_last) / half-life).
  static float aged(float w, uint32_t t_last, uint32_t now);

  // Number of key bytes an entry holds: 4 for a resolved neighbor, else its hash width.
  static int keyLength(const LinkStatEntry& e);

  float sd(const LinkStatEntry& e) const;
  int count() const;
  bool wasFull() const { return _was_full; }

  // Fills `out` with the used entries, highest weight (aged to now) first. Returns how many.
  int sorted(const LinkStatEntry** out, int max, uint32_t now) const;

  // Writes entries in wire form (docs/pces/link-stats.md), starting at sorted index `offset`, at
  // most `count` of them, while they fit in `dest_max` bytes. Each entry:
  //   [key, prefix_len bytes][kind][W x4, u16][mean SNR x4, int8][sd x4, u8][rssi, int8]
  //   [age in minutes, u16][n, u16]
  // Returns the bytes written; *n_out is set to the number of entries written.
  int encode(uint8_t* dest, int dest_max, uint8_t offset, uint8_t count, uint8_t prefix_len,
             uint32_t now, uint8_t* n_out) const;
  static int entrySize(uint8_t prefix_len) { return prefix_len + 10; }
};
