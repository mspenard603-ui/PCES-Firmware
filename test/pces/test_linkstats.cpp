// Host test for MeshCore src/helpers/LinkStats (the PCES link-stats firmware patch).
//   g++ -std=c++17 -I src/helpers -o /tmp/t test/pces/test_linkstats.cpp src/helpers/LinkStats.cpp && /tmp/t
#include "LinkStats.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

static const uint32_t H = LINK_STATS_HALF_LIFE_SECS;

int main() {
  LinkStats ls;
  const uint8_t ka[4] = {0xF7, 0xC3, 0x11, 0x22}, kb[4] = {0x60, 0x79, 0x33, 0x44};
  uint32_t t = 1800000000;

  // same-time samples: plain mean and population sd
  float xs[] = {10, 12, 8, 10};
  for (float x : xs) ls.sample(ka, 4, true, false, x, -60, true, t);
  const LinkStatEntry* l[MAX_LINK_STATS];
  CHECK(ls.sorted(l, MAX_LINK_STATS, t) == 1);
  NEAR(l[0]->w, 4.0, 1e-4);
  NEAR(l[0]->mean, 10.0, 1e-4);
  NEAR(ls.sd(*l[0]), sqrt(2.0), 1e-3);        // var = (0+4+4+0)/4
  NEAR(l[0]->rssi, -60.0, 1e-4);
  CHECK(l[0]->n == 4 && (l[0]->kind & LINK_KIND_RESOLVED));

  // ageing: one half-life later the weight is halved; a new sample counts for 1 against 2
  ls.sample(ka, 4, true, false, 13, -70, true, t + H);
  ls.sorted(l, MAX_LINK_STATS, t + H);
  NEAR(l[0]->w, 3.0, 1e-3);
  NEAR(l[0]->mean, 10.0 + 3.0 / 3.0, 1e-3);   // mean moves by (13-10)/W
  NEAR(LinkStats::aged(l[0]->w, l[0]->t_last, t + 2 * H), 1.5, 1e-3);

  // unresolved 1-byte hash and resolved key are separate entries; ordering by aged weight
  for (int i = 0; i < 10; i++) ls.sample(kb, 1, false, false, -3, -110, true, t + H);
  CHECK(ls.count() == 2);
  int n = ls.sorted(l, MAX_LINK_STATS, t + H);
  CHECK(n == 2 && !(l[0]->kind & LINK_KIND_RESOLVED) && (l[0]->kind & LINK_KIND_WIDTH_MASK) == 0);

  // advert samples: SNR only, flag kept until a relayed sample arrives
  LinkStats la;
  la.sample(kb, 4, true, true, 5, 0, false, t);
  la.sorted(l, MAX_LINK_STATS, t);
  CHECK((l[0]->kind & LINK_KIND_ADVERT_ONLY) && l[0]->wr == 0);
  la.sample(kb, 4, true, false, 7, -90, true, t);
  la.sorted(l, MAX_LINK_STATS, t);
  CHECK(!(l[0]->kind & LINK_KIND_ADVERT_ONLY) && l[0]->n == 2);

  // full table: the weakest (aged) entry is replaced, wasFull set
  LinkStats lf;
  for (int i = 0; i < MAX_LINK_STATS; i++) {
    uint8_t k[4] = {(uint8_t)i, 1, 2, 3};
    for (int j = 0; j <= i; j++) lf.sample(k, 4, true, false, 1, -80, true, t);
  }
  CHECK(lf.count() == MAX_LINK_STATS && !lf.wasFull());
  uint8_t knew[4] = {0xEE, 1, 2, 3};
  lf.sample(knew, 4, true, false, 2, -80, true, t);
  CHECK(lf.wasFull() && lf.count() == MAX_LINK_STATS);
  n = lf.sorted(l, MAX_LINK_STATS, t);
  bool has0 = false, hasNew = false;
  for (int i = 0; i < n; i++) { has0 |= l[i]->key[0] == 0; hasNew |= l[i]->key[0] == 0xEE; }
  CHECK(!has0 && hasNew);   // key 0 had weight 1, the lowest

  // wire encoding: 2-byte prefixes -> 12 bytes each, fits the budget, paging by offset
  uint8_t buf[150];
  uint8_t wrote = 0;
  int len = lf.encode(buf, 150, 0, 255, 2, t, &wrote);
  CHECK(wrote == 150 / 12 && len == wrote * 12);
  CHECK(buf[0] == MAX_LINK_STATS - 1 && buf[1] == 1);   // top entry: highest weight first
  uint16_t w4; memcpy(&w4, &buf[3], 2);
  CHECK(w4 == (MAX_LINK_STATS) * 4);                      // W = 32 samples x 4
  CHECK((int8_t)buf[5] == 4 && buf[6] == 0);               // mean +1 dB x4, sd 0
  CHECK((int8_t)buf[7] == -80);                            // RSSI
  len = lf.encode(buf, 150, 30, 255, 4, t, &wrote);
  CHECK(wrote == 2 && len == 2 * 14);

  // one neighbor heard under several keys (1-byte and 2-byte path hashes, then resolved): one entry
  {
    LinkStats lm;
    const uint8_t f7[4] = {0xF7, 0xC3, 0xA1, 0xB2};
    for (int i = 0; i < 4; i++) lm.sample(f7, 1, false, false, 12, -94, true, t);   // F7?
    for (int i = 0; i < 4; i++) lm.sample(f7, 2, false, false, 10, -96, true, t);   // F7C3?: widens F7?
    CHECK(lm.count() == 1);
    lm.sorted(l, MAX_LINK_STATS, t);
    CHECK((l[0]->kind & LINK_KIND_WIDTH_MASK) == 1 && l[0]->n == 8);
    NEAR(l[0]->w, 8.0, 1e-4);
    NEAR(l[0]->mean, 11.0, 1e-4);
    NEAR(lm.sd(*l[0]), 1.0, 1e-3);
    lm.sample(f7, 1, false, false, 11, -95, true, t);   // F7? again: counted on F7C3?
    CHECK(lm.count() == 1);
    lm.sample(f7, 4, true, false, 11, -95, true, t);    // resolved: the entry becomes F7C3A1B2
    CHECK(lm.count() == 1);
    lm.sorted(l, MAX_LINK_STATS, t);
    CHECK((l[0]->kind & LINK_KIND_RESOLVED) && l[0]->n == 10 && memcmp(l[0]->key, f7, 4) == 0);

    // the old order (F7C3? first, then F7?, then resolved; separate entries from before the fix)
    LinkStats lo;
    lo.sample(f7, 2, false, false, 12, -94, true, t);
    lo.sample(f7, 1, false, false, 10, -94, true, t);
    CHECK(lo.count() == 1);
    lo.sample(f7, 4, true, true, 11, 0, false, t);
    CHECK(lo.count() == 1);

    // a short hash shared by two neighbors stays apart, and isn't folded into either
    LinkStats ls2;
    const uint8_t f712[4] = {0xF7, 0x12, 0, 0};
    ls2.sample(f7, 2, false, false, 10, -90, true, t);
    ls2.sample(f712, 2, false, false, -5, -110, true, t);
    ls2.sample(f7, 1, false, false, 3, -100, true, t);   // F7?: could be either
    CHECK(ls2.count() == 3);

    // a resolved entry folds in an older unresolved one heard first (aged pool)
    LinkStats lr;
    lr.sample(f7, 2, false, false, 8, -90, true, t);
    lr.sample(f7, 4, true, false, 12, -90, true, t + H);
    CHECK(lr.count() == 1);
    lr.sorted(l, MAX_LINK_STATS, t + H);
    NEAR(l[0]->w, 1.5, 1e-3);                       // 0.5 (aged) + 1
    NEAR(l[0]->mean, (0.5 * 8 + 12) / 1.5, 1e-3);
  }

  // the CLI's own ages: an RTC that is not set (0) records nothing
  LinkStats lz;
  lz.sample(ka, 4, true, false, 1, -1, true, 0);
  CHECK(lz.count() == 0);

  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails ? 1 : 0;
}
