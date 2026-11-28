#include "MyMesh.h"
#include <algorithm>

/* ------------------------------ Config -------------------------------- */

#ifndef LORA_FREQ
  #define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
  #define LORA_BW 250
#endif
#ifndef LORA_SF
  #define LORA_SF 10
#endif
#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif

#ifndef ADVERT_NAME
  #define ADVERT_NAME "repeater"
#endif
#ifndef ADVERT_LAT
  #define ADVERT_LAT 0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON 0.0
#endif

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY 300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY 200
#endif

#define FIRMWARE_VER_LEVEL       2

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07     // FIRMWARE_VER_LEVEL >= 2

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define ANON_REQ_TYPE_REGIONS      0x01
#define ANON_REQ_TYPE_OWNER        0x02
#define ANON_REQ_TYPE_BASIC        0x03   // just remote clock
// PCES (docs/pces/). The request types are provisional until MeshCore allocates them; they must
// stay below 0x20, as onAnonDataRecv reads anything from 0x20 up as a login password.
#define ANON_REQ_TYPE_LINK_STATS   0x1C   // link statistics (docs/pces/link-stats.md)
#define ANON_REQ_TYPE_LOAD         0x1D   // channel load (docs/pces/load.md)

#define PCES_FEAT1_FAMILY          0xC500 // advert feature 1: the PCES marker family 0xC5xx
#define PCES_FEAT1_LINK_STATS      0x0004 // answers link-stats requests
#define PCES_FEAT1_LOAD            0x0008 // answers channel-load requests
#define PCES_PREF_FILE             "/pces_ls"
#define PCES_CLI_REPLY_MAX         150    // longest CLI reply sent over the air
#define PCES_MORE_LINE_ROOM        20     // room kept for the "more: linkstats N" line
#define LOAD_BUSY_SAMPLE_MS        50     // one RSSI reading every 50 ms while receiving
#define LOAD_BUSY_DB               6      // busy: RSSI more than this above the noise floor
#define LINK_STATS_MAX_REPLY       167    // MAX_PACKET_PAYLOAD - CIPHER_MAC_SIZE - (CIPHER_BLOCK_SIZE - 1)

// "on" or "off" -> *value; false for anything else.
static bool parseOnOff(const char* text, bool* value) {
  if (strcmp(text, "on") == 0) {
    *value = true;
    return true;
  }
  if (strcmp(text, "off") == 0) {
    *value = false;
    return true;
  }
  return false;
}

#define CLI_REPLY_DELAY_MILLIS      600

#define LAZY_CONTACTS_WRITE_DELAY    5000

void MyMesh::putNeighbour(const mesh::Identity &id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS // check if neighbours enabled
  // find existing neighbour, else use least recently updated
  uint32_t oldest_timestamp = 0xFFFFFFFF;
  NeighbourInfo *neighbour = &neighbours[0];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    // if neighbour already known, we should update it
    if (id.matches(neighbours[i].id)) {
      neighbour = &neighbours[i];
      break;
    }

    // otherwise we should update the least recently updated neighbour
    if (neighbours[i].heard_timestamp < oldest_timestamp) {
      neighbour = &neighbours[i];
      oldest_timestamp = neighbour->heard_timestamp;
    }
  }

  // update neighbour info
  neighbour->id = id;
  neighbour->advert_timestamp = timestamp;
  neighbour->heard_timestamp = getRTCClock()->getCurrentTime();
  neighbour->snr = (int8_t)(snr * 4);
#endif
}

uint8_t MyMesh::handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood) {
  ClientInfo* client = NULL;
  if (data[0] == 0) {   // blank password, just check if sender is in ACL
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    if (client == NULL) {
    #if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Login, sender not in ACL");
    #endif
    }
  }
  if (client == NULL) {
    uint8_t perms;
    if (strcmp((char *)data, _prefs.password) == 0) { // check for valid admin password
      perms = PERM_ACL_ADMIN;
    } else if (strcmp((char *)data, _prefs.guest_password) == 0) { // check guest password
      perms = PERM_ACL_GUEST;
    } else {
#if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Invalid password: %s", data);
#endif
      return 0;
    }

    client = acl.putClient(sender, 0);  // add to contacts (if not already known)
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("Possible login replay attack!");
      return 0;  // FATAL: client table is full -OR- replay attack
    }

    MESH_DEBUG_PRINTLN("Login success!");
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perms;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    if (perms != PERM_ACL_GUEST) {   // keep number of FS writes to a minimum
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }

  if (is_flood) {
    client->out_path_len = OUT_PATH_UNKNOWN;  // need to rediscover out_path
  }

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;  // Legacy: was recommended keep-alive interval (secs / 16)
  reply_data[6] = client->isAdmin() ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

  return 13;  // reply length
}

uint8_t MyMesh::handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)

    return 8 + region_map.exportNamesTo((char *) &reply_data[8], sizeof(reply_data) - 12, REGION_DENY_FLOOD);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    sprintf((char *) &reply_data[8], "%s\n%s", _prefs.node_name, _prefs.owner_info);

    return 8 + strlen((char *) &reply_data[8]);   // reply length
  }
  return 0;
}

// ---- PCES: link statistics and channel load (docs/pces/link-stats.md, docs/pces/load.md) ----

static uint8_t clampToU8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

static uint16_t clampToU16(uint64_t v) {
  if (v > 65535) return 65535;
  return (uint16_t)v;
}

// A signed value as one byte (int8, two's complement), clamped to -128..127.
static uint8_t toInt8Byte(int v) {
  if (v < -128) v = -128;
  if (v > 127) v = 127;
  return (uint8_t)(int8_t)v;
}

static int putU16(uint8_t* dest, int pos, uint16_t v) {
  memcpy(&dest[pos], &v, 2);
  return pos + 2;
}

static int putU32(uint8_t* dest, int pos, uint32_t v) {
  memcpy(&dest[pos], &v, 4);
  return pos + 4;
}

