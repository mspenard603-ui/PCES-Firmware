#include "LinkStats.h"
#include <math.h>
#include <string.h>

void LinkStats::clear() {
  memset(_e, 0, sizeof(_e));
  _was_full = false;
}

float LinkStats::aged(float w, uint32_t t_last, uint32_t now) {
  if (now <= t_last) return w;
  return w * exp2f(-(float)(now - t_last) / (float)LINK_STATS_HALF_LIFE_SECS);
}

float LinkStats::sd(const LinkStatEntry& e) const {
  if (e.w <= 0.0f) return 0.0f;
  float v = e.s / e.w;
  return v > 0.0f ? sqrtf(v) : 0.0f;
}

int LinkStats::count() const {
  int n = 0;
  for (int i = 0; i < MAX_LINK_STATS; i++) if (_e[i].t_last) n++;
  return n;
}

LinkStatEntry* LinkStats::find(const uint8_t* key, uint8_t len, uint8_t kind) {
  for (int i = 0; i < MAX_LINK_STATS; i++) {
    LinkStatEntry* e = &_e[i];
    if (!e->t_last) continue;
    if ((e->kind & (LINK_KIND_RESOLVED | LINK_KIND_WIDTH_MASK)) != (kind & (LINK_KIND_RESOLVED | LINK_KIND_WIDTH_MASK))) continue;
    if (memcmp(e->key, key, len) == 0) return e;
  }
  return NULL;
}

LinkStatEntry* LinkStats::slotFor(uint32_t now) {
  LinkStatEntry* low = NULL;
  float low_w = 0;
  for (int i = 0; i < MAX_LINK_STATS; i++) {
    LinkStatEntry* e = &_e[i];
    if (!e->t_last) return e;
    float w = aged(e->w, e->t_last, now);
    if (low == NULL || w < low_w) { low = e; low_w = w; }
  }
  _was_full = true;
  return low;   // replace the weakest (aged to now)
}

void LinkStats::sample(const uint8_t* key, uint8_t key_len, bool resolved, bool advert,
                       float snr, float rssi, bool rssi_valid, uint32_t now) {
  if (now == 0 || key_len == 0) return;   // RTC not set: no ages to keep
  uint8_t k[4] = {0, 0, 0, 0};
  uint8_t kind;
  uint8_t len;
  if (resolved) {
    len = key_len < 4 ? key_len : 4;
    kind = LINK_KIND_RESOLVED;
  } else {
    len = key_len > 3 ? 3 : key_len;
    kind = (uint8_t)(len - 1);
  }
  memcpy(k, key, len);

  LinkStatEntry* e = find(k, resolved ? 4 : len, kind);
  if (e == NULL) {
    e = slotFor(now);
    memset(e, 0, sizeof(*e));
    memcpy(e->key, k, 4);
    e->kind = kind | (advert ? LINK_KIND_ADVERT_ONLY : 0);
  }
  if (!advert) e->kind &= ~LINK_KIND_ADVERT_ONLY;

  uint32_t t0 = e->t_last ? e->t_last : now;
  float d = e->t_last ? exp2f(-(float)(now > t0 ? now - t0 : 0) / (float)LINK_STATS_HALF_LIFE_SECS) : 0.0f;
  float w = e->w * d + 1.0f;
  float delta = snr - e->mean;
  float mean = e->mean + delta / w;
  e->s = e->s * d + delta * (snr - mean);
  e->mean = mean;
  e->w = w;
  if (rssi_valid) {
    float wr = e->wr * d + 1.0f;
    e->rssi += (rssi - e->rssi) / wr;
    e->wr = wr;
  } else {
    e->wr *= d;
  }
  e->t_last = now;
  if (e->n < 0xFFFF) e->n++;
}

int LinkStats::sorted(const LinkStatEntry** out, int max, uint32_t now) const {
  int n = 0;
  float ws[MAX_LINK_STATS];
  for (int i = 0; i < MAX_LINK_STATS && n < max; i++) {
    if (!_e[i].t_last) continue;
    float w = aged(_e[i].w, _e[i].t_last, now);
    int j = n++;
    while (j > 0 && ws[j - 1] < w) { ws[j] = ws[j - 1]; out[j] = out[j - 1]; j--; }   // insertion sort, highest first
    ws[j] = w; out[j] = &_e[i];
  }
  return n;
}

static int8_t clamp8(float v) {
  if (v != v) return -128;   // NaN
  long r = lroundf(v);
  return (int8_t)(r < -127 ? -127 : r > 127 ? 127 : r);
}

int LinkStats::encode(uint8_t* dest, int dest_max, uint8_t offset, uint8_t count, uint8_t prefix_len,
                      uint32_t now, uint8_t* n_out) const {
  if (prefix_len < 1) prefix_len = 1;
  if (prefix_len > 4) prefix_len = 4;
  const LinkStatEntry* list[MAX_LINK_STATS];
  int total = sorted(list, MAX_LINK_STATS, now);
  int pos = 0;
  uint8_t written = 0;
  for (int i = offset; i < total && written < count; i++) {
    if (pos + entrySize(prefix_len) > dest_max) break;
    const LinkStatEntry& e = *list[i];
    memcpy(&dest[pos], e.key, prefix_len); pos += prefix_len;
    dest[pos++] = e.kind;
    float w4 = aged(e.w, e.t_last, now) * 4.0f;
    uint16_t w = w4 >= 65535.0f ? 65535 : (uint16_t)lroundf(w4);
    memcpy(&dest[pos], &w, 2); pos += 2;
    dest[pos++] = (uint8_t)clamp8(e.mean * 4.0f);
    float sd4 = sd(e) * 4.0f;
    dest[pos++] = sd4 >= 255.0f ? 255 : (uint8_t)lroundf(sd4);
    dest[pos++] = (uint8_t)(e.wr > 0.0f ? clamp8(e.rssi) : -128);
    uint32_t age = now > e.t_last ? (now - e.t_last) / 60 : 0;
    uint16_t a = age > 65535 ? 65535 : (uint16_t)age;
    memcpy(&dest[pos], &a, 2); pos += 2;
    memcpy(&dest[pos], &e.n, 2); pos += 2;
    written++;
  }
  if (n_out) *n_out = written;
  return pos;
}
