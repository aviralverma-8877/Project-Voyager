// Device admin authentication (Contract 2). Owner: web-api-engineer.
//
// HTTP Digest, realm "Voyager", user "admin". NVS (Preferences "voyager", key "ha1")
// stores HA1 = md5("admin:Voyager:<password>"), never the password. A successful
// GET /api/session also issues a "vsid" session cookie (128-bit, RAM only, max 4).
// Nothing here logs the password or HA1.
#ifndef AUTH_H
#define AUTH_H
#include <Arduino.h>

class AsyncWebServerRequest;

#define AUTH_REALM "Voyager"
#define AUTH_USER  "admin"
#define AUTH_SESSION_COOKIE "vsid"

void auth_init();                          // loads HA1 from NVS (also done lazily)
bool auth_is_set();                        // any task
bool auth_set(const char* password);       // 8-63 chars; clears sessions. Any task.
void auth_clear();                         // removes HA1 + sessions (factory reset). Any task.

// Valid vsid cookie or valid Digest credentials. False when no password is set.
bool auth_check(AsyncWebServerRequest* request);
// auth_check, or replies itself (403 {"error":"setup_required"} / 401 Digest challenge)
// and returns false.
bool auth_require(AsyncWebServerRequest* request);

// Issues a new session token (32 lowercase hex chars + NUL). False if no password is set.
bool auth_session_create(char out[33]);
void auth_session_revoke(const char* token);
#endif
