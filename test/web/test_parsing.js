// L3: parsing of device/radio messages, including hostile HTML. Parsed values
// must come through as plain text (the UI renders them with .text()), and
// escapeHtml must neutralise them if they ever go into markup.
load("data_main/lib.js");
var L = VoyagerLib;

var XSS = [
  "<img src=x onerror=alert(1)>",
  "<script>fetch('/update_flash')</script>",
  "\"><svg onload=alert(1)>",
  "'); alert(1); ('",
  "&lt;b&gt;already-escaped",
];

function loraMsg(name, inner) {
  return JSON.stringify({ name: name, mac: "AA:BB:CC:DD:EE:FF", data: JSON.stringify(inner) });
}

test("escapeHtml escapes every HTML metacharacter", function () {
  assertEq(L.escapeHtml("<img src=x onerror=alert(1)>"), "&lt;img src=x onerror=alert(1)&gt;");
  assertEq(L.escapeHtml("\"'&"), "&quot;&#39;&amp;");
  XSS.forEach(function (s) {
    var e = L.escapeHtml(s);
    assertTrue(!/[<>"']/.test(e), "no raw metachars in " + e);
  });
  assertEq(L.escapeHtml(null), "");
  assertEq(L.escapeHtml(5), "5");
});

test("text message: malicious name and text pass through as text", function () {
  XSS.forEach(function (s) {
    var m = L.parseLoraRx(loraMsg(s, { pack_type: "msg", data: s }));
    assertEq(m.packType, "msg");
    assertEq(m.sender, s, "sender verbatim");
    assertEq(m.text, s, "text verbatim");
    assertEq(typeof m.text, "string");
  });
});

test("non-string fields are coerced to strings, never objects", function () {
  var m = L.parseLoraRx(loraMsg({ toString: 1 }, { pack_type: "msg", data: { html: "<b>" } }));
  assertEq(typeof m.sender, "string");
  assertEq(typeof m.text, "string");
  assertEq(m.text, '{"html":"<b>"}');
});

test("empty name falls back to MAC", function () {
  var m = L.parseLoraRx(loraMsg("", { pack_type: "msg", data: "hi" }));
  assertEq(m.sender, "AA:BB:CC:DD:EE:FF");
});

test("SOS action", function () {
  var m = L.parseLoraRx(loraMsg(XSS[0], { pack_type: "action", data: "sos" }));
  assertEq(m.action, "sos");
  assertEq(m.sender, XSS[0]);
});

test("file_tx_invite with hostile file name/size/tx_id passes through as text", function () {
  var m = L.parseLoraRx(loraMsg("bob", {
    pack_type: "action", data: "file_tx_invite", name: XSS[2], size: XSS[1], tx_id: XSS[0],
  }));
  assertEq(m.action, "file_tx_invite");
  assertEq(m.fileName, XSS[2]);
  assertEq(m.size, XSS[1]);
  assertEq(m.txId, XSS[0]);
});

test("net_status: hostile peer fields pass through as text, numbers sanitised", function () {
  var s = L.parseNetStatus({ active: true, ip: "10.73.241.92", tx: 5, rx: "7", drop: -3,
    peers: [{ ip: XSS[0], rssi: "-71", age_s: 4 }, "junk", { ip: "10.73.232.236", rssi: "x" }] });
  assertEq(s.active, true);
  assertEq([s.tx, s.rx, s.drop], [5, 7, 0]);
  assertEq(s.peers.length, 2);
  assertEq(s.peers[0], { ip: XSS[0], rssi: -71, age: 4 });
  assertEq(s.peers[1].rssi, 0);
  assertEq(L.parseNetStatus({ active: true }), null);
  assertEq(L.parseNetStatus("nope"), null);
});

test("malformed lora_rx payloads return null instead of throwing", function () {
  [undefined, null, "", "not json", "[]", "{}", '{"name":"a","data":"not json"}',
   '{"name":"a","data":"{\\"pack_type\\":\\"other\\"}"}'].forEach(function (raw) {
    assertEq(L.parseLoraRx(raw), null, String(raw));
  });
});

test("wifi scan: malicious SSIDs pass through as text", function () {
  var list = L.parseWifiScan({ SSID: [
    { ssid: "<img src=x onerror=alert(1)>", rssi: -40 },
    { ssid: "');alert(1);('", rssi: -95 },
    { ssid: 123, rssi: "x" },
    "junk",
  ] });
  assertEq(list.length, 3);
  assertEq(list[0].ssid, "<img src=x onerror=alert(1)>");
  assertEq(list[0].quality, 100);
  assertEq(list[0].cls, "list-group-item-success");
  assertEq(list[1].ssid, "');alert(1);('");
  assertEq(list[1].cls, "list-group-item-danger");
  assertEq(list[2].ssid, "123");
  assertEq(list[2].quality, 0);
  assertEq(L.parseWifiScan({}), []);
  assertEq(L.parseWifiScan(null), []);
});

test("wifi quality clamps to 0..100", function () {
  assertEq(L.wifiQuality(-100), 0);
  assertEq(L.wifiQuality(-120), 0);
  assertEq(L.wifiQuality(-50), 100);
  assertEq(L.wifiQuality(-10), 100);
  assertEq(L.wifiQuality(-70), 60);
});

test("DEBUG SSE lines: data passes through as text", function () {
  XSS.forEach(function (s) {
    var ev = L.parseSseData(JSON.stringify({ millis: 42, data: s }));
    assertEq(ev.millis, 42);
    assertEq(ev.data, s);
  });
  assertEq(L.parseSseData("garbage"), null);
  assertEq(L.parseSseData(JSON.stringify({ millis: 1, data: { a: 1 } })).data, '{"a":1}');
});

test("heap thresholds use <=", function () {
  assertEq(L.heapStatus(70, 100).cls, "bg-success", "30% -> success");
  assertEq(L.heapStatus(69, 100).cls, "bg-warning", "31%");
  assertEq(L.heapStatus(30, 100).cls, "bg-warning", "70% -> warning");
  assertEq(L.heapStatus(29, 100).cls, "bg-danger", "71%");
  assertEq(L.heapStatus(1, 0), null);
});

test("config name allowlist", function () {
  assertTrue(L.isAllowedConfigName("wifi_config.json"));
  assertTrue(!L.isAllowedConfigName("user_data.json"));
  assertTrue(!L.isAllowedConfigName("../lora_config.json"));
  assertTrue(L.isLoraPreset("long_lora_config.json"));
  assertTrue(!L.isLoraPreset("wifi_config.json"));
});

test("password validation", function () {
  assertTrue(!L.validatePassword("1234567"));
  assertTrue(L.validatePassword("12345678"));
  assertTrue(L.validatePassword(new Array(64).join("a")), "63 chars");
  assertTrue(!L.validatePassword(new Array(65).join("a")), "64 chars");
  assertTrue(L.validateWifiPassword(""), "open network");
  assertTrue(!L.validateWifiPassword("short"));
  assertTrue(L.validateApPassword("goodpass"));
  assertTrue(!L.validateApPassword("cafécafé"), "non-ASCII rejected");
  assertTrue(L.validateUsername("a"));
  assertTrue(!L.validateUsername(""));
  assertTrue(!L.validateUsername(new Array(34).join("a")), "33 chars");
});

done();
