// L3: wrapped LORA_MSG payload size (Contract 1: <= 251 bytes).
load("data_main/lib.js");
var L = VoyagerLib;

function rep(ch, n) { var s = ""; for (var i = 0; i < n; i++) s += ch; return s; }

test("limit is 251", function () {
  assertEq(L.LORA_MAX_PAYLOAD, 251);
});

test("wrapped size matches the wrapper format exactly", function () {
  var inner = L.buildTextMessage("hi");
  var expected = JSON.stringify({ name: "bob", mac: "XX:XX:XX:XX:XX:XX", data: inner }).length;
  assertEq(L.wrappedPayloadSize("bob", inner), expected);
});

test("empty username is counted as the 17-char MAC", function () {
  var inner = L.buildTextMessage("hi");
  assertEq(L.wrappedPayloadSize("", inner), L.wrappedPayloadSize("XX:XX:XX:XX:XX:XX", inner));
});

test("boundary: largest ASCII text fits, one more byte does not", function () {
  var n = 0;
  while (L.checkPayload("bob", L.buildTextMessage(rep("a", n + 1))).ok) n++;
  var fit = L.checkPayload("bob", L.buildTextMessage(rep("a", n)));
  var over = L.checkPayload("bob", L.buildTextMessage(rep("a", n + 1)));
  assertTrue(fit.ok, "n fits");
  assertEq(fit.size, 251, "exactly at the limit");
  assertTrue(!over.ok, "n+1 rejected");
  assertEq(over.size, 252);
});

test("quotes are double-escaped and counted", function () {
  // " inside text -> \" in inner JSON -> \\\" in the wrapper: 4 bytes each.
  var a = L.wrappedPayloadSize("bob", L.buildTextMessage("a"));
  var q = L.wrappedPayloadSize("bob", L.buildTextMessage('"'));
  assertEq(q - a, 3);
});

test("multi-byte UTF-8 counts bytes, not characters", function () {
  var a = L.wrappedPayloadSize("bob", L.buildTextMessage("a"));
  assertEq(L.wrappedPayloadSize("bob", L.buildTextMessage("€")) - a, 2, "euro = 3 bytes");
  assertEq(L.wrappedPayloadSize("bob", L.buildTextMessage("😀")) - a, 3, "emoji = 4 bytes");
  // 100 euro signs: 300 bytes of text, must be rejected even though length is 100.
  assertTrue(!L.checkPayload("bob", L.buildTextMessage(rep("€", 100))).ok);
});

test("longer usernames shrink the room for text", function () {
  var t = L.buildTextMessage(rep("a", 150));
  assertTrue(L.checkPayload("a", t).ok);
  assertTrue(!L.checkPayload(rep("n", 32), t).ok);
});

done();
