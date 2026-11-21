#pragma once

// Channel load seen by this node: its own TX airtime, the airtime of packets it decoded, how often
// the channel is busy (RSSI above the noise floor), packet counts and its TX queue. Kept per minute
// for the last hour and per hour for the last day, from counters the firmware keeps anyway
// (nothing is sent).
//
// Plain C++ without Arduino, so it can be tested on a host (test/pces/test_loadstats.cpp).
// Spec: docs/pces/load.md.

#include <stdint.h>
#include <stddef.h>

#define LOAD_MINUTES   60
#define LOAD_HOURS     24
#define LOAD_WINDOW_WIRE_SIZE  22

// Cumulative counters, as the firmware keeps them (they may wrap: only differences are used).
struct LoadCounters {
  uint32_t tx_ms;      // own transmit airtime
  uint32_t rx_ms;      // airtime of decoded packets
  uint32_t n_rx;       // packets decoded
  uint32_t n_tx;       // packets sent
  uint32_t n_flood;    // flood packets received
  uint32_t n_dup;      // flood copies already seen
  uint32_t n_err;      // receive (CRC) errors
};

struct LoadBucket {
  uint32_t secs;       // seconds covered
  uint32_t tx_ms, rx_ms;
  uint32_t busy, samples;   // busy RSSI samples, of all samples taken
  uint32_t n_rx, n_tx, n_flood, n_dup, n_err;
  uint32_t q_sum;      // sum of the per-second TX queue length
  uint32_t held_s;     // seconds with packets waiting while the airtime budget held them back
  uint8_t  q_max;
};

class LoadStats {
  // Ring buffers: the complete buckets plus the open one being filled.
  LoadBucket _min[LOAD_MINUTES + 1];
  LoadBucket _hour[LOAD_HOURS + 1];
  uint8_t _mi, _hi;            // index of the open minute / hour
  uint8_t _n_min, _n_hour;     // number of complete minutes / hours kept
  uint8_t _min_in_hour;        // complete minutes added to the open hour so far
  LoadCounters _last;          // counters at the previous tick
  bool _have_last;

  static void add(LoadBucket& total, const LoadBucket& part);
  const LoadBucket& completeMinute(int k) const;   // k = 1: the most recent complete minute
  const LoadBucket& completeHour(int k) const;     // k = 1: the most recent complete hour
public:
  LoadStats() { clear(); }
  void clear();

  void busySample(bool busy);  // one RSSI reading while receiving: above the busy threshold or not

  // Called about once a second: elapsed_s seconds since the last call, the cumulative counters,
  // the TX queue length now, and whether queued packets are being held by the airtime budget.
  void tick(uint32_t elapsed_s, const LoadCounters& c, uint8_t queue, bool held);

  // The last `minutes` complete minutes (1..60). false: none yet.
  bool window(int minutes, LoadBucket* out) const;
  // The last 24 complete hours. false: none yet.
  bool day(LoadBucket* out) const;
  // The complete hour of the last 24 with the highest busy share; *ago_h 0 = the last complete hour.
  bool peak(LoadBucket* out, uint8_t* ago_h) const;
  int minutesKept() const { return _n_min; }
  int hoursKept() const { return _n_hour; }

  // Shares in permille (busy: 0xFFFF when no sample was taken).
  static uint16_t txPermille(const LoadBucket& b);
  static uint16_t rxPermille(const LoadBucket& b);
  static uint16_t busyPermille(const LoadBucket& b);
  static uint16_t heldPermille(const LoadBucket& b);

  // Wire form (docs/pces/load.md), 22 bytes little-endian: [covered min u16][tx u16][rx u16]
  // [busy u16][held u16] (permille) [rx pkts u16][tx pkts u16][flood rx u16][flood dups u16]
  // [rx errors u16] (counts in the window, saturating) [queue avg x10 u8][queue max u8].
  // An empty window (ok false) is all zero with busy 0xFFFF.
  static int encodeWindow(uint8_t* dest, const LoadBucket& b, bool ok);
};
