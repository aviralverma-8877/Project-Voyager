#include "auth.h"
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <MD5Builder.h>
#include <esp_random.h>
#include "web_validate.h"

#define AUTH_NVS_NS  "voyager"
#define AUTH_NVS_KEY "ha1"
#define AUTH_MAX_SESSIONS 4

static portMUX_TYPE auth_mux = portMUX_INITIALIZER_UNLOCKED;
static char g_ha1[33] = {0};          // "" = no password set
static volatile bool g_loaded = false;
static char g_sessions[AUTH_MAX_SESSIONS][33] = {{0}};
static uint8_t g_next_slot = 0;

static void clear_sessions_locked() {
  memset(g_sessions, 0, sizeof(g_sessions));
  g_next_slot = 0;
}

void auth_init() {
  char buf[33] = {0};
  Preferences prefs;
  if (prefs.begin(AUTH_NVS_NS, true)) {
    if (prefs.isKey(AUTH_NVS_KEY)) {
      size_t n = prefs.getString(AUTH_NVS_KEY, buf, sizeof(buf));
      if (n != 33 && strlen(buf) != 32) buf[0] = '\0';
    }
    prefs.end();
  }
  if (strlen(buf) != 32) buf[0] = '\0';
  portENTER_CRITICAL(&auth_mux);
  memcpy(g_ha1, buf, sizeof(g_ha1));
  g_loaded = true;
  portEXIT_CRITICAL(&auth_mux);
  memset(buf, 0, sizeof(buf));
}

static void ensure_loaded() {
  if (!g_loaded) auth_init();
}

static void copy_ha1(char out[33]) {
  ensure_loaded();
  portENTER_CRITICAL(&auth_mux);
  memcpy(out, g_ha1, 33);
  portEXIT_CRITICAL(&auth_mux);
}

bool auth_is_set() {
  ensure_loaded();
  portENTER_CRITICAL(&auth_mux);
  bool set = g_ha1[0] != '\0';
  portEXIT_CRITICAL(&auth_mux);
  return set;
}

bool auth_set(const char* password) {
  if (!wv::password_valid(password)) return false;
  MD5Builder md5;
  md5.begin();
  md5.add(AUTH_USER ":" AUTH_REALM ":");
  md5.add(password);
  md5.calculate();
  char ha1[33] = {0};
  md5.getChars(ha1);

  Preferences prefs;
  bool ok = prefs.begin(AUTH_NVS_NS, false);
  if (ok) {
    ok = prefs.putString(AUTH_NVS_KEY, ha1) == 32;
    prefs.end();
  }
  if (ok) {
    portENTER_CRITICAL(&auth_mux);
    memcpy(g_ha1, ha1, sizeof(g_ha1));
    g_loaded = true;
    clear_sessions_locked();
    portEXIT_CRITICAL(&auth_mux);
  }
  memset(ha1, 0, sizeof(ha1));
  return ok;
}

void auth_clear() {
  Preferences prefs;
  if (prefs.begin(AUTH_NVS_NS, false)) {
    prefs.remove(AUTH_NVS_KEY);
    prefs.end();
  }
  portENTER_CRITICAL(&auth_mux);
  memset(g_ha1, 0, sizeof(g_ha1));
  g_loaded = true;
  clear_sessions_locked();
  portEXIT_CRITICAL(&auth_mux);
}

bool auth_session_create(char out[33]) {
  if (!auth_is_set()) return false;
  static const char hex[] = "0123456789abcdef";
  for (int w = 0; w < 4; w++) {
    uint32_t r = esp_random();
    for (int i = 0; i < 8; i++) {
      out[w * 8 + i] = hex[r & 0xF];
      r >>= 4;
    }
  }
  out[32] = '\0';
  portENTER_CRITICAL(&auth_mux);
  memcpy(g_sessions[g_next_slot], out, 33);   // oldest token is evicted
  g_next_slot = (g_next_slot + 1) % AUTH_MAX_SESSIONS;
  portEXIT_CRITICAL(&auth_mux);
  return true;
}

static bool session_valid(const char* token) {
  if (!wv::is_session_token(token, strlen(token))) return false;
  bool found = false;
  portENTER_CRITICAL(&auth_mux);
  for (int i = 0; i < AUTH_MAX_SESSIONS; i++)
    if (g_sessions[i][0] && wv::ct_equal(g_sessions[i], token, 32)) found = true;
  portEXIT_CRITICAL(&auth_mux);
  return found;
}

void auth_session_revoke(const char* token) {
  if (token == nullptr || strlen(token) != 32) return;
  portENTER_CRITICAL(&auth_mux);
  for (int i = 0; i < AUTH_MAX_SESSIONS; i++)
    if (g_sessions[i][0] && wv::ct_equal(g_sessions[i], token, 32)) memset(g_sessions[i], 0, 33);
  portEXIT_CRITICAL(&auth_mux);
}

// Hardware test builds only (env esp_lora_v2_test): every request counts as signed
// in once a password exists, so test tools can drive the API and OTA without it.
#if defined(VOYAGER_TEST_NOAUTH) && VOYAGER_TEST_NOAUTH
#if !DEBUG
#error "VOYAGER_TEST_NOAUTH must never be combined with a release build"
#endif
#warning "VOYAGER_TEST_NOAUTH: web authentication is disabled in this build"
#endif

bool auth_check(AsyncWebServerRequest* request) {
  if (request == nullptr || !auth_is_set()) return false;
#if defined(VOYAGER_TEST_NOAUTH) && VOYAGER_TEST_NOAUTH
  return true;
#endif

  const AsyncWebHeader* cookie = request->getHeader("Cookie");
  if (cookie) {
    char tok[40];
    if (wv::cookie_get(cookie->value().c_str(), AUTH_SESSION_COOKIE, tok, sizeof(tok)) &&
        session_valid(tok))
      return true;
  }

  // Only Digest. authenticate() with passwordIsHash compares a *Basic* header with
  // the stored hash verbatim, which must not be an accepted credential.
  const AsyncWebHeader* authz = request->getHeader("Authorization");
  if (authz == nullptr) return false;
  const String& v = authz->value();
  if (v.length() < 7 || !v.substring(0, 6).equalsIgnoreCase("Digest")) return false;

  char ha1[33];
  copy_ha1(ha1);
  bool ok = ha1[0] != '\0' && request->authenticate(AUTH_USER, ha1, AUTH_REALM, true);
  memset(ha1, 0, sizeof(ha1));
  return ok;
}

bool auth_require(AsyncWebServerRequest* request) {
  if (!auth_is_set()) {
    request->send(403, "application/json", "{\"error\":\"setup_required\"}");
    return false;
  }
  if (auth_check(request)) return true;
  request->requestAuthentication(AUTH_REALM, true);
  return false;
}
