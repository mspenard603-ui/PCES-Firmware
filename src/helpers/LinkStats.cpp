#include "LinkStats.h"
#include <math.h>
#include <string.h>

// How much a weight recorded at time `from` still counts at time `to` (1 when `to` is not later).
static float decayFactor(uint32_t from, uint32_t to) {
  if (to <= from) {
    return 1.0f;
  }
  return exp2f(-(float)(to - from) / (float)LINK_STATS_HALF_LIFE_SECS);
}

static bool isResolved(const LinkStatEntry& e) {
  return (e.kind & LINK_KIND_RESOLVED) != 0;
}

static bool isUsed(const LinkStatEntry& e) {
  return e.t_last != 0;
}

// Pools the statistics of `from` into `into` and empties `from`. Both weights are first aged to
// `now`. The pooled mean is the weighted mean; the pooled sum of squares is the sum of both plus
// the between-means term w1*w2/w * (mean1 - mean2)^2.
static void poolEntries(LinkStatEntry* into, LinkStatEntry* from, uint32_t now) {
  float decay_into = decayFactor(into->t_last, now);
  float decay_from = decayFactor(from->t_last, now);

  float w_into = into->w * decay_into;
  float w_from = from->w * decay_from;
  float w_total = w_into + w_from;
  if (w_total > 0.0f) {
    float mean_diff = into->mean - from->mean;
    into->s = into->s * decay_into + from->s * decay_from + w_into * w_from / w_total * mean_diff * mean_diff;
    into->mean = (w_into * into->mean + w_from * from->mean) / w_total;
  }
  into->w = w_total;

  float wr_into = into->wr * decay_into;
  float wr_from = from->wr * decay_from;
  if (wr_into + wr_from > 0.0f) {
    into->rssi = (wr_into * into->rssi + wr_from * from->rssi) / (wr_into + wr_from);
  }
  into->wr = wr_into + wr_from;

  if ((from->kind & LINK_KIND_ADVERT_ONLY) == 0) {
    into->kind &= ~LINK_KIND_ADVERT_ONLY;
  }
  uint32_t n = (uint32_t)into->n + from->n;
  into->n = n > 0xFFFF ? 0xFFFF : (uint16_t)n;
  into->t_last = now;

  memset(from, 0, sizeof(*from));
}

void LinkStats::clear() {
  memset(_e, 0, sizeof(_e));
  _was_full = false;
}

float LinkStats::aged(float w, uint32_t t_last, uint32_t now) {
  return w * decayFactor(t_last, now);
}

int LinkStats::keyLength(const LinkStatEntry& e) {
  if (isResolved(e)) {
    return LINK_KEY_SIZE;
  }
  return (e.kind & LINK_KIND_WIDTH_MASK) + 1;
}

float LinkStats::sd(const LinkStatEntry& e) const {
  if (e.w <= 0.0f) {
    return 0.0f;
  }
  float variance = e.s / e.w;
  if (variance <= 0.0f) {
    return 0.0f;
  }
  return sqrtf(variance);
}

int LinkStats::count() const {
  int n = 0;
  for (int i = 0; i < MAX_LINK_STATS; i++) {
    if (isUsed(_e[i])) {
      n++;
    }
  }
  return n;
}

// Finds the entry a sample for `key` (`len` bytes) belongs to, or returns NULL for a new one.
//
// One neighbor can be heard under several keys: its 1-, 2- or 3-byte path hash (each packet's
// originator picks the width) and, once resolved, its public key prefix. A key that starts with
// another key is taken as the same neighbor, as long as no other entry also starts with the
// shorter key. For example, with only "F7C3" in the table, a packet heard as "F7" is counted on
// "F7C3"; with both "F7C3" and "F712" present, "F7" could be either and stays an entry of its own.
LinkStatEntry* LinkStats::findEntryFor(const uint8_t* key, uint8_t len, bool resolved) {
  LinkStatEntry* same_key = NULL;   // exactly this key
  LinkStatEntry* longer = NULL;     // an entry with a longer key that starts with this one
  int longer_count = 0;
  LinkStatEntry* shorter = NULL;    // the longest unresolved entry whose key this one starts with

  for (int i = 0; i < MAX_LINK_STATS; i++) {
    LinkStatEntry* e = &_e[i];
    if (!isUsed(*e)) {
      continue;
    }
    int e_len = keyLength(*e);

    if (e_len == len && isResolved(*e) == resolved && memcmp(e->key, key, len) == 0) {
      same_key = e;
    }
    if (e_len > len && memcmp(e->key, key, len) == 0) {
      longer = e;
      longer_count++;
    }
    if (e_len < len && !isResolved(*e) && memcmp(e->key, key, e_len) == 0) {
      if (shorter == NULL || e_len > keyLength(*shorter)) {
        shorter = e;
      }
    }
  }

  if (longer_count == 1) {
    return longer;   // mergeShorterEntriesInto() then folds an entry for the short key into it
  }
  if (same_key != NULL) {
    return same_key;
  }
  if (longer_count == 0 && shorter != NULL) {
    // The neighbor is now heard under a longer key: keep its history under the new key.
    memset(shorter->key, 0, LINK_KEY_SIZE);
    memcpy(shorter->key, key, len);
    uint8_t advert_only = shorter->kind & LINK_KIND_ADVERT_ONLY;
    if (resolved) {
      shorter->kind = LINK_KIND_RESOLVED | advert_only;
    } else {
      shorter->kind = (uint8_t)(len - 1) | advert_only;
    }
    return shorter;
  }
  return NULL;
}

