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

void LoadStats::add(LoadBucket& total, const LoadBucket& part) {
  total.secs += part.secs;
  total.tx_ms += part.tx_ms;
  total.rx_ms += part.rx_ms;
  total.busy += part.busy;
  total.samples += part.samples;
  total.n_rx += part.n_rx;
  total.n_tx += part.n_tx;
  total.n_flood += part.n_flood;
  total.n_dup += part.n_dup;
  total.n_err += part.n_err;
  total.q_sum += part.q_sum;
  total.held_s += part.held_s;
  if (part.q_max > total.q_max) {
    total.q_max = part.q_max;
  }
}

const LoadBucket& LoadStats::completeMinute(int k) const {
  int slots = LOAD_MINUTES + 1;
  return _min[(_mi + slots - k) % slots];
}

const LoadBucket& LoadStats::completeHour(int k) const {
  int slots = LOAD_HOURS + 1;
  return _hour[(_hi + slots - k) % slots];
}

void LoadStats::busySample(bool busy) {
  LoadBucket& minute = _min[_mi];
  minute.samples++;
  if (busy) {
    minute.busy++;
  }
}

void LoadStats::tick(uint32_t elapsed_s, const LoadCounters& c, uint8_t queue, bool held) {
  if (!_have_last) {
    // The counters have run since boot: the first call only records them as the baseline.
    _last = c;
    _have_last = true;
    return;
  }
  if (elapsed_s == 0) {
    return;
  }

  // Counter differences are taken unsigned, so they stay right when a counter wraps.
  LoadBucket& minute = _min[_mi];
  minute.secs += elapsed_s;
  minute.tx_ms += c.tx_ms - _last.tx_ms;
  minute.rx_ms += c.rx_ms - _last.rx_ms;
  minute.n_rx += c.n_rx - _last.n_rx;
  minute.n_tx += c.n_tx - _last.n_tx;
  minute.n_flood += c.n_flood - _last.n_flood;
  minute.n_dup += c.n_dup - _last.n_dup;
  minute.n_err += c.n_err - _last.n_err;
  minute.q_sum += (uint32_t)queue * elapsed_s;
  if (queue > minute.q_max) {
    minute.q_max = queue;
  }
  if (held) {
    minute.held_s += elapsed_s;
  }
  _last = c;

  if (minute.secs < 60) {
    return;
  }

  // The minute is complete: add it to the open hour and start a new minute.
  add(_hour[_hi], minute);
  if (_n_min < LOAD_MINUTES) {
    _n_min++;
  }
  _mi = (_mi + 1) % (LOAD_MINUTES + 1);
  memset(&_min[_mi], 0, sizeof(LoadBucket));

  _min_in_hour++;
  if (_min_in_hour >= 60) {
    // The hour is complete as well: start a new hour.
    _min_in_hour = 0;
    if (_n_hour < LOAD_HOURS) {
      _n_hour++;
    }
    _hi = (_hi + 1) % (LOAD_HOURS + 1);
    memset(&_hour[_hi], 0, sizeof(LoadBucket));
  }
}

bool LoadStats::window(int minutes, LoadBucket* out) const {
  memset(out, 0, sizeof(LoadBucket));
  if (minutes > _n_min) {
    minutes = _n_min;
  }
  if (minutes <= 0) {
    return false;
  }
  for (int k = 1; k <= minutes; k++) {
    add(*out, completeMinute(k));
  }
  return true;
}

bool LoadStats::day(LoadBucket* out) const {
  memset(out, 0, sizeof(LoadBucket));
  if (_n_hour == 0) {
    return false;
  }
  for (int k = 1; k <= _n_hour; k++) {
    add(*out, completeHour(k));
  }
  return true;
}

bool LoadStats::peak(LoadBucket* out, uint8_t* ago_h) const {
  memset(out, 0, sizeof(LoadBucket));
  int best_k = -1;
  uint16_t best_busy = 0;
  for (int k = 1; k <= _n_hour; k++) {
    const LoadBucket& hour = completeHour(k);
    if (hour.samples == 0) {
      continue;   // busy was not sampled in this hour
    }
    uint16_t busy = busyPermille(hour);
    if (best_k < 0 || busy > best_busy) {
      best_k = k;
      best_busy = busy;
    }
  }
  if (best_k < 0) {
    return false;
  }
  *out = completeHour(best_k);
  *ago_h = (uint8_t)(best_k - 1);
  return true;
}

// num / den in permille, rounded, at most 1000.
static uint16_t permille(uint64_t num, uint64_t den) {
  if (den == 0) {
    return 0;
  }
  uint64_t v = (num * 1000 + den / 2) / den;
  if (v > 1000) {
    return 1000;
  }
  return (uint16_t)v;
}

uint16_t LoadStats::txPermille(const LoadBucket& b) { return permille(b.tx_ms, (uint64_t)b.secs * 1000); }
uint16_t LoadStats::rxPermille(const LoadBucket& b) { return permille(b.rx_ms, (uint64_t)b.secs * 1000); }
uint16_t LoadStats::busyPermille(const LoadBucket& b) {
  if (b.samples == 0) {
    return 0xFFFF;   // not sampled
  }
  return permille(b.busy, b.samples);
}
uint16_t LoadStats::heldPermille(const LoadBucket& b) { return permille(b.held_s, b.secs); }

// Little-endian u16, saturating at 65535.
static void put16(uint8_t* dest, uint32_t v) {
  uint16_t x = v > 0xFFFF ? 0xFFFF : (uint16_t)v;
  dest[0] = x & 0xFF;
  dest[1] = x >> 8;
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
  uint64_t queue_avg_x10 = ((uint64_t)b.q_sum * 10 + b.secs / 2) / b.secs;
  dest[20] = queue_avg_x10 > 255 ? 255 : (uint8_t)queue_avg_x10;
  dest[21] = b.q_max;
  return LOAD_WINDOW_WIRE_SIZE;
}