// Anonymous request 0x1C: the link-stats table. Request data after the sub-type:
//   {reply-path-len}{reply-path}{version = 1}{offset}{count}{prefix_len}
// The answer is a 17-byte header, then as many table entries as fit (LinkStats::encode).
uint8_t MyMesh::handleAnonLinkStatsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data, size_t len) {
  if (!anon_limiter.allow(rtc_clock.getCurrentTime())) return 0;
  if (len < 1) return 0;
  reply_path_len = *data++;
  len--;
  if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding
  size_t path_bytes = mesh::Packet::writePath(reply_path, data, reply_path_len);
  if (len < path_bytes + 4) return 0;
  data += path_bytes;

  uint8_t version = data[0];
  uint8_t offset = data[1];
  uint8_t count = data[2];
  uint8_t prefix_len = data[3];
  if (version != 1) return 0;

  uint8_t flags = 0x01;        // bit 0: link stats on
  if (link_stats.wasFull()) {
    flags |= 0x02;             // bit 1: the table has been full
  }

  uint32_t now = getRTCClock()->getCurrentTime();
  int i = 0;
  i = putU32(reply_data, i, sender_timestamp);   // the request's timestamp, as a tag
  i = putU32(reply_data, i, now);
  reply_data[i++] = 1;   // version
  reply_data[i++] = flags;
  reply_data[i++] = toInt8Byte(_radio->getNoiseFloor());
  reply_data[i++] = (uint8_t)(LINK_STATS_HALF_LIFE_SECS / 3600);
  i = putU16(reply_data, i, clampToU16(uptime_millis / 3600000ULL));
  reply_data[i++] = (uint8_t)link_stats.count();
  reply_data[i++] = offset;
  int count_pos = i++;   // the number of entries sent, filled in below

  uint8_t written = 0;
  i += link_stats.encode(&reply_data[i], LINK_STATS_MAX_REPLY - i, offset, count, prefix_len, now, &written);
  reply_data[count_pos] = written;
  return i;
}

// Formats v with one decimal: "12.3", or "+12.3" with show_plus. (No %f: the newlib-nano used on
// some boards doesn't support it.)
static void formatTenths(char* out, float v, bool show_plus) {
  long tenths = lroundf(v * 10.0f);
  const char* sign = "";
  if (tenths < 0) {
    sign = "-";
    tenths = -tenths;
  } else if (show_plus) {
    sign = "+";
  }
  sprintf(out, "%s%ld.%ld", sign, tenths / 10, tenths % 10);
}

// The entry's key in hex: 2 bytes for a resolved neighbor (enough to recognize it), otherwise the
// hash as heard, followed by "?".
static void formatLinkKey(char* out, const LinkStatEntry& e) {
  bool resolved = (e.kind & LINK_KIND_RESOLVED) != 0;
  int bytes = resolved ? 2 : LinkStats::keyLength(e);
  out[0] = 0;
  for (int i = 0; i < bytes; i++) {
    sprintf(&out[i * 2], "%02X", (uint32_t)e.key[i]);
  }
  if (!resolved) {
    strcat(out, "?");
  }
}

static void formatAge(char* out, uint32_t secs) {
  if (secs < 3600) {
    sprintf(out, "%lum", (unsigned long)(secs / 60));
  } else if (secs < 2 * 86400) {
    sprintf(out, "%luh", (unsigned long)(secs / 3600));
  } else {
    sprintf(out, "%lud", (unsigned long)(secs / 86400));
  }
}

// The text of one link-stats row.
struct LinkRowText {
  char key[12];
  char snr[12];
  char sd[12];
  char age[12];
  int weight;
};

static void describeLinkEntry(const LinkStats& stats, const LinkStatEntry& e, uint32_t now, LinkRowText* row) {
  formatLinkKey(row->key, e);
  formatTenths(row->snr, e.mean, true);
  formatTenths(row->sd, stats.sd(e), false);
  uint32_t age = 0;
  if (now > e.t_last) {
    age = now - e.t_last;
  }
  formatAge(row->age, age);
  row->weight = (int)lroundf(LinkStats::aged(e.w, e.t_last, now));
}

// CLI "linkstats [N]": the table, highest weight first. Over USB serial every entry is printed with
// its RSSI and sample count. A remote reply starts with a legend and lists entries from N while
// they fit; if some are left out, its last line gives the command for the rest, e.g.
// "more: linkstats 5".
void MyMesh::formatLinkStats(char* reply, int offset, bool serial) {
  if (!link_stats_on) {
    strcpy(reply, "linkstats off (set pces.linkstats on)");
    return;
  }
  uint32_t now = getRTCClock()->getCurrentTime();
  const LinkStatEntry* list[MAX_LINK_STATS];
  int n = link_stats.sorted(list, MAX_LINK_STATS, now);

  if (serial) {
    Serial.printf("link stats: %d of %d, half-life %luh\r\n", n, MAX_LINK_STATS,
                  (unsigned long)(LINK_STATS_HALF_LIFE_SECS / 3600));
    for (int i = 0; i < n; i++) {
      const LinkStatEntry& e = *list[i];
      LinkRowText row;
      describeLinkEntry(link_stats, e, now, &row);
      int rssi = 0;
      if (e.wr > 0) {
        rssi = (int)lroundf(e.rssi);
      }
      const char* note = "";
      if (e.kind & LINK_KIND_ADVERT_ONLY) {
        note = " advert";
      }
      Serial.printf("%-7s snr %s sd %s w %d rssi %d n %u age %s%s\r\n",
                    row.key, row.snr, row.sd, row.weight, rssi, (uint32_t)e.n, row.age, note);
    }
    reply[0] = 0;
    return;
  }

  int pos = sprintf(reply, "%d %s: key SNR/sd dB, w=weight, age", n, n == 1 ? "link" : "links");
  int i = offset;
  while (i < n) {
    LinkRowText row;
    describeLinkEntry(link_stats, *list[i], now, &row);
    char line[48];
    int len = sprintf(line, "\n%s %s/%s w%d %s", row.key, row.snr, row.sd, row.weight, row.age);

    int limit = PCES_CLI_REPLY_MAX;
    bool last_entry = (i + 1 == n);
    if (!last_entry) {
      limit -= PCES_MORE_LINE_ROOM;   // keep room for the "more" line
    }
    if (pos + len > limit) break;

    strcpy(&reply[pos], line);
    pos += len;
    i++;
  }
  if (i < n) {
    sprintf(&reply[pos], "\nmore: linkstats %d", i);
  }
}

// The PCES switches are saved in PCES_PREF_FILE: [link stats on][load on], one byte each. Both
// default to on (flashing this firmware is the opt-in). A file from a build before channel load
// has only the first byte; load is then on.
void MyMesh::loadLinkStatsPref() {
  link_stats_on = true;
  load_on = true;
  if (!_fs->exists(PCES_PREF_FILE)) return;

#if defined(RP2040_PLATFORM)
  File f = _fs->open(PCES_PREF_FILE, "r");
#else
  File f = _fs->open(PCES_PREF_FILE);
#endif
  if (f) {
    uint8_t v[2] = { 1, 1 };
    int n = f.read(v, 2);
    if (n >= 1) link_stats_on = v[0] != 0;
    if (n >= 2) load_on = v[1] != 0;
    f.close();
  }
}

