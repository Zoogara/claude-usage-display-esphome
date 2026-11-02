#pragma once

#include <atomic>
#include <vector>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/http_request/http_request.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace claude_usage {

struct BreakdownRow {
  std::string name;
  float percent;
};

struct TokenStore {
  char refresh[320];
};

/// Polls Claude.ai subscription usage (5h session / 7d weekly / per-model
/// weekly) straight from Anthropic's OAuth usage endpoint. Sign-in is a
/// one-off PKCE flow: open `auth_url`, approve, paste the code into
/// submit_code(). The rotating refresh token is kept in flash.
class ClaudeUsage : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_http(http_request::HttpRequestComponent *h) { this->http_ = h; }
  void set_scoped_model(const std::string &m) { this->scoped_model_ = m; }
  void set_session_percent(sensor::Sensor *s) { this->session_percent_ = s; }
  void set_weekly_percent(sensor::Sensor *s) { this->weekly_percent_ = s; }
  void set_scoped_percent(sensor::Sensor *s) { this->scoped_percent_ = s; }
  void set_session_reset(text_sensor::TextSensor *s) { this->session_reset_ = s; }
  void set_weekly_reset(text_sensor::TextSensor *s) { this->weekly_reset_ = s; }
  void set_scoped_reset(text_sensor::TextSensor *s) { this->scoped_reset_ = s; }
  void set_scoped_name(text_sensor::TextSensor *s) { this->scoped_name_ = s; }
  void set_plan(text_sensor::TextSensor *s) { this->plan_ = s; }
  void set_status(text_sensor::TextSensor *s) { this->status_ = s; }
  void set_auth_url(text_sensor::TextSensor *s) { this->auth_url_ = s; }

  /// Queue the code Anthropic displays after approval ("code#state"). Safe to
  /// call from the web server task; the exchange runs in the main loop.
  void submit_code(const std::string &code);
  /// Weekly usage split by source (Claude Code / Chats / ...), as shares of the total.
  const std::vector<BreakdownRow> &breakdown() const { return this->breakdown_; }
  /// Forget the stored login and start a fresh sign-in.
  void sign_out();

 protected:
  int request_(const char *method, const std::string &url, const std::string &body,
            const std::vector<http_request::Header> &headers, std::string &out);
  bool refresh_access_();
  bool fetch_usage_();
  bool fetch_plan_();
  void exchange_code_(const std::string &raw);
  void begin_sign_in_();
  void save_refresh_(const std::string &token);
  bool store_tokens_(const std::string &body, bool exchange);
  void set_status_(const std::string &s);
  void publish_resets_();

  http_request::HttpRequestComponent *http_{nullptr};
  std::string scoped_model_;
  ESPPreferenceObject pref_;

  sensor::Sensor *session_percent_{nullptr}, *weekly_percent_{nullptr}, *scoped_percent_{nullptr};
  text_sensor::TextSensor *session_reset_{nullptr}, *weekly_reset_{nullptr}, *scoped_reset_{nullptr},
      *scoped_name_{nullptr}, *plan_{nullptr}, *status_{nullptr}, *auth_url_{nullptr};

  std::string refresh_token_, access_token_;
  uint32_t token_issued_ms_{0}, token_ttl_ms_{0};
  bool have_access_{false};
  bool have_plan_{false};
  uint32_t backoff_until_ms_{0};
  bool backing_off_{false};
  bool busy_{false};
  uint8_t refresh_fail_{0};
  std::vector<BreakdownRow> breakdown_;
  std::string pending_code_;
  std::atomic<bool> has_pending_{false};

  std::string verifier_, challenge_, state_;
  int64_t session_reset_at_{0}, weekly_reset_at_{0}, scoped_reset_at_{0};
};

}  // namespace claude_usage
}  // namespace esphome
