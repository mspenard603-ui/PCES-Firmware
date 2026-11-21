// Host test for src/helpers/LoadStats (PCES channel load, docs/pces/load.md).
//   g++ -std=c++17 -I src/helpers -o /tmp/tl test/pces/test_loadstats.cpp src/helpers/LoadStats.cpp && /tmp/tl
#include "LoadStats.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint16_t get16(const uint8_t* d) { return (uint16_t)(d[0] | (d[1] << 8)); }

int main() {
  LoadStats ls;
  LoadCounters c;
  memset(&c, 0, sizeof(c));
  c.tx_ms = 123456; c.n_rx = 999;   // counters ran since boot: the first tick is a baseline only
  ls.tick(1, c, 0, false);
  LoadBucket b;
  CHECK(!ls.window(1, &b));
  CHECK(!ls.day(&b));

  // one minute: 6 s TX (10 %), 12 s RX (20 %), queue 2 for 30 s, held 15 s, busy 1 in 4
  for (int s = 0; s < 60; s++) {
    c.tx_ms += 100; c.rx_ms += 200; c.n_rx += 1;
    if (s % 2 == 0) { c.n_flood += 1; }
    if (s % 6 == 0) { c.n_dup += 1; c.n_tx += 1; }
    for (int k = 0; k < 4; k++) ls.busySample(k == 0);
    ls.tick(1, c, s < 30 ? 2 : 0, s < 15);
  }
  CHECK(ls.minutesKept() == 1);
  CHECK(ls.window(15, &b));    // only one complete minute exists
  CHECK(b.secs == 60);
  CHECK(LoadStats::txPermille(b) == 100);
  CHECK(LoadStats::rxPermille(b) == 200);
  CHECK(LoadStats::busyPermille(b) == 250);
  CHECK(LoadStats::heldPermille(b) == 250);
  CHECK(b.n_rx == 60 && b.n_flood == 30 && b.n_dup == 10 && b.n_tx == 10);
  CHECK(b.q_max == 2);

  uint8_t w[LOAD_WINDOW_WIRE_SIZE];
  CHECK(LoadStats::encodeWindow(w, b, true) == 22);
  CHECK(get16(&w[0]) == 1);
  CHECK(get16(&w[2]) == 100 && get16(&w[4]) == 200 && get16(&w[6]) == 250 && get16(&w[8]) == 250);
  CHECK(get16(&w[10]) == 60 && get16(&w[12]) == 10 && get16(&w[14]) == 30 && get16(&w[16]) == 10);
  CHECK(w[20] == 10 && w[21] == 2);   // queue avg 2 x 30/60 = 1.0 -> 10

  LoadBucket e;
  memset(&e, 0, sizeof(e));
  CHECK(LoadStats::encodeWindow(w, e, false) == 22);
  CHECK(get16(&w[6]) == 0xFFFF && get16(&w[0]) == 0);

  // counter wrap: differences stay right
  c.tx_ms = 0xFFFFFF00u;
  ls.tick(1, c, 0, false);
  c.tx_ms += 0x200;   // wraps
  ls.tick(1, c, 0, false);
  ls.window(1, &b);   // still the first minute: the open one isn't reported
  CHECK(b.tx_ms == 6000);

  // two quiet hours then one busy minute: day, peak, 60 min ring
  ls.clear();
  memset(&c, 0, sizeof(c));
  ls.tick(1, c, 0, false);
  for (int m = 0; m < 120; m++) {
    for (int s = 0; s < 60; s++) {
      if (m == 30) { c.tx_ms += 500; ls.busySample(true); }   // hour 0 has one busy minute
      else ls.busySample(false);
      ls.tick(1, c, 0, false);
    }
  }
  CHECK(ls.hoursKept() == 2);
  CHECK(ls.minutesKept() == 60);
  CHECK(ls.day(&b) && b.secs == 7200);
  uint8_t ago = 99;
  CHECK(ls.peak(&b, &ago));
  CHECK(ago == 1);   // the older of the two complete hours
  CHECK(LoadStats::busyPermille(b) == 17);   // 60 of 3600
  CHECK(ls.window(60, &b) && b.secs == 3600 && b.tx_ms == 0);

  // 26 h: the day keeps 24 complete hours, the ring 60 complete minutes
  ls.clear();
  ls.tick(1, c, 0, false);
  for (int m = 0; m < 26 * 60; m++) for (int s = 0; s < 60; s++) { ls.busySample(false); ls.tick(1, c, 0, false); }
  CHECK(ls.hoursKept() == 24 && ls.day(&b) && b.secs == 24 * 3600);
  CHECK(ls.window(60, &b) && b.secs == 3600);
  CHECK(ls.window(99, &b) && b.secs == 3600);

  // a long elapsed gap (loop stalled) still closes the minute
  ls.clear();
  ls.tick(1, c, 0, false);
  ls.tick(75, c, 0, false);
  CHECK(ls.minutesKept() == 1);

  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails ? 1 : 0;
}
