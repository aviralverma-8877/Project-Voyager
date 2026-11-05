// L3: navbar WiFi label from the WIFI_DATA SSE payload (STA / AP / OFF / legacy).
load("data_main/lib.js");
var L = VoyagerLib;

test("STA shows signal quality", function () {
  assertEq(L.wifiLabel({ mode: "STA", rssi: -55 }), "90 %");
  assertEq(L.wifiLabel({ mode: "STA", rssi: "-100" }), "0 %");
  assertEq(L.wifiLabel({ mode: "STA", rssi: -20 }), "100 %");
});

test("legacy payload without mode still works", function () {
  assertEq(L.wifiLabel({ rssi: -60 }), "80 %");
});

test("AP shows the client count", function () {
  assertEq(L.wifiLabel({ mode: "AP", clients: 0 }), "AP, 0 clients");
  assertEq(L.wifiLabel({ mode: "AP", clients: 1 }), "AP, 1 client");
  assertEq(L.wifiLabel({ mode: "AP", clients: 2 }), "AP, 2 clients");
  assertEq(L.wifiLabel({ mode: "ap", clients: "3" }), "AP, 3 clients");
  assertEq(L.wifiLabel({ mode: "AP" }), "AP");
  assertEq(L.wifiLabel({ mode: "AP", clients: -1 }), "AP");
});

test("OFF", function () {
  assertEq(L.wifiLabel({ mode: "OFF" }), "off");
});

test("unusable payloads return null (keep the previous label)", function () {
  assertEq(L.wifiLabel(null), null);
  assertEq(L.wifiLabel("x"), null);
  assertEq(L.wifiLabel([]), null);
  assertEq(L.wifiLabel({}), null);
  assertEq(L.wifiLabel({ mode: "STA" }), null);
  assertEq(L.wifiLabel({ mode: "STA", rssi: "abc" }), null);
  assertEq(L.wifiLabel({ mode: "<img src=x>", rssi: -50 }), null);
});

test("signal bars: STA level from quality, AP full, OFF empty", function () {
  assertEq(L.wifiBars({ mode: "STA", rssi: -55 }), { mode: "sta", level: 4 }); // 90 %
  assertEq(L.wifiBars({ mode: "STA", rssi: -70 }), { mode: "sta", level: 3 }); // 60 %
  assertEq(L.wifiBars({ mode: "STA", rssi: -80 }), { mode: "sta", level: 2 }); // 40 %
  assertEq(L.wifiBars({ mode: "STA", rssi: -95 }), { mode: "sta", level: 1 }); // 10 %
  assertEq(L.wifiBars({ mode: "STA", rssi: -100 }), { mode: "sta", level: 0 });
  assertEq(L.wifiBars({ rssi: -66 }), { mode: "sta", level: 3 }); // legacy, 68 %
  assertEq(L.wifiBars({ mode: "AP", clients: 0 }), { mode: "ap", level: 4 });
  assertEq(L.wifiBars({ mode: "OFF" }), { mode: "off", level: 0 });
});

test("signal bars: unusable payloads return null", function () {
  assertEq(L.wifiBars(null), null);
  assertEq(L.wifiBars({}), null);
  assertEq(L.wifiBars({ mode: "STA" }), null);
  assertEq(L.wifiBars({ mode: "MESH", rssi: -50 }), null);
});

done();