void MyMesh::saveLinkStatsPref() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(PCES_PREF_FILE);
  File f = _fs->open(PCES_PREF_FILE, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File f = _fs->open(PCES_PREF_FILE, "w");
#else
  File f = _fs->open(PCES_PREF_FILE, "w", true);
#endif
  if (f) {
    uint8_t v[2];
    v[0] = link_stats_on ? 1 : 0;
    v[1] = load_on ? 1 : 0;
    f.write(v, 2);
    f.close();
  }
}

// Anonymous request 0x1D: channel load. Request data after the sub-type:
//   {reply-path-len}{reply-path}{version = 1}
// The answer is an 18-byte header and five 22-byte windows (1 min, 15 min, 1 h, 24 h, busiest
// hour): 128 bytes.
uint8_t MyMesh::handleAnonLoadReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data, size_t len) {
  if (!anon_limiter.allow(rtc_clock.getCurrentTime())) return 0;
  if (len < 1) return 0;
  reply_path_len = *data++;
  len--;
  if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding
  size_t path_bytes = mesh::Packet::writePath(reply_path, data, reply_path_len);
  if (len < path_bytes + 1) return 0;
  data += path_bytes;
  uint8_t version = data[0];
  if (version != 1) return 0;

  int noise_floor = _radio->getNoiseFloor();
  bool busy_sampled = noise_floor != 0 && !_prefs.powersaving_enabled;
  bool listen_before_talk = _prefs.interference_threshold > 0 || _prefs.cad_enabled;
  uint8_t flags = 0x01;                              // bit 0: load on
  if (busy_sampled) flags |= 0x02;                   // bit 1: the busy share is being sampled
  if (listen_before_talk) flags |= 0x04;             // bit 2: it waits while the channel is busy
  if (_prefs.powersaving_enabled) flags |= 0x08;     // bit 3: power saving on (busy not sampled)

  uint32_t now = getRTCClock()->getCurrentTime();
  int i = 0;
  i = putU32(reply_data, i, sender_timestamp);   // the request's timestamp, as a tag
  i = putU32(reply_data, i, now);
  reply_data[i++] = 1;   // version
  reply_data[i++] = flags;
  reply_data[i++] = toInt8Byte(noise_floor);
  reply_data[i++] = LOAD_BUSY_DB;
  reply_data[i++] = clampToU8((int)lroundf(_prefs.airtime_factor * 10.0f));
  i = putU16(reply_data, i, clampToU16(uptime_millis / 3600000ULL));
  reply_data[i++] = clampToU8(_mgr->getOutboundTotal());   // TX queue now
  reply_data[i++] = clampToU8(_mgr->getFreeCount());       // free packet buffers
  int peak_ago_pos = i++;
  reply_data[peak_ago_pos] = 0xFF;                         // no busiest hour yet

  static const int window_minutes[3] = { 1, 15, 60 };
  LoadBucket b;
  for (int k = 0; k < 3; k++) {
    bool ok = load_stats.window(window_minutes[k], &b);
    i += LoadStats::encodeWindow(&reply_data[i], b, ok);
  }
  bool ok = load_stats.day(&b);
  i += LoadStats::encodeWindow(&reply_data[i], b, ok);

  uint8_t peak_ago = 0;
  ok = load_stats.peak(&b, &peak_ago);
  if (ok) {
    reply_data[peak_ago_pos] = peak_ago;
  }
  i += LoadStats::encodeWindow(&reply_data[i], b, ok);
  return i;
}

// Called from loop(), right after the mesh has handled the radio.
void MyMesh::sampleLoad() {
  if (!load_on) return;
  unsigned long now = _ms->getMillis();

  // Busy share: one RSSI reading every LOAD_BUSY_SAMPLE_MS while the radio is receiving (so our
  // own TX never counts), once the noise floor is known. Skipped with power saving on: the MCU
  // then sleeps between radio events, the loop runs mostly right after a packet, and the readings
  // would be biased. Sampling never adds a wake-up.
  if ((long)(now - next_busy_sample) >= 0) {
    next_busy_sample = now + LOAD_BUSY_SAMPLE_MS;
    int noise_floor = _radio->getNoiseFloor();
    bool can_sample = noise_floor != 0 && !_prefs.powersaving_enabled && radio_driver.isInRecvMode();
    if (can_sample) {
      bool busy = radio_driver.getCurrentRSSI() > noise_floor + LOAD_BUSY_DB;
      load_stats.busySample(busy);
    }
  }

  // Once a second: the counters the firmware keeps anyway.
  uint32_t elapsed_s = (uint32_t)(now - last_load_tick) / 1000;
  if (elapsed_s == 0) return;
  last_load_tick += elapsed_s * 1000;

  LoadCounters c;
  c.tx_ms = getTotalAirTime();
  c.rx_ms = getReceiveAirTime();
  c.n_rx = radio_driver.getPacketsRecv();
  c.n_tx = radio_driver.getPacketsSent();
  c.n_flood = getNumRecvFlood();
  c.n_dup = ((SimpleMeshTables *)getTables())->getNumFloodDups();
  c.n_err = radio_driver.getPacketsRecvErrors();

  // "Held": a packet is ready to go, but the airtime budget doesn't cover half a full-size packet.
  bool packet_ready = _mgr->getOutboundCount(now) > 0;
  bool held = packet_ready && getRemainingTxBudget() < _radio->getEstAirtimeFor(MAX_TRANS_UNIT) / 2;
  load_stats.tick(elapsed_s, c, clampToU8(_mgr->getOutboundTotal()), held);
}

// Advert feature 1: the PCES marker family with a bit per answer this repeater gives, or 0 (no
// marker) when both are off.
uint16_t MyMesh::pcesFeat1() const {
  uint16_t bits = 0;
  if (link_stats_on) bits |= PCES_FEAT1_LINK_STATS;
  if (load_on) bits |= PCES_FEAT1_LOAD;
  if (bits == 0) return 0;
  return PCES_FEAT1_FAMILY | bits;
}

// A share in percent with one decimal, from permille; "-" when it wasn't measured (0xFFFF).
static void formatPercent(char* out, uint16_t permille) {
  if (permille == 0xFFFF) {
    strcpy(out, "-");
    return;
  }
  sprintf(out, "%u.%u", (uint32_t)(permille / 10), (uint32_t)(permille % 10));
}

