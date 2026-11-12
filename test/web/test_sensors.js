// L3: sensor helpers in lib.js - packet parsing (including hostile input),
// reading formatting, config validation and the no-ack tx_result flag.
load("data_main/lib.js");
var L = VoyagerLib;

var PINS = { io: [13, 16, 17, 25], in: [13, 16, 17, 25, 34, 35], adc: [34, 35] };

function sensorMsg(name, data) {
  return JSON.stringify({
    name: name,
    mac: "AA:BB:CC:DD:EE:FF",
    data: JSON.stringify({ pack_type: "sensor", data: data }),
  });
}

test("type ids match the firmware and never move", function () {
  assertEq(L.SENSOR_TYPES.map(function (t) { return [t.id, t.key]; }), [
    [1, "dht11"], [2, "dht22"], [3, "ds18b20"], [4, "hcsr04"], [5, "analog"], [6, "digital"],
  ]);
  assertEq(L.sensorType(4).key, "hcsr04");
  assertEq(L.sensorType("analog").id, 5);
  assertEq(L.sensorType(99), null);
});

test("sensor packet parses into readings with text", function () {
  var m = L.parseLoraRx(sensorMsg("node1", [["room", 2, 23.5, 40.1], ["tank", 4, 120.4], ["light", 5, 2048], ["door", 6, 1]]));
  assertEq(m.packType, "sensor");
  assertEq(m.sender, "node1");
  assertEq(m.sensors.length, 4);
  assertEq(m.sensors[0].text, "23.5 °C, 40.1 %");
  assertEq(m.sensors[0].typeLabel, "DHT22");
  assertEq(m.sensors[1].text, "120.4 cm");
  assertEq(m.sensors[2].text, "2048 / 4095 (50 %)");
  assertEq(m.sensors[3].text, "1 (HIGH)");
});

test("failed read (nulls) is shown as failed, not as numbers", function () {
  var r = L.parseSensorReadings([["room", 1, null, null], ["room2", 2, 21.0, null]]);
  assertEq(r[0].ok, false);
  assertEq(r[0].text, "read failed");
  assertEq(r[1].ok, false);
});

test("unknown type id from newer firmware still lists its values", function () {
  var r = L.parseSensorReadings([["x", 42, 7, 8]]);
  assertEq(r[0].typeLabel, "type 42");
  assertEq(r[0].text, "7, 8");
});

test("malformed rows are dropped, hostile names stay plain text", function () {
  var evil = "<img src=x onerror=alert(1)>";
  var r = L.parseSensorReadings([null, "x", [], ["a"], [5, 1, 2], ["ok", "1", 3, 4], [evil, 3, 20.5], ["n", -1, 1], ["n", 1.5, 1]]);
  assertEq(r.length, 2);
  assertEq(r[0].typeLabel, "DHT11");
  assertEq(r[1].name.indexOf("<img"), 0); // unchanged text; the UI renders with .text()
  assertEq(L.parseSensorReadings("nope"), []);
  assertEq(L.parseSensorReadings({ a: 1 }), []);
  // Values that are not finite numbers never reach the formatter.
  assertEq(L.parseSensorReadings([["a", 3, "<b>"]])[0].text, "read failed");
});

test("local WS sensor_data uses the same inner JSON", function () {
  var inner = JSON.stringify({ pack_type: "sensor", data: [["room", 3, -5.2]] });
  assertEq(L.parseSensorData(inner)[0].text, "-5.2 °C");
  assertEq(L.parseSensorData("{bad"), []);
  assertEq(L.parseSensorData(JSON.stringify({ pack_type: "msg", data: "hi" })), []);
});

test("valid config is normalised for the device", function () {
  var v = L.validateSensorConfig(
    {
      enabled: true, interval: "60", ack: false,
      sensors: [
        { type: "dht22", name: "room", pin: "13", pin2: "" },
        { type: "hcsr04", name: "tank", pin: "16", pin2: "34" },
        { type: "analog", name: "light", pin: "35", pin2: "" },
      ],
    },
    PINS
  );
  assertEq(v.errors, []);
  assertEq(v.config, {
    enabled: true, interval: 60, ack: false,
    sensors: [
      { type: "dht22", name: "room", pin: 13, pin2: -1 },
      { type: "hcsr04", name: "tank", pin: 16, pin2: 34 },
      { type: "analog", name: "light", pin: 35, pin2: -1 },
    ],
  });
});

test("config errors: interval, names, pins, count, enabling with nothing", function () {
  function errs(cfg) { return L.validateSensorConfig(cfg, PINS).errors.length; }
  var one = [{ type: "dht11", name: "a", pin: 13 }];
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: one }), 0);
  assertEq(errs({ enabled: false, interval: 1, ack: false, sensors: one }), 1);
  assertEq(errs({ enabled: false, interval: 86401, ack: false, sensors: one }), 1);
  assertEq(errs({ enabled: false, interval: 2.5, ack: false, sensors: one }), 1);
  assertEq(errs({ enabled: false, interval: "", ack: false, sensors: one }), 1);
  assertEq(errs({ enabled: true, interval: 30, ack: false, sensors: [] }), 1);
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: [{ type: "bme280", name: "a", pin: 13 }] }), 1);
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: [{ type: "dht11", name: "a b", pin: 13 }] }), 1);
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: [{ type: "dht11", name: "a", pin: "" }] }), 1);
  // input-only pin for a sensor that must drive the line; non-ADC pin for analog
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: [{ type: "dht11", name: "a", pin: 34 }] }), 1);
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: [{ type: "analog", name: "a", pin: 13 }] }), 1);
  // ultrasonic without echo, and with trigger == echo
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: [{ type: "hcsr04", name: "a", pin: 13 }] }), 1);
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: [{ type: "hcsr04", name: "a", pin: 13, pin2: 13 }] }), 1);
  // duplicate name, duplicate pin
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: [{ type: "dht11", name: "a", pin: 13 }, { type: "dht22", name: "a", pin: 16 }] }), 1);
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: [{ type: "dht11", name: "a", pin: 13 }, { type: "dht22", name: "b", pin: 13 }] }), 1);
  var five = ["a", "b", "c", "d", "e"].map(function (n, i) { return { type: "digital", name: n, pin: [13, 16, 17, 25, 34][i] }; });
  assertEq(errs({ enabled: false, interval: 30, ack: false, sensors: five }), 1);
  assertEq(L.validateSensorConfig(null, PINS).ok, false);
});

test("tx_result carries the noack flag", function () {
  var timers = [];
  var t = L.createTxTracker({
    setTimeout: function (fn) { timers.push(fn); return timers.length; },
    clearTimeout: function () {},
  });
  var got = null;
  t.register(7, function (r) { got = r; });
  t.resolve({ response_type: "tx_result", id: 7, akn: 1, noack: true, username: "me" });
  assertEq(got.akn, 1);
  assertEq(got.noack, true);
  t.register(8, function (r) { got = r; });
  t.resolve({ response_type: "tx_result", id: 8, akn: 1, username: "me" });
  assertEq(got.noack, false);
});

done();
