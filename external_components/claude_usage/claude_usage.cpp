#include "claude_usage.h"

#include <cmath>
#include <ctime>

#include "esp_random.h"
#include "mbedtls/sha256.h"

#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace claude_usage {

static const char *const TAG = "claude_usage";

// Claude Code's public OAuth client, as used by other community integrations.
static const char *const CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static const char *const TOKEN_URL = "https://console.anthropic.com/v1/oauth/token";
static const char *const REDIRECT_URI = "https://console.anthropic.com/oauth/code/callback";
static const char *const USAGE_URL = "https://api.anthropic.com/api/oauth/usage";
static const char *const PROFILE_URL = "https://api.anthropic.com/api/oauth/profile";
static const char *const BETA = "oauth-2025-04-20";
static const size_t MAX_BODY = 16384;
static const uint32_t RATE_LIMIT_BACKOFF_MS = 30 * 60 * 1000;

static std::string b64url(const uint8_t *d, size_t n) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string o;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = d[i] << 16;
    if (i + 1 < n) v |= d[i + 1] << 8;
    if (i + 2 < n) v |= d[i + 2];
    o += T[(v >> 18) & 63];
    o += T[(v >> 12) & 63];
    if (i + 1 < n) o += T[(v >> 6) & 63];
    if (i + 2 < n) o += T[v & 63];
  }
  return o;
}

static std::string rand_b64url(size_t bytes) {
  uint8_t r[32];
  esp_fill_random(r, bytes);
  return b64url(r, bytes);
}