// One line of the "load" CLI output: a window of the history and its label.
struct LoadWindow {
  char label[16];
  LoadBucket bucket;
};

// The windows "load" shows: 1m, 15m, 1h, 24h and the busiest hour. After a restart a window covers
// only the complete minutes or hours kept so far. A window that would cover no more than the one
// before it is left out (it would repeat it), and a short one says what it covers, e.g. "15m(4)" or
// "24h(3h)". The busiest hour is shown once there are two hours to choose from.
static int collectLoadWindows(const LoadStats& stats, LoadWindow* out) {
  static const int window_minutes[3] = { 1, 15, 60 };
  static const char* window_names[3] = { "1m", "15m", "1h" };
  int n = 0;
  uint32_t shown_minutes = 0;   // minutes covered by the last window added

  for (int k = 0; k < 3; k++) {
    LoadBucket b;
    if (!stats.window(window_minutes[k], &b)) continue;
    uint32_t covered = b.secs / 60;
    if (covered <= shown_minutes) continue;
    if (covered < (uint32_t)window_minutes[k]) {
      sprintf(out[n].label, "%s(%lu)", window_names[k], (unsigned long)covered);
    } else {
      strcpy(out[n].label, window_names[k]);
    }
    out[n].bucket = b;
    n++;
    shown_minutes = covered;
  }

  LoadBucket day;
  if (stats.day(&day) && day.secs / 60 > shown_minutes) {
    int hours = stats.hoursKept();
    if (hours < 24) {
      sprintf(out[n].label, "24h(%dh)", hours);
    } else {
      strcpy(out[n].label, "24h");
    }
    out[n].bucket = day;
    n++;
  }

  LoadBucket peak;
  uint8_t peak_ago = 0;
  if (stats.hoursKept() >= 2 && stats.peak(&peak, &peak_ago)) {
    sprintf(out[n].label, "peak-%uh", (uint32_t)peak_ago);
    out[n].bucket = peak;
    n++;
  }
  return n;
}

// CLI "load": TX / RX / busy shares (%) and the TX queue (average / maximum) per window. Over USB
// serial it also prints the noise floor, the held share and the packet counts.
void MyMesh::formatLoad(char* reply, bool serial) {
  if (!load_on) {
    strcpy(reply, "load off (set pces.load on)");
    return;
  }
  LoadWindow windows[5];
  int n = collectLoadWindows(load_stats, windows);

  int pos = 0;
  if (serial) {
    Serial.printf("load: noise floor %d dBm, busy above +%d dB, %d min / %d h kept, queue %d\r\n",
                  _radio->getNoiseFloor(), LOAD_BUSY_DB, load_stats.minutesKept(), load_stats.hoursKept(),
                  _mgr->getOutboundTotal());
  } else {
    pos = sprintf(reply, "load %% tx/rx/busy q");
  }

  for (int k = 0; k < n; k++) {
    const LoadBucket& b = windows[k].bucket;
    char tx[8], rx[8], busy[8], held[8];
    formatPercent(tx, LoadStats::txPermille(b));
    formatPercent(rx, LoadStats::rxPermille(b));
    formatPercent(busy, LoadStats::busyPermille(b));
    formatPercent(held, LoadStats::heldPermille(b));
    uint32_t queue_avg_x10 = 0;
    if (b.secs > 0) {
      queue_avg_x10 = (b.q_sum * 10 + b.secs / 2) / b.secs;
    }
    unsigned long queue_whole = queue_avg_x10 / 10;
    unsigned long queue_tenth = queue_avg_x10 % 10;

    if (serial) {
      Serial.printf("%-8s tx %s%% rx %s%% busy %s%% held %s%% q %lu.%lu/%u rx %lu tx %lu flood %lu dup %lu err %lu (%lu min)\r\n",
                    windows[k].label, tx, rx, busy, held, queue_whole, queue_tenth, (uint32_t)b.q_max,
                    (unsigned long)b.n_rx, (unsigned long)b.n_tx, (unsigned long)b.n_flood,
                    (unsigned long)b.n_dup, (unsigned long)b.n_err, (unsigned long)(b.secs / 60));
    } else {
      char line[48];
      int len = sprintf(line, "\n%s %s/%s/%s %lu.%lu/%u", windows[k].label, tx, rx, busy,
                        queue_whole, queue_tenth, (uint32_t)b.q_max);
      if (pos + len > PCES_CLI_REPLY_MAX) break;
      strcpy(&reply[pos], line);
      pos += len;
    }
  }

  if (serial) {
    reply[0] = 0;
  } else if (load_stats.minutesKept() == 0) {
    strcpy(&reply[pos], "\n(first minute not complete)");
  }
}

uint8_t MyMesh::handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    reply_data[8] = 0;  // features
#ifdef WITH_RS232_BRIDGE
    reply_data[8] |= 0x01;  // is bridge, type UART
#elif WITH_ESPNOW_BRIDGE
    reply_data[8] |= 0x03;  // is bridge, type ESP-NOW