// Folds unresolved entries with a shorter key into `e` when `e` is the only entry whose key starts
// with theirs (see findEntryFor).
void LinkStats::mergeShorterEntriesInto(LinkStatEntry* e, uint32_t now) {
  int e_len = keyLength(*e);
  for (int i = 0; i < MAX_LINK_STATS; i++) {
    LinkStatEntry* candidate = &_e[i];
    if (candidate == e || !isUsed(*candidate) || isResolved(*candidate)) {
      continue;
    }
    int c_len = keyLength(*candidate);
    if (c_len >= e_len || memcmp(candidate->key, e->key, c_len) != 0) {
      continue;
    }

    bool another_match = false;
    for (int j = 0; j < MAX_LINK_STATS; j++) {
      const LinkStatEntry* other = &_e[j];
      if (other == e || other == candidate || !isUsed(*other)) {
        continue;
      }
      if (keyLength(*other) > c_len && memcmp(other->key, candidate->key, c_len) == 0) {
        another_match = true;
        break;
      }
    }
    if (!another_match) {
      poolEntries(e, candidate, now);
    }
  }
}

// An empty slot, or, when the table is full, the entry with the lowest weight aged to now.
LinkStatEntry* LinkStats::freeSlot(uint32_t now) {
  LinkStatEntry* weakest = NULL;
  float weakest_w = 0.0f;
  for (int i = 0; i < MAX_LINK_STATS; i++) {
    LinkStatEntry* e = &_e[i];
    if (!isUsed(*e)) {
      return e;
    }
    float w = aged(e->w, e->t_last, now);
    if (weakest == NULL || w < weakest_w) {
      weakest = e;
      weakest_w = w;
    }
  }
  _was_full = true;
  return weakest;
}

void LinkStats::addPacket(const uint8_t* key, uint8_t key_len, bool resolved, float snr, float rssi, uint32_t now) {
  addSample(key, key_len, resolved, false, snr, true, rssi, now);
}

void LinkStats::addAdvert(const uint8_t* pub_key, float snr, uint32_t now) {
  addSample(pub_key, LINK_KEY_SIZE, true, true, snr, false, 0.0f, now);
}