// "2026-11-02T14:00:00.123+00:00" / "...Z" -> unix seconds (0 on failure).
static int64_t parse_iso(const char *s) {
  int Y, M, D, h, m;
  float sec;
  if (s == nullptr || sscanf(s, "%d-%d-%dT%d:%d:%f", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  int y = Y - (M <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t) era * 146097 + doe - 719468;
  int64_t t = days * 86400 + h * 3600 + m * 60 + (int) sec;
  const char *z = strchr(s, 'T');
  const char *sign = z ? strpbrk(z, "+-") : nullptr;
  if (sign) {
    int oh = 0, om = 0;
    if (sscanf(sign + 1, "%d:%d", &oh, &om) >= 1) {
      int64_t off = oh * 3600 + om * 60;
      t += (*sign == '+') ? -off : off;
    }
  }
  return t;
}

static bool time_valid() { return time(nullptr) > 1700000000; }

static std::string fmt_countdown(int64_t at) {
  if (at == 0 || !time_valid()) return "";
  int64_t d = at - time(nullptr);
  if (d <= 0) return "now";
  char b[24];
  if (d >= 86400)
    snprintf(b, sizeof b, "%dd %dh", (int) (d / 86400), (int) (d % 86400 / 3600));
  else
    snprintf(b, sizeof b, "%dh %02dm", (int) (d / 3600), (int) (d % 3600 / 60));
  return b;
}

static std::string fmt_clock(int64_t at, const char *f) {
  if (at == 0 || !time_valid()) return "";
  time_t t = (time_t) at;
  struct tm tm;
  localtime_r(&t, &tm);
  char b[24];
  strftime(b, sizeof b, f, &tm);
  return b;
}

void ClaudeUsage::setup() {
  this->pref_ = global_preferences->make_preference<TokenStore>(fnv1_hash("claude_usage_token"));
  TokenStore ts{};
  if (this->pref_.load(&ts)) {
    ts.refresh[sizeof ts.refresh - 1] = 0;
    this->refresh_token_ = ts.refresh;
  }
  this->set_interval("countdown", 30000, [this]() { this->publish_resets_(); });
}

void ClaudeUsage::dump_config() {
  ESP_LOGCONFIG(TAG, "Claude Usage:");
  ESP_LOGCONFIG(TAG, "  Signed in: %s", this->refresh_token_.empty() ? "no" : "yes");
  ESP_LOGCONFIG(TAG, "  Scoped model filter: '%s'", this->scoped_model_.c_str());
  LOG_UPDATE_INTERVAL(this);
}

void ClaudeUsage::set_status_(const std::string &s) {
  ESP_LOGI(TAG, "Status: %s", s.c_str());
  if (this->status_ != nullptr) this->status_->publish_state(s);
}

void ClaudeUsage::save_refresh_(const std::string &token) {
  this->refresh_token_ = token;
  TokenStore ts{};
  strncpy(ts.refresh, token.c_str(), sizeof ts.refresh - 1);
  this->pref_.save(&ts);
  global_preferences->sync();
}

void ClaudeUsage::begin_sign_in_() {
  if (this->verifier_.empty()) {
    this->verifier_ = rand_b64url(32);
    uint8_t h[32];
    mbedtls_sha256((const uint8_t *) this->verifier_.c_str(), this->verifier_.size(), h, 0);
    this->challenge_ = b64url(h, 32);
    this->state_ = rand_b64url(24);
  }
  if (this->auth_url_ != nullptr) {
    this->auth_url_->publish_state(
        std::string("https://claude.ai/oauth/authorize?code=true&client_id=") + CLIENT_ID +
        "&response_type=code&redirect_uri=https%3A%2F%2Fconsole.anthropic.com%2Foauth%2Fcode%2Fcallback"
        "&scope=user%3Aprofile&code_challenge=" + this->challenge_ + "&code_challenge_method=S256&state=" +
        this->state_);
  }
  this->set_status_("Sign-in needed");
}

int ClaudeUsage::request_(const char *method, const std::string &url, const std::string &body,
                       const std::vector<http_request::Header> &headers, std::string &out) {
  out.clear();
  auto c = this->http_->start(url, method, body, headers);
  if (c == nullptr) return -1;
  int status = c->status_code;
  size_t len = c->content_length;
  bool known = len > 0 && len < MAX_BODY;
  uint8_t buf[512];
  while (out.size() < MAX_BODY && (!known || c->get_bytes_read() < len)) {
    int n = c->read(buf, sizeof buf);
    App.feed_wdt();
    if (n <= 0) break;
    out.append((char *) buf, n);
  }
  c->end();
  return status;
}

// Parses an OAuth token response and keeps the (rotated) refresh token.
bool ClaudeUsage::store_tokens_(const std::string &body, bool exchange) {
  std::string access, refresh;
  uint32_t ttl = 3600;
  bool ok = json::parse_json(body, [&](JsonObject root) -> bool {
    if (root["access_token"].isNull()) return false;
    access = root["access_token"].as<std::string>();
    if (!root["refresh_token"].isNull()) refresh = root["refresh_token"].as<std::string>();
    if (!root["expires_in"].isNull()) ttl = root["expires_in"].as<uint32_t>();
    return true;
  });
  if (!ok) return false;
  this->access_token_ = access;
  this->token_issued_ms_ = millis();
  this->token_ttl_ms_ = (ttl > 120 ? ttl - 60 : ttl) * 1000;
  this->have_access_ = true;
  if (!refresh.empty()) this->save_refresh_(refresh);
  (void) exchange;
  return true;
}

void ClaudeUsage::submit_code(const std::string &raw) {
  // May run on the web server's httpd task, whose stack is too small for TLS.
  if (raw.empty()) return;
  this->pending_code_ = raw;
  this->has_pending_.store(true);
}

void ClaudeUsage::loop() {
  if (!this->has_pending_.exchange(false)) return;
  std::string code = this->pending_code_;
  this->exchange_code_(code);
}

void ClaudeUsage::exchange_code_(const std::string &raw) {
  std::string code = raw;
  while (!code.empty() && isspace((unsigned char) code.back())) code.pop_back();
  while (!code.empty() && isspace((unsigned char) code.front())) code.erase(code.begin());
  if (code.empty() || this->verifier_.empty() || this->busy_) return;
  std::string state = this->state_;
  size_t hash = code.find('#');
  if (hash != std::string::npos) {
    state = code.substr(hash + 1);
    code = code.substr(0, hash);
  }
  if (state != this->state_) {
    this->set_status_("Code does not match this sign-in");
    return;
  }
  this->busy_ = true;
  this->set_status_("Exchanging code...");
  std::string body = json::build_json([&](JsonObject r) {
    r["grant_type"] = "authorization_code";
    r["code"] = code;
    r["state"] = state;
    r["client_id"] = CLIENT_ID;
    r["redirect_uri"] = REDIRECT_URI;
    r["code_verifier"] = this->verifier_;
  });
  std::string resp;
  int s = this->request_("POST", TOKEN_URL, body, {{"Content-Type", "application/json"}}, resp);
  this->busy_ = false;
  if (s == 200 && this->store_tokens_(resp, true)) {
    this->verifier_.clear();
    this->refresh_fail_ = 0;
    if (this->auth_url_ != nullptr) this->auth_url_->publish_state("");
    this->set_status_("Signed in");
    this->set_timeout("first_fetch", 100, [this]() { this->update(); });
  } else {
    ESP_LOGW(TAG, "Code exchange failed: HTTP %d", s);
    this->set_status_("Code rejected (HTTP " + std::to_string(s) + ") - try again");
  }
}

void ClaudeUsage::sign_out() {
  this->save_refresh_("");
  this->access_token_.clear();
  this->have_access_ = false;
  this->verifier_.clear();
  this->begin_sign_in_();
}

bool ClaudeUsage::refresh_access_() {
  if (this->have_access_ && millis() - this->token_issued_ms_ < this->token_ttl_ms_) return true;
  if (this->refresh_token_.empty()) return false;
  std::string body = json::build_json([&](JsonObject r) {
    r["grant_type"] = "refresh_token";
    r["refresh_token"] = this->refresh_token_;
    r["client_id"] = CLIENT_ID;
  });
  std::string resp;
  int s = this->request_("POST", TOKEN_URL, body, {{"Content-Type", "application/json"}}, resp);
  if (s == 200 && this->store_tokens_(resp, false)) {
    this->refresh_fail_ = 0;
    return true;
  }
  this->have_access_ = false;
  if (s == 400 || s == 401 || s == 403) {
    ESP_LOGW(TAG, "Refresh token rejected (HTTP %d)", s);
    this->save_refresh_("");
    this->begin_sign_in_();
  } else {
    ESP_LOGW(TAG, "Token refresh failed: HTTP %d (%u in a row)", s, (unsigned) this->refresh_fail_ + 1);
    // Keep the stored token (the failure may be transient) but, after a few
    // misses, offer a fresh sign-in so no manual logout is needed.
    if (++this->refresh_fail_ >= 3)
      this->begin_sign_in_();
    else
      this->set_status_("Token refresh failed (HTTP " + std::to_string(s) + ")");
  }
  return false;
}

bool ClaudeUsage::fetch_plan_() {
  std::string resp;
  int s = this->request_("GET", PROFILE_URL, "",
                      {{"Authorization", "Bearer " + this->access_token_}, {"anthropic-beta", BETA}}, resp);
  if (s != 200) return false;
  std::string plan;
  json::parse_json(resp, [&](JsonObject root) -> bool {
    // Field names vary; take the first of the likely ones.
    for (const char *k : {"subscription_level", "plan", "rate_limit_tier"}) {
      if (root[k].is<const char *>()) { plan = root[k].as<std::string>(); return true; }
      if (root["organization"][k].is<const char *>()) { plan = root["organization"][k].as<std::string>(); return true; }
      if (root["account"][k].is<const char *>()) { plan = root["account"][k].as<std::string>(); return true; }
    }
    return true;
  });
  if (this->plan_ != nullptr && !plan.empty()) this->plan_->publish_state(plan);
  return true;
}

static bool icontains(const std::string &h, const std::string &n) {
  if (n.empty()) return true;
  std::string a = h, b = n;
  for (auto &c : a) c = tolower((unsigned char) c);
  for (auto &c : b) c = tolower((unsigned char) c);
  return a.find(b) != std::string::npos;
}

bool ClaudeUsage::fetch_usage_() {
  std::string resp;
  int s = this->request_("GET", USAGE_URL, "",
                      {{"Authorization", "Bearer " + this->access_token_}, {"anthropic-beta", BETA}}, resp);
  if (s == 429) {
    this->backoff_until_ms_ = millis() + RATE_LIMIT_BACKOFF_MS;
    this->backing_off_ = true;
    this->set_status_("Rate limited - retrying in 30 min");
    return false;
  }
  if (s == 401 || s == 403) {
    this->have_access_ = false;  // force a refresh next time
    this->set_status_("Auth rejected (HTTP " + std::to_string(s) + ")");
    return false;
  }
  if (s != 200) {
    this->set_status_("Usage fetch failed (HTTP " + std::to_string(s) + ")");
    return false;
  }
  // Log the whole payload in chunks (the logger truncates long lines).
  for (size_t i = 0; i < resp.size() && i < 4000; i += 300)
    ESP_LOGD(TAG, "usage[%u]: %s", (unsigned) (i / 300), resp.substr(i, 300).c_str());

  float sess = NAN, week = NAN, scoped = NAN;
  int64_t sess_r = 0, week_r = 0, scoped_r = 0;
  std::string scoped_name;
  std::vector<BreakdownRow> rows;
  bool ok = json::parse_json(resp, [&](JsonObject root) -> bool {
    JsonArray brk = root["seven_day_breakdown"]["rows"].as<JsonArray>();
    for (JsonVariant v : brk) {
      JsonObject e = v.as<JsonObject>();
      if (e["percent"].isNull()) continue;
      std::string name = e["display_name"] | (e["key"] | "?");
      rows.push_back({name, e["percent"].as<float>()});
    }
    // The limits[] array is authoritative; the flat five_hour/seven_day keys
    // are a fallback for older payloads.
    JsonArray limits = root["limits"].as<JsonArray>();
    for (JsonVariant v : limits) {
      JsonObject e = v.as<JsonObject>();
      std::string kind = e["kind"] | "";
      if (e["percent"].isNull()) continue;
      float pct = e["percent"].as<float>();
      int64_t at = parse_iso(e["resets_at"] | (const char *) nullptr);
      if (kind == "session" && std::isnan(sess)) {
        sess = pct; sess_r = at;
      } else if (kind == "weekly_all" && std::isnan(week)) {
        week = pct; week_r = at;
      } else if (kind == "weekly_scoped" && std::isnan(scoped)) {
        std::string name = e["scope"]["model"]["display_name"] | "";
        if (icontains(name, this->scoped_model_)) {
          scoped = pct; scoped_r = at; scoped_name = name;
        }
      }
    }
    // Fallback: flat seven_day_<model> keys (e.g. seven_day_sonnet) when no
    // matching limits[] bucket exists. Null entries are skipped.
    if (std::isnan(scoped)) {
      for (JsonPair kv : root) {
        std::string key = kv.key().c_str();
        if (key.rfind("seven_day_", 0) != 0 || !kv.value().is<JsonObject>()) continue;
        JsonObject o = kv.value().as<JsonObject>();
        std::string model = key.substr(10);
        if (o["utilization"].isNull() || !icontains(model, this->scoped_model_)) continue;
        scoped = o["utilization"].as<float>();
        scoped_r = parse_iso(o["resets_at"] | (const char *) nullptr);
        if (!model.empty()) model[0] = toupper((unsigned char) model[0]);
        scoped_name = model;
        break;
      }
    }
    if (std::isnan(sess) && !root["five_hour"]["utilization"].isNull()) {
      sess = root["five_hour"]["utilization"].as<float>();
      sess_r = parse_iso(root["five_hour"]["resets_at"] | (const char *) nullptr);
    }
    if (std::isnan(week) && !root["seven_day"]["utilization"].isNull()) {
      week = root["seven_day"]["utilization"].as<float>();
      week_r = parse_iso(root["seven_day"]["resets_at"] | (const char *) nullptr);
    }
    return true;
  });
  if (!ok) {
    this->set_status_("Usage JSON parse failed");
    return false;
  }

  this->breakdown_ = rows;
  if (this->session_percent_ != nullptr) this->session_percent_->publish_state(sess);
  if (this->weekly_percent_ != nullptr) this->weekly_percent_->publish_state(week);
  if (this->scoped_percent_ != nullptr) this->scoped_percent_->publish_state(scoped);
  if (this->scoped_name_ != nullptr) this->scoped_name_->publish_state(scoped_name);
  this->session_reset_at_ = sess_r;
  this->weekly_reset_at_ = week_r;
  this->scoped_reset_at_ = scoped_r;
  this->publish_resets_();
  this->set_status_("OK");
  return true;
}

void ClaudeUsage::publish_resets_() {
  if (this->session_reset_ != nullptr) this->session_reset_->publish_state(fmt_countdown(this->session_reset_at_));
  if (this->weekly_reset_ != nullptr)
    this->weekly_reset_->publish_state(fmt_clock(this->weekly_reset_at_, "%a %H:%M"));
  if (this->scoped_reset_ != nullptr)
    this->scoped_reset_->publish_state(fmt_clock(this->scoped_reset_at_, "%a %H:%M"));
}

void ClaudeUsage::update() {
  if (this->busy_) return;
  if (this->backing_off_) {
    if ((int32_t) (millis() - this->backoff_until_ms_) < 0) return;
    this->backing_off_ = false;
  }
  if (this->refresh_token_.empty()) {
    this->begin_sign_in_();
    return;
  }
  if (!network::is_connected() || !time_valid()) {
    // Boot race: the first poll fires before Wi-Fi is up and the clock is
    // synced (TLS needs a valid date, and a request started too early can
    // block long enough to trip the watchdog). Retry shortly rather than
    // failing and waiting a whole update interval.
    this->set_timeout("net_retry", 5000, [this]() { this->update(); });
    return;
  }
  this->busy_ = true;
  if (this->refresh_access_()) {
    this->fetch_usage_();
    if (!this->have_plan_) this->have_plan_ = this->fetch_plan_();
  }
  this->busy_ = false;
}

}  // namespace claude_usage
}  // namespace esphome