#endif
    if (_prefs.disable_fwd) {   // is this repeater currently disabled
      reply_data[8] |= 0x80;  // is disabled
    }
    // TODO:  add some kind of moving-window utilisation metric, so can query 'how busy' is this repeater
    return 9;   // reply length
  }
  return 0;
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload, size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {  // guests can also access this now
    RepeaterStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundTotal();
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
    stats.n_recv_errors = radio_driver.getPacketsRecvErrors();
    memcpy(&reply_data[4], &stats, sizeof(stats));

    return 4 + sizeof(stats); //  reply_len
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);

    // query other sensors -- target specific
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

	// This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;  // skip deleted entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    uint8_t request_version = payload[1];
    if (request_version == 0) {

      // reply data offset (after response sender_timestamp/tag)
      int reply_offset = 4;

      // get request params
      uint8_t count = payload[2]; // how many neighbours to fetch (0-255)
      uint16_t offset;
      memcpy(&offset, &payload[3], 2); // offset from start of neighbours list (0-65535)
      uint8_t order_by = payload[5]; // how to order neighbours. 0=newest_to_oldest, 1=oldest_to_newest, 2=strongest_to_weakest, 3=weakest_to_strongest
      uint8_t pubkey_prefix_length = payload[6]; // how many bytes of neighbour pub key we want
      // we also send a 4 byte random blob in payload[7...10] to help packet uniqueness

      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS count=%d, offset=%d, order_by=%d, pubkey_prefix_length=%d", count, offset, order_by, pubkey_prefix_length);

      // clamp pub key prefix length to max pub key length
      if(pubkey_prefix_length > PUB_KEY_SIZE){
        pubkey_prefix_length = PUB_KEY_SIZE;
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS invalid pubkey_prefix_length=%d clamping to %d", pubkey_prefix_length, PUB_KEY_SIZE);
      }

      // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
      int16_t neighbours_count = 0;
#if MAX_NEIGHBOURS
      NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
      for (int i = 0; i < MAX_NEIGHBOURS; i++) {
        auto neighbour = &neighbours[i];
        if (neighbour->heard_timestamp > 0) {
          sorted_neighbours[neighbours_count] = neighbour;
          neighbours_count++;
        }
      }

      // sort neighbours based on order
      if (order_by == 0) {
        // sort by newest to oldest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting newest to oldest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp > b->heard_timestamp; // desc
        });
      } else if (order_by == 1) {
        // sort by oldest to newest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting oldest to newest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp < b->heard_timestamp; // asc
        });
      } else if (order_by == 2) {
        // sort by strongest to weakest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting strongest to weakest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr > b->snr; // desc
        });
      } else if (order_by == 3) {
        // sort by weakest to strongest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting weakest to strongest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr < b->snr; // asc
        });
      }
#endif

      // build results buffer
      int results_count = 0;
      int results_offset = 0;
      uint8_t results_buffer[130];
      for(int index = 0; index < count && index + offset < neighbours_count; index++){

        // stop if we can't fit another entry in results
        int entry_size = pubkey_prefix_length + 4 + 1;
        if(results_offset + entry_size > sizeof(results_buffer)){
          MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS no more entries can fit in results buffer");
          break;
        }

#if MAX_NEIGHBOURS
        // add next neighbour to results
        auto neighbour = sorted_neighbours[index + offset];
        uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
        memcpy(&results_buffer[results_offset], neighbour->id.pub_key, pubkey_prefix_length); results_offset += pubkey_prefix_length;
        memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4); results_offset += 4;
        memcpy(&results_buffer[results_offset], &neighbour->snr, 1); results_offset += 1;
        results_count++;
#endif

      }

      // build reply
      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS neighbours_count=%d results_count=%d", neighbours_count, results_count);
      memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_buffer, results_offset); reply_offset += results_offset;

      return reply_offset;
    }
  } else if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    sprintf((char *) &reply_data[4], "%s\n%s\n%s", FIRMWARE_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + strlen((char *) &reply_data[4]);
  }
  return 0; // unknown command
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data, pcesFeat1());

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

static uint8_t max_loop_minimal[] =  { 0, /* 1-byte */  4, /* 2-byte */  2, /* 3-byte */  1 };
static uint8_t max_loop_moderate[] = { 0, /* 1-byte */  2, /* 2-byte */  1, /* 3-byte */  1 };
static uint8_t max_loop_strict[] =   { 0, /* 1-byte */  1, /* 2-byte */  1, /* 3-byte */  1 };

bool MyMesh::isLooped(const mesh::Packet* packet, const uint8_t max_counters[]) {
  uint8_t hash_size = packet->getPathHashSize();
  uint8_t hash_count = packet->getPathHashCount();
  uint8_t n = 0;
  const uint8_t* path = packet->path;
  while (hash_count > 0) {      // count how many times this node is already in the path
    if (self_id.isHashMatch(path, hash_size)) n++;
    hash_count--;
    path += hash_size;
  }
  return n >= max_counters[hash_size];
}

void MyMesh::sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size) {
  TransportKey req_scope;
  bool is_wildcard = recv_pkt_region != NULL && recv_pkt_region->isWildcard();
  bool req_scope_known = recv_pkt_region != NULL && !is_wildcard
                      && region_map.getTransportKeysFor(*recv_pkt_region, &req_scope, 1) > 0;

  switch (mesh::chooseReplyScope(req_scope_known, is_wildcard, !default_scope.isNull())) {
    case mesh::REPLY_SCOPE_REQUEST:
      sendFloodScoped(req_scope, packet, delay_millis, path_hash_size);   // reply with same scope as request
      break;
    case mesh::REPLY_SCOPE_DEFAULT:
      // requester's scope is unknown: DIRECT request (no transport codes), or code matched no Region.
      // un-scoped would be dropped at hop 0 by repeaters running flood.max.unscoped=0
      sendFloodScoped(default_scope, packet, delay_millis, path_hash_size);
      break;
    case mesh::REPLY_SCOPE_NONE:
      sendFlood(packet, delay_millis, path_hash_size);  // send un-scoped
      break;
  }
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood()
      && mesh::isFloodHopLimitExceeded(packet, _prefs.flood_max, _prefs.flood_max_unscoped, _prefs.flood_max_advert)) {
    return false;
  }
  if (packet->isRouteFlood() && recv_pkt_region == NULL) {
    MESH_DEBUG_PRINTLN("allowPacketForward: unknown transport code, or wildcard not allowed for FLOOD packet");
    return false;
  }
  if (packet->isRouteFlood() && _prefs.loop_detect != LOOP_DETECT_OFF) {
    const uint8_t* maximums;
    if (_prefs.loop_detect == LOOP_DETECT_MINIMAL) {
      maximums = max_loop_minimal;
    } else if (_prefs.loop_detect == LOOP_DETECT_MODERATE) {
      maximums = max_loop_moderate;
    } else {
      maximums = max_loop_strict;
    }
    if (isLooped(packet, maximums)) {
      MESH_DEBUG_PRINTLN("allowPacketForward: FLOOD packet loop detected!");
      return false;
    }
  }
  return true;
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

// The neighbor a packet was relayed by, from its hash: the one entry of the legacy neighbor table
// (filled from zero-hop repeater adverts) whose key starts with it. Writes its 4-byte key prefix;
// false when no entry, or more than one, matches.
bool MyMesh::resolveNeighbour(const uint8_t* hash, uint8_t len, uint8_t* key4) {
#if MAX_NEIGHBOURS
  int found = -1;
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    if (neighbours[i].heard_timestamp == 0) continue;
    if (memcmp(neighbours[i].id.pub_key, hash, len) == 0) {
      if (found >= 0) return false;   // several share the hash
      found = i;
    }
  }
  if (found < 0) return false;
  memcpy(key4, neighbours[found].id.pub_key, LINK_KEY_SIZE);
  return true;
