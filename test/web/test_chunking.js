// L3: chunking, packet count (Math.ceil), size formatting, data URL decoding.
load("data_main/lib.js");
var L = VoyagerLib;

function str(n) { var s = ""; for (var i = 0; i < n; i++) s += String.fromCharCode(65 + (i % 26)); return s; }

test("chunk size validation accepts 1..200 integers only", function () {
  assertTrue(L.validateChunkSize(1), "1");
  assertTrue(L.validateChunkSize(200), "200");
  assertTrue(L.validateChunkSize("150"), "'150'");
  assertTrue(!L.validateChunkSize(0), "0");
  assertTrue(!L.validateChunkSize(201), "201");
  assertTrue(!L.validateChunkSize(-5), "-5");
  assertTrue(!L.validateChunkSize(1.5), "1.5");
  assertTrue(!L.validateChunkSize(""), "empty");
  assertTrue(!L.validateChunkSize("abc"), "abc");
});

test("chunkString: length 1, size 200 -> 1 chunk", function () {
  assertEq(L.chunkString("A", 200), ["A"]);
  assertEq(L.totalPackets(1, 200), 1);
});

test("chunkString: length 200, size 200 -> 1 chunk", function () {
  var c = L.chunkString(str(200), 200);
  assertEq(c.length, 1);
  assertEq(c[0].length, 200);
  assertEq(L.totalPackets(200, 200), 1);
});

test("chunkString: length 201, size 200 -> 2 chunks, last has 1 char (Math.ceil)", function () {
  var s = str(201);
  var c = L.chunkString(s, 200);
  assertEq(c.length, 2);
  assertEq(c[1].length, 1);
  assertEq(L.totalPackets(201, 200), 2);
  assertEq(c.join(""), s, "reassembles");
});

test("chunkString: empty string -> no chunks, 0 packets", function () {
  assertEq(L.chunkString("", 200), []);
  assertEq(L.totalPackets(0, 200), 0);
});

test("chunkString: count always equals totalPackets", function () {
  [1, 7, 199, 200, 201, 399, 400, 401, 1000].forEach(function (n) {
    [1, 3, 100, 200].forEach(function (size) {
      assertEq(L.chunkString(str(n), size).length, L.totalPackets(n, size), n + "/" + size);
    });
  });
});

test("chunkString rejects invalid chunk size", function () {
  var threw = false;
  try { L.chunkString("abc", 0); } catch (e) { threw = true; }
  assertTrue(threw, "0 throws");
  threw = false;
  try { L.chunkString("abc", 201); } catch (e) { threw = true; }
  assertTrue(threw, "201 throws");
});

test("formatSize uses 1024", function () {
  assertEq(L.formatSize(0), "0 Bytes");
  assertEq(L.formatSize(1023), "1023 Bytes");
  assertEq(L.formatSize(1024), "1.00 KB");
  assertEq(L.formatSize(20480), "20.00 KB");
  assertEq(L.formatSize(1024 * 1024), "1.00 MB");
  assertEq(L.formatSize(1536 * 1024), "1.50 MB");
});

test("get_string_size counts UTF-8 bytes", function () {
  assertEq(L.get_string_size("abc"), "3 Bytes");
  assertEq(L.get_string_size("é"), "2 Bytes");
  assertEq(L.get_string_size("€"), "3 Bytes");
  assertEq(L.get_string_size("😀"), "4 Bytes");
  assertEq(L.get_string_size(str(2048)), "2.00 KB");
});

test("formatDuration", function () {
  assertEq(L.formatDuration(0), "0:00:00");
  assertEq(L.formatDuration(61), "0:01:01");
  assertEq(L.formatDuration(3725), "1:02:05");
});

test("dataURIToBytes decodes base64 data URLs", function () {
  var d = L.dataURIToBytes("data:text/plain;base64,SGVsbG8sIFdvcmxkIQ==");
  assertEq(d.mime, "text/plain");
  assertEq(String.fromCharCode.apply(null, Array.prototype.slice.call(d.bytes)), "Hello, World!");
  var e = L.dataURIToBytes("data:application/octet-stream;base64,");
  assertEq(e.bytes.length, 0, "empty file");
  var b = L.dataURIToBytes("data:application/octet-stream;base64,AP8=");
  assertEq(Array.prototype.slice.call(b.bytes), [0, 255]);
});

test("dataURIToBytes rejects malformed input", function () {
  ["", "hello", "data:text/plain;base64,@@@", "data:text/plain;base64,A"].forEach(function (bad) {
    var threw = false;
    try { L.dataURIToBytes(bad); } catch (e) { threw = true; }
    assertTrue(threw, JSON.stringify(bad));
  });
});

done();
