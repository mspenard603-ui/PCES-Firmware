#include "LoadStats.h"
#include <string.h>

void LoadStats::clear() {
  memset(_min, 0, sizeof(_min));
  memset(_hour, 0, sizeof(_hour));
  _mi = _hi = 0;
  _n_min = _n_hour = 0;
  _min_in_hour = 0;
  memset(&_last, 0, sizeof(_last));
  _have_last = false;
}

void LoadStats::add(LoadBucket& a, const LoadBucket& b) {
  a.secs += b.secs;
  a.tx_ms += b.tx_ms;
  a.rx_ms += b.rx_ms;
  a.busy += b.busy;
  a.samples += b.samples;
  a.n_rx += b.n_rx;
  a.n_tx += b.n_tx;
  a.n_flood += b.n_flood;
  a.n_dup += b.n_dup;
  a.n_err += b.n_err;
  a.q_sum += b.q_sum;
  a.held_s += b.held_s;
  if (b.q_max > a.q_max) a.q_max = b.q_max;
}

void LoadStats::busySample(bool busy) {
  LoadBucket& m = _min[_mi];
  m.samples++;
  if (busy) m.busy++;
}

void LoadStats::tick(uint32_t elapsed_s, const LoadCounters& c, uint8_t queue, bool held) {
  if (!_have_last) {   // first call: a baseline only (the counters ran since boot)
    _last = c;
    _have_last = true;
    return;
  }
  if (elapsed_s == 0) return;
  LoadBucket& m = _min[_mi];
  m.secs += elapsed_s;
  m.tx_ms += c.tx_ms - _last.tx_ms;
  m.rx_ms += c.rx_ms - _last.rx_ms;
  m.n_rx += c.n_rx - _last.n_rx;
  m.n_tx += c.n_tx - _last.n_tx;
  m.n_flood += c.n_flood - _last.n_flood;
  m.n_dup += c.n_dup - _last.n_dup;
  m.n_err += c.n_err - _last.n_err;
  m.q_sum += (uint32_t)queue * elapsed_s;
  if (queue > m.q_max) m.q_max = queue;
  if (held) m.held_s += elapsed_s;
  _last = c;

  if (m.secs < 60) return;
  // the minute is complete: into the open hour, then a new minute
  add(_hour[_hi], m);
  if (_n_min < LOAD_MINUTES) _n_min++;
  _mi = (_mi + 1) % (LOAD_MINUTES + 1);
  memset(&_min[_mi], 0, sizeof(LoadBucket));
  if (++_min_in_hour >= 60) {
    _min_in_hour = 0;
    if (_n_hour < LOAD_HOURS) _n_hour++;
    _hi = (_hi + 1) % (LOAD_HOURS + 1);
    memset(&_hour[_hi], 0, sizeof(LoadBucket));
  }
}

bool LoadStats::window(int minutes, LoadBucket* out) const {
  memset(out, 0, sizeof(LoadBucket));
  if (minutes > _n_min) minutes = _n_min;
  if (minutes <= 0) return false;
  for (int k = 1; k <= minutes; k++) {
    add(*out, _min[(_mi + LOAD_MINUTES + 1 - k) % (LOAD_MINUTES + 1)]);
  }
  return true;
}

bool LoadStats::day(LoadBucket* out) const {
  memset(out, 0, sizeof(LoadBucket));
  if (_n_hour == 0) return false;
  for (int k = 1; k <= _n_hour; k++) {
    add(*out, _hour[(_hi + LOAD_HOURS + 1 - k) % (LOAD_HOURS + 1)]);
  }
  return true;
}

bool LoadStats::peak(LoadBucket* out, uint8_t* ago_h) const {
  memset(out, 0, sizeof(LoadBucket));
  int best = -1;
  uint16_t best_b = 0;
  for (int k = 1; k <= _n_hour; k++) {
    const LoadBucket& h = _hour[(_hi + LOAD_HOURS + 1 - k) % (LOAD_HOURS + 1)];
    if (h.samples == 0) continue;
    uint16_t b = busyPermille(h);
    if (best < 0 || b > best_b) { best = k; best_b = b; }
  }
  if (best < 0) return false;
  *out = _hour[(_hi + LOAD_HOURS + 1 - best) % (LOAD_HOURS + 1)];
  *ago_h = (uint8_t)(best - 1);
  return true;
}

static uint16_t permille(uint64_t num, uint64_t den) {
  if (den == 0) return 0;
  uint64_t v = (num * 1000 + den / 2) / den;
  return v > 1000 ? 1000 : (uint16_t)v;
}

uint16_t LoadStats::txPermille(const LoadBucket& b) { return permille(b.tx_ms, (uint64_t)b.secs * 1000); }
uint16_t LoadStats::rxPermille(const LoadBucket& b) { return permille(b.rx_ms, (uint64_t)b.secs * 1000); }
uint16_t LoadStats::busyPermille(const LoadBucket& b) { return b.samples ? permille(b.busy, b.samples) : 0xFFFF; }
uint16_t LoadStats::heldPermille(const LoadBucket& b) { return permille(b.held_s, b.secs); }

static void put16(uint8_t* d, uint32_t v) {
  uint16_t x = v > 0xFFFF ? 0xFFFF : (uint16_t)v;
  d[0] = x & 0xFF;
  d[1] = x >> 8;
}

int LoadStats::encodeWindow(uint8_t* dest, const LoadBucket& b, bool ok) {
  memset(dest, 0, LOAD_WINDOW_WIRE_SIZE);
  if (!ok || b.secs == 0) {
    put16(&dest[6], 0xFFFF);
    return LOAD_WINDOW_WIRE_SIZE;
  }
  put16(&dest[0], b.secs / 60);
  put16(&dest[2], txPermille(b));
  put16(&dest[4], rxPermille(b));
  put16(&dest[6], busyPermille(b));
  put16(&dest[8], heldPermille(b));
  put16(&dest[10], b.n_rx);
  put16(&dest[12], b.n_tx);
  put16(&dest[14], b.n_flood);
  put16(&dest[16], b.n_dup);
  put16(&dest[18], b.n_err);
  uint64_t qa = ((uint64_t)b.q_sum * 10 + b.secs / 2) / b.secs;
  dest[20] = qa > 255 ? 255 : (uint8_t)qa;
  dest[21] = b.q_max;
  return LOAD_WINDOW_WIRE_SIZE;
}