#else
  return false;
#endif
}

// Called for every packet received (from logRx, duplicates included). Samples the link from the
// neighbor that sent this copy, when the packet names it:
//  - a flood packet: the last hash in its path is the repeater that just relayed it;
//  - a TRACE: after k SNR bytes, route hop k-1 sent it.
// Other packets don't name the sender: a direct packet's path holds the hops still ahead, and a
// flood with an empty path comes from its originator (zero-hop adverts: see onAdvertRecv).
void MyMesh::sampleLinkStats(const mesh::Packet* pkt) {
  if (!link_stats_on) return;

  const uint8_t* hash = NULL;
  uint8_t hash_len = 0;
  if (pkt->getPayloadType() == PAYLOAD_TYPE_TRACE) {
    if (!pkt->isRouteDirect() || pkt->payload_len < 9) return;
    uint8_t snr_count = pkt->path_len;   // a TRACE's path holds the SNRs added so far
    if (snr_count == 0 || snr_count >= MAX_PATH_SIZE) return;
    uint8_t size_code = pkt->payload[8] & 0x03;   // route hashes are (1 << size_code) bytes
    hash_len = 1 << size_code;
    uint16_t hash_pos = 9 + ((uint16_t)(snr_count - 1) << size_code);
    if (hash_pos + hash_len > pkt->payload_len) return;
    hash = &pkt->payload[hash_pos];
  } else if (pkt->isRouteFlood()) {
    uint8_t hop_count = pkt->getPathHashCount();
    if (hop_count == 0) return;
    hash_len = pkt->getPathHashSize();
    hash = &pkt->path[(hop_count - 1) * hash_len];
  } else {
    return;
  }
  if (self_id.isHashMatch(hash, hash_len)) return;   // a copy we relayed ourselves

  uint32_t now = getRTCClock()->getCurrentTime();
  float snr = pkt->getSNR();
  float rssi = _radio->getLastRSSI();
  uint8_t key[LINK_KEY_SIZE];
  bool resolved = resolveNeighbour(hash, hash_len, key);
  if (resolved) {
    link_stats.addPacket(key, LINK_KEY_SIZE, resolved, snr, rssi, now);
  } else {
    link_stats.addPacket(hash, hash_len, resolved, snr, rssi, now);
  }
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
  sampleLinkStats(pkt);   // PCES link statistics (receiving only)
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 1) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTx(mesh::Packet *pkt, int len) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 0) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

mesh::DispatcherAction MyMesh::onRecvPacket(mesh::Packet* pkt) {
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  return Mesh::onRecvPacket(pkt);
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    data[len] = 0;  // ensure null terminator
    uint8_t reply_len;

    reply_path_len = 0xFF;
    if (data[4] == 0 || data[4] >= ' ') {   // is password, ie. a login request
      reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
    } else if (data[4] == ANON_REQ_TYPE_REGIONS && packet->isRouteDirect()) {
      reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_OWNER && packet->isRouteDirect()) {
      reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_BASIC && packet->isRouteDirect()) {
      reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_LINK_STATS && packet->isRouteDirect() && link_stats_on && len > 5) {
      reply_len = handleAnonLinkStatsReq(sender, timestamp, &data[5], len - 5);
    } else if (data[4] == ANON_REQ_TYPE_LOAD && packet->isRouteDirect() && load_on && len > 5) {
      reply_len = handleAnonLoadReq(sender, timestamp, &data[5], len - 5);
    } else {
      reply_len = 0;  // unknown/invalid request type
    }

    if (reply_len == 0) return;   // invalid request

    // a DIRECT login can reply via the stored out_path, as onPeerDataRecv() does for REQ
    ClientInfo* client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    bool have_out_path = client != NULL && client->out_path_len != OUT_PATH_UNKNOWN;

    auto route = mesh::chooseReplyRoute(packet->isRouteFlood(), reply_path_len != 0xFF, have_out_path);

    if (route == mesh::REPLY_ROUTE_PATH_RETURN) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
      if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      return;
    }

    mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
    if (reply == NULL) return;

    if (route == mesh::REPLY_ROUTE_DIRECT_SUPPLIED) {
      sendDirect(reply, reply_path, reply_path_len, SERVER_RESPONSE_DELAY);
    } else if (route == mesh::REPLY_ROUTE_DIRECT_OUT_PATH) {
      sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
    } else {
      sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    }
  }
}

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

static bool isShare(const mesh::Packet *packet) {
  if (packet->hasTransportCodes()) {
    return packet->transport_codes[0] == 0 && packet->transport_codes[1] == 0;  // codes { 0, 0 } means 'send to nowhere'
  }
  return false;
}

void MyMesh::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                          const uint8_t *app_data, size_t app_data_len) {
  mesh::Mesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len); // chain to super impl

  // if this a zero hop advert (and not via 'Share'), add it to neighbours
  if (packet->getPathHashCount() == 0 && !isShare(packet)) {
    AdvertDataParser parser(app_data, app_data_len);
    if (parser.isValid() && parser.getType() == ADV_TYPE_REPEATER) { // just keep neigbouring Repeaters
      putNeighbour(id, timestamp, packet->getSNR());
    }
    // PCES link statistics: repeaters and room servers only, never companions (that would make a
    // public list of the phones this repeater hears).
    bool is_relay = parser.getType() == ADV_TYPE_REPEATER || parser.getType() == ADV_TYPE_ROOM;
    if (link_stats_on && parser.isValid() && is_relay) {
      link_stats.addAdvert(id.pub_key, packet->getSNR(), getRTCClock()->getCurrentTime());
    }
  }
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) { // request (from a Known admin client!)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    if (timestamp > client->last_timestamp) { // prevent replay attacks
      int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
      if (reply_len == 0) return; // invalid command

      client->last_timestamp = timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
        mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                              PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
        if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      } else {
        mesh::Packet *reply =
            createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
        if (reply) {
          if (client->out_path_len != OUT_PATH_UNKNOWN) { // we have an out_path, so send DIRECT
            sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
          } else {
            sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()) { // a CLI command
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA || flags == TXT_TYPE_CLI_COMMAND)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported text type received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      // len can be > original length, but 'text' will be padded with zeroes
      data[len] = 0; // need to make a C string again, with null terminator

      if (flags == TXT_TYPE_PLAIN) { // for legacy CLI, send Acks
        uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove
                           // to sender that we got it
        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);

        mesh::Packet *ack = createAck(ack_hash);
        if (ack) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());
          } else {
            sendDirect(ack, client->out_path, client->out_path_len, TXT_ACK_DELAY);
          }
        }
      }

      uint8_t temp[166];
      char *command = (char *)&data[5];
      char *reply = (char *)&temp[5];
      if (is_retry) {
        *reply = 0;
      } else {
        handleCommand(sender_timestamp, command, reply);
      }
      int text_len = strlen(reply);
      if (text_len > 0) {
        uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
        if (timestamp == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          timestamp++;
        }
        memcpy(temp, &timestamp, 4);        // mostly an extra blob to help make packet_hash unique
        temp[4] = (TXT_TYPE_CLI_DATA << 2); // NOTE: legacy was: TXT_TYPE_PLAIN

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(reply, CLI_REPLY_DELAY_MILLIS, packet->getPathHashSize());
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, CLI_REPLY_DELAY_MILLIS);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);

    // store a copy of path, for sendDirect()
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

