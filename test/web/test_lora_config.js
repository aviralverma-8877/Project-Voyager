// L3: LoRa config validation (Contract 5 ranges).
load("data_main/lib.js");
var L = VoyagerLib;

function base() {
  return { freq: 510000000, TxPower: 10, SpreadingFactor: 8, SignalBandwidth: 500000, CodingRate4: 7, SyncWord: 52 };
}
function withKey(k, v) { var c = base(); c[k] = v; return c; }

test("shipped default config is valid", function () {
  var v = L.validateLoraConfig(base());
  assertTrue(v.ok, v.errors.join("; "));
  assertEq(v.config, base());
});

test("freq range 410-525 MHz inclusive", function () {
  assertTrue(L.validateLoraConfig(withKey("freq", 410000000)).ok);
  assertTrue(L.validateLoraConfig(withKey("freq", 525000000)).ok);
  assertTrue(!L.validateLoraConfig(withKey("freq", 409999999)).ok);
  assertTrue(!L.validateLoraConfig(withKey("freq", 525000001)).ok);
  assertTrue(!L.validateLoraConfig(withKey("freq", 868000000)).ok);
  assertTrue(!L.validateLoraConfig(withKey("freq", 433.5)).ok, "MHz value rejected");
});

test("TxPower 2-20", function () {
  assertTrue(L.validateLoraConfig(withKey("TxPower", 2)).ok);
  assertTrue(L.validateLoraConfig(withKey("TxPower", 20)).ok);
  assertTrue(!L.validateLoraConfig(withKey("TxPower", 1)).ok);
  assertTrue(!L.validateLoraConfig(withKey("TxPower", 21)).ok);
});

test("SpreadingFactor 6-12", function () {
  assertTrue(L.validateLoraConfig(withKey("SpreadingFactor", 6)).ok);
  assertTrue(L.validateLoraConfig(withKey("SpreadingFactor", 12)).ok);
  assertTrue(!L.validateLoraConfig(withKey("SpreadingFactor", 5)).ok);
  assertTrue(!L.validateLoraConfig(withKey("SpreadingFactor", 13)).ok);
});

test("SignalBandwidth must be an SX127x value", function () {
  L.LORA_BANDWIDTHS.forEach(function (bw) {
    assertTrue(L.validateLoraConfig(withKey("SignalBandwidth", bw)).ok, String(bw));
  });
  assertTrue(!L.validateLoraConfig(withKey("SignalBandwidth", 100000)).ok);
  assertTrue(!L.validateLoraConfig(withKey("SignalBandwidth", 0)).ok);
});

test("CodingRate4 5-8", function () {
  assertTrue(L.validateLoraConfig(withKey("CodingRate4", 5)).ok);
  assertTrue(L.validateLoraConfig(withKey("CodingRate4", 8)).ok);
  assertTrue(!L.validateLoraConfig(withKey("CodingRate4", 4)).ok);
  assertTrue(!L.validateLoraConfig(withKey("CodingRate4", 9)).ok);
});

test("SyncWord 0-255", function () {
  assertTrue(L.validateLoraConfig(withKey("SyncWord", 0)).ok);
  assertTrue(L.validateLoraConfig(withKey("SyncWord", 255)).ok);
  assertTrue(!L.validateLoraConfig(withKey("SyncWord", 256)).ok);
  assertTrue(!L.validateLoraConfig(withKey("SyncWord", -1)).ok);
});

test("missing / non-numeric / hostile values rejected", function () {
  var c = base(); delete c.SyncWord;
  assertTrue(!L.validateLoraConfig(c).ok, "missing key");
  assertTrue(!L.validateLoraConfig(withKey("TxPower", "<script>")).ok);
  assertTrue(!L.validateLoraConfig(withKey("TxPower", "")).ok);
  assertTrue(!L.validateLoraConfig(withKey("TxPower", null)).ok);
  assertTrue(!L.validateLoraConfig(null).ok);
  assertTrue(!L.validateLoraConfig([]).ok);
  assertTrue(!L.validateLoraConfig("x").ok);
});

test("numeric strings (from a loaded file) are normalised to numbers", function () {
  var v = L.validateLoraConfig({ freq: "510000000", TxPower: "10", SpreadingFactor: "8",
    SignalBandwidth: "125000", CodingRate4: "7", SyncWord: "52" });
  assertTrue(v.ok);
  assertEq(v.config.SignalBandwidth, 125000);
});

test("extra keys are dropped from the config sent to the device", function () {
  var c = base(); c.evil = "<b>";
  var v = L.validateLoraConfig(c);
  assertTrue(v.ok);
  assertEq(Object.keys(v.config).sort(), Object.keys(base()).sort());
});

test("form values -> config (MHz to Hz, slider endpoints)", function () {
  var f = { freqMHz: "433.5", txPower: "17", spreadingFactor: "9", bandwidth: "125000", codingRate: "5", syncWord: "18" };
  var c = L.loraConfigFromForm(f);
  assertEq(c, { freq: 433500000, TxPower: 17, SpreadingFactor: 9, SignalBandwidth: 125000, CodingRate4: 5, SyncWord: 18 });
  assertTrue(L.validateLoraConfig(c).ok);
  f.freqMHz = "410"; assertTrue(L.validateLoraConfig(L.loraConfigFromForm(f)).ok, "slider min");
  f.freqMHz = "525"; assertTrue(L.validateLoraConfig(L.loraConfigFromForm(f)).ok, "slider max");
});

done();