void LinkStats::addSample(const uint8_t* key, uint8_t key_len, bool resolved, bool from_advert,
                          float snr, bool has_rssi, float rssi, uint32_t now) {
  if (now == 0 || key_len == 0) {
    return;   // without a clock time no age can be kept
  }

  // The key as stored: a resolved key is always looked up as 4 bytes (zero padded); a hash keeps
  // its own width, at most 3 bytes.
  uint8_t stored_key[LINK_KEY_SIZE] = { 0, 0, 0, 0 };
  uint8_t lookup_len;
  uint8_t kind;
  if (resolved) {
    memcpy(stored_key, key, key_len < LINK_KEY_SIZE ? key_len : LINK_KEY_SIZE);
    lookup_len = LINK_KEY_SIZE;
    kind = LINK_KIND_RESOLVED;
  } else {
    uint8_t hash_len = key_len > LINK_MAX_HASH_SIZE ? LINK_MAX_HASH_SIZE : key_len;
    memcpy(stored_key, key, hash_len);
    lookup_len = hash_len;
    kind = (uint8_t)(hash_len - 1);
  }

  LinkStatEntry* e = findEntryFor(stored_key, lookup_len, resolved);
  if (e == NULL) {
    e = freeSlot(now);
    memset(e, 0, sizeof(*e));
    memcpy(e->key, stored_key, LINK_KEY_SIZE);
    e->kind = kind;
    if (from_advert) {
      e->kind |= LINK_KIND_ADVERT_ONLY;
    }
  }
  if (!from_advert) {
    e->kind &= ~LINK_KIND_ADVERT_ONLY;
  }

  // Age the old weight, then add this sample with weight 1 (a new entry starts from nothing).
  float decay = 0.0f;
  if (isUsed(*e)) {
    decay = decayFactor(e->t_last, now);
  }
  float w = e->w * decay + 1.0f;
  float delta = snr - e->mean;
  float mean = e->mean + delta / w;
  e->s = e->s * decay + delta * (snr - mean);
  e->mean = mean;
  e->w = w;

  if (has_rssi) {
    float wr = e->wr * decay + 1.0f;
    e->rssi += (rssi - e->rssi) / wr;
    e->wr = wr;
  } else {
    e->wr *= decay;
  }

  e->t_last = now;
  if (e->n < 0xFFFF) {
    e->n++;
  }
  mergeShorterEntriesInto(e, now);
}

int LinkStats::sorted(const LinkStatEntry** out, int max, uint32_t now) const {
  float weights[MAX_LINK_STATS];
  int n = 0;
  for (int i = 0; i < MAX_LINK_STATS && n < max; i++) {
    if (!isUsed(_e[i])) {
      continue;
    }
    float w = aged(_e[i].w, _e[i].t_last, now);

    // insertion sort, highest weight first
    int j = n;
    while (j > 0 && weights[j - 1] < w) {
      weights[j] = weights[j - 1];
      out[j] = out[j - 1];
      j--;
    }
    weights[j] = w;
    out[j] = &_e[i];
    n++;
  }
  return n;
}

// Rounds to an int8, keeping -128 free to mean "none".
static int8_t roundToInt8(float v) {
  if (v != v) {
    return -128;   // NaN
  }
  long r = lroundf(v);
  if (r < -127) {
    return -127;
  }
  if (r > 127) {
    return 127;
  }
  return (int8_t)r;
}

static uint16_t roundToU16(float v) {
  if (v >= 65535.0f) {
    return 65535;
  }
  return (uint16_t)lroundf(v);
}

static uint8_t roundToU8(float v) {
  if (v >= 255.0f) {
    return 255;
  }
  return (uint8_t)lroundf(v);
}

static void putU16(uint8_t* dest, int* pos, uint16_t v) {
  memcpy(&dest[*pos], &v, 2);
  *pos += 2;
}

int LinkStats::encode(uint8_t* dest, int dest_max, uint8_t offset, uint8_t count, uint8_t prefix_len,
                      uint32_t now, uint8_t* n_out) const {
  if (prefix_len < 1) {
    prefix_len = 1;
  }
  if (prefix_len > LINK_KEY_SIZE) {
    prefix_len = LINK_KEY_SIZE;
  }

  const LinkStatEntry* list[MAX_LINK_STATS];
  int total = sorted(list, MAX_LINK_STATS, now);
  int pos = 0;
  uint8_t written = 0;

  for (int i = offset; i < total && written < count; i++) {
    if (pos + entrySize(prefix_len) > dest_max) {
      break;
    }
    const LinkStatEntry& e = *list[i];

    memcpy(&dest[pos], e.key, prefix_len);
    pos += prefix_len;
    dest[pos++] = e.kind;
    putU16(dest, &pos, roundToU16(aged(e.w, e.t_last, now) * 4.0f));
    dest[pos++] = (uint8_t)roundToInt8(e.mean * 4.0f);
    dest[pos++] = roundToU8(sd(e) * 4.0f);
    if (e.wr > 0.0f) {
      dest[pos++] = (uint8_t)roundToInt8(e.rssi);
    } else {
      dest[pos++] = (uint8_t)(int8_t)-128;   // no RSSI yet
    }

    uint32_t age_min = 0;
    if (now > e.t_last) {
      age_min = (now - e.t_last) / 60;
    }
    putU16(dest, &pos, age_min > 65535 ? 65535 : (uint16_t)age_min);
    putU16(dest, &pos, e.n);
    written++;
  }

  if (n_out != NULL) {
    *n_out = written;
  }
  return pos;
}