#define CTL_TYPE_NODE_DISCOVER_REQ   0x80
#define CTL_TYPE_NODE_DISCOVER_RESP  0x90

void MyMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;    // just test upper 4 bits
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd && discover_limiter.allow(rtc_clock.getCurrentTime())
      && !isHiddenNode()   // this node wishes to NOT be discoverable
  ) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;
    memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since;
    if (packet->payload_len >= i+4) {   // optional since field
      memcpy(&since, &packet->payload[i], 4); i += 4;
    } else {
      since = 0;
    }

    if ((filter & (1 << ADV_TYPE_REPEATER)) != 0 && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;   // low 4-bits for node type
      data[1] = packet->_snr;   // let sender know the inbound SNR ( x 4)
      memcpy(&data[2], &tag, 4);     // include tag from request, for client to match to
      memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) {
        sendZeroHop(resp, getRetransmitDelay(resp)*4);  // apply random delay (widened x4), as multiple nodes can respond to this
      }
    }
  } else if (type == CTL_TYPE_NODE_DISCOVER_RESP && packet->payload_len >= 6) {
    uint8_t node_type = packet->payload[0] & 0x0F;
    if (node_type != ADV_TYPE_REPEATER) {
      return;
    }
    if (packet->payload_len < 6 + PUB_KEY_SIZE) {
      MESH_DEBUG_PRINTLN("onControlDataRecv: DISCOVER_RESP pubkey too short: %d", (uint32_t)packet->payload_len);
      return;
    }

    if (pending_discover_tag == 0 || millisHasNowPassed(pending_discover_until)) {
      pending_discover_tag = 0;
      return;
    }
    uint32_t tag;
    memcpy(&tag, &packet->payload[2], 4);
    if (tag != pending_discover_tag) {
      return;
    }

    mesh::Identity id(&packet->payload[6]);
    if (id.matches(self_id)) {
      return;
    }
    putNeighbour(id, rtc_clock.getCurrentTime(), packet->getSNR());
  }
}

void MyMesh::sendNodeDiscoverReq() {
  uint8_t data[10];
  data[0] = CTL_TYPE_NODE_DISCOVER_REQ; // prefix_only=0
  data[1] = (1 << ADV_TYPE_REPEATER);
  getRNG()->random(&data[2], 4); // tag
  memcpy(&pending_discover_tag, &data[2], 4);
  pending_discover_until = futureMillis(60000);
  uint32_t since = 0;
  memcpy(&data[6], &since, 4);

  auto pkt = createControlData(data, sizeof(data));
  if (pkt) {
    sendZeroHop(pkt);
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      region_map(key_store), temp_map(key_store),
      _cli(board, rtc, sensors, region_map, acl, &_prefs, this),
      telemetry(MAX_PACKET_PAYLOAD - 4),
      discover_limiter(4, 120),  // max 4 every 2 minutes
      anon_limiter(4, 180),  // max 4 every 3 minutes
      link_stats_on(true),
      load_on(true),
      next_busy_sample(0),
      last_load_tick(0)
#if defined(WITH_RS232_BRIDGE)
      , bridge(&_prefs, WITH_RS232_BRIDGE, _mgr, &rtc)
#endif
#if defined(WITH_ESPNOW_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  set_radio_at = revert_radio_at = 0;
  _logging = false;
  region_load_active = false;
  recv_pkt_region = NULL;

#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // defaults
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = 0.0f;   // turn off by default, was 10.0;
  _prefs.tx_delay_factor = 0.5f; // was 0.25f
  _prefs.direct_tx_delay_factor = 0.3f; // was 0.2
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = 1;        // default to 2 minutes for NEW installs
  _prefs.flood_advert_interval = 47; // 47 hours
  _prefs.flood_max = 64;
  _prefs.flood_max_unscoped = 64;
  _prefs.flood_max_advert = 8;
  _prefs.interference_threshold = 0; // disabled
  _prefs.cad_enabled = 0;            // hardware CAD before TX (off by default; 'set cad on')
  _prefs.loop_detect = LOOP_DETECT_MINIMAL;

  // bridge defaults
  _prefs.bridge_enabled = 1;    // enabled
  _prefs.bridge_delay   = 500;  // milliseconds
  _prefs.bridge_pkt_src = 0;    // logTx
  _prefs.bridge_baud = 115200;  // baud rate
  _prefs.bridge_channel = 1;    // channel 1

  StrHelper::strncpy(_prefs.bridge_secret, "LVSITANOS", sizeof(_prefs.bridge_secret));

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs.adc_multiplier = 0.0f; // 0.0f means use default board multiplier

#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN;
#else
  _prefs.rx_boosted_gain = 1; // enabled by default;
#endif
#endif
  _prefs.radio_fem_rxgain = 1;
  _prefs.radio_fem_txgain = 0;

  pending_discover_tag = 0;
  pending_discover_until = 0;

  memset(default_scope.key, 0, sizeof(default_scope.key));
}

void MyMesh::begin(FILESYSTEM *fs) {
  mesh::Mesh::begin();
  _fs = fs;
  // load persisted prefs
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);
  loadLinkStatsPref();
  last_load_tick = _ms->getMillis();
  // TODO: key_store.begin();
  region_map.load(_fs);

  // establish default-scope
  {
    RegionEntry* r = region_map.getDefaultRegion();
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    } else {
#ifdef DEFAULT_FLOOD_SCOPE_NAME
      r = region_map.findByName(DEFAULT_FLOOD_SCOPE_NAME);
      if (r == NULL) {
        r = region_map.putRegion(DEFAULT_FLOOD_SCOPE_NAME, 0);  // auto-create the default scope region
        if (r) { r->flags = 0; }   // Allow-flood
      }
      if (r) {
        region_map.setDefaultRegion(r);
        region_map.getTransportKeysFor(*r, &default_scope, 1);
      }
#endif
    }
  }

#if defined(WITH_BRIDGE)
  if (_prefs.bridge_enabled) {
    bridge.begin();
  }
#endif

  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);

  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  MESH_DEBUG_PRINTLN("RX Boosted Gain Mode: %s",
                     radio_driver.getRxBoostedGainMode() ? "Enabled" : "Disabled");

  board.attachDynamicPrefs(_prefs.getCustom());

  updateAdvertTimer();
  updateFloodAdvertTimer();

  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif
}

void MyMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;  // REVISIT: set to 'home' Region, for sender/return region?
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000); // give CLI reply some time to be sent back, before applying temp radio params
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;

  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000); // schedule when to revert radio params
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) { // schedule local advert timer
    next_local_advert = futureMillis(((uint32_t)_prefs.advert_interval) * 2 * 60 * 1000);
  } else {
    next_local_advert = 0; // stop the timer
  }
}

void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) { // schedule flood advert timer
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0; // stop the timer
  }
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(int8_t power_dbm) {
  radio_driver.setTxPower(power_dbm);
}

bool MyMesh::setRxBoostedGain(bool enable) {
  return radio_driver.setRxBoostedGainMode(enable);
}

#if defined(USE_LR2021)
bool MyMesh::configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) {
  return radio_driver.configSideDetectors(sideDetSFs, num, bw);
}
#endif

void MyMesh::formatNeighborsReply(char *reply) {
  char *dp = reply;

#if MAX_NEIGHBOURS
  // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = &neighbours[i];
    if (neighbour->heard_timestamp > 0) {
      sorted_neighbours[neighbours_count] = neighbour;
      neighbours_count++;
    }
  }

  // sort neighbours newest to oldest
  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp; // desc
  });

  for (int i = 0; i < neighbours_count && dp - reply < 134; i++) {
    NeighbourInfo *neighbour = sorted_neighbours[i];

    // add new line if not first item
    if (i > 0) *dp++ = '\n';

    char hex[10];
    // get 4 bytes of neighbour id as hex
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 4);

    // add next neighbour
    uint32_t secs_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
    sprintf(dp, "%s:%d:%d", hex, secs_ago, neighbour->snr);
    while (*dp)
      dp++; // find end of string
  }
#endif
  if (dp == reply) { // no neighbours, need empty response
    strcpy(dp, "-none-");
    dp += 6;
  }
  *dp = 0; // null terminator
}

void MyMesh::removeNeighbor(const uint8_t *pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    NeighbourInfo *neighbour = &neighbours[i];
    if (memcmp(neighbour->id.pub_key, pubkey, key_len) == 0) {
      neighbours[i] = NeighbourInfo(); // clear neighbour entry
    }
  }
#endif
}

void MyMesh::startRegionsLoad() {
  temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
  memset(load_stack, 0, sizeof(load_stack));
  load_stack[0] = &temp_map.getWildcard();
  region_load_active = true;
}

bool MyMesh::saveRegions() {
  return region_map.save(_fs);
}

void MyMesh::onDefaultRegionChanged(const RegionEntry* r) {
  if (r) {
    region_map.getTransportKeysFor(*r, &default_scope, 1);
  } else {
    memset(default_scope.key, 0, sizeof(default_scope.key));
  }
}

void MyMesh::formatStatsReply(char *reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MyMesh::formatRadioStatsReply(char *reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(),
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
      region_map = temp_map;  // copy over the temp instance as new current map
      region_load_active = false;

      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char *np = command;
      while (*np == ' ') np++;   // skip indent
      int indent = np - command;

      char *ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }  // set null terminator for end of name

      while (*ep && *ep != 'F') ep++;  // look for (optional) flags

      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

            load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

  while (*command == ' ') command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (strcmp(command, "get pces.linkstats") == 0) {
    sprintf(reply, "> %s", link_stats_on ? "on" : "off");
  } else if (memcmp(command, "set pces.linkstats ", 19) == 0) {
    bool on;
    if (parseOnOff(&command[19], &on)) {
      link_stats_on = on;
      saveLinkStatsPref();
      strcpy(reply, "OK - advert marker from the next advert");
    } else {
      strcpy(reply, "Err - on or off");
    }
  } else if (strcmp(command, "get pces.load") == 0) {
    sprintf(reply, "> %s", load_on ? "on" : "off");
  } else if (memcmp(command, "set pces.load ", 14) == 0) {
    bool on;
    if (parseOnOff(&command[14], &on)) {
      load_on = on;
      if (!load_on) load_stats.clear();
      saveLinkStatsPref();
      strcpy(reply, "OK - advert marker from the next advert");
    } else {
      strcpy(reply, "Err - on or off");
    }
  } else if (strcmp(command, "load clear") == 0) {
    load_stats.clear();
    strcpy(reply, "OK");
  } else if (strcmp(command, "load") == 0) {
    formatLoad(reply, sender_timestamp == 0);   // timestamp 0: typed on the serial console
  } else if (strcmp(command, "linkstats clear") == 0) {
    link_stats.clear();
    strcpy(reply, "OK");
  } else if (memcmp(command, "linkstats", 9) == 0 && (command[9] == 0 || command[9] == ' ')) {
    int offset = 0;
    if (command[9] == ' ') offset = atoi(&command[10]);
    formatLinkStats(reply, offset, sender_timestamp == 0);
  } else if (memcmp(command, "discover.neighbors", 18) == 0) {
    const char* sub = command + 18;
    while (*sub == ' ') sub++;
    if (*sub != 0) {
      strcpy(reply, "Err - discover.neighbors has no options");
    } else {
      sendNodeDiscoverReq();
      strcpy(reply, "OK - Discover sent");
    }
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

  mesh::Mesh::loop();
  sampleLoad();   // PCES channel load (sampling only, nothing sent)

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    uint32_t delay_millis = 0;
    if (pkt) sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_driver.setParams(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs);
    dirty_contacts_expiry = 0;
  }

  // update uptime
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
#if defined(WITH_BRIDGE)
  if (bridge.isRunning()) return true;  // bridge needs WiFi radio, can't sleep
#endif
  return _mgr->getOutboundTotal() > 0;
}
