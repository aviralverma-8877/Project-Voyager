// L3: pure-JS SHA-256 against known vectors (hashlib reference values).
load("data_main/lib.js");
var L = VoyagerLib;

test("sha256('')", function () {
  assertEq(L.sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
});
test("sha256('abc')", function () {
  assertEq(L.sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
});
test("sha256 of 1000 x 'a'", function () {
  var s = ""; for (var i = 0; i < 1000; i++) s += "a";
  assertEq(L.sha256Hex(s), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
});
test("sha256 of 1000 chars a..z repeating", function () {
  var s = ""; for (var i = 0; i < 1000; i++) s += String.fromCharCode(97 + (i % 26));
  assertEq(L.sha256Hex(s), "915e53a44c18b19bb06ba5b3f5fcaf1dc4651e8404c63425cfc6174e74659d87");
});
test("sha256 of the 448-bit NIST vector (two blocks after padding)", function () {
  assertEq(L.sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
});
test("sha256 hashes UTF-8 bytes", function () {
  assertEq(L.sha256Hex("héllo €😀"),
    "5350f147c6ff4293fd01390ef97e6cdb1cf3ad691d5c47f77246262f59dd1003");
});
test("sha256 of a ~27 KB data URL finishes and is stable", function () {
  var s = "data:application/octet-stream;base64,";
  for (var i = 0; i < 27000; i++) s += "ABCD"[i % 4];
  var h1 = L.sha256Hex(s);
  assertTrue(/^[0-9a-f]{64}$/.test(h1));
  assertEq(L.sha256Hex(s), h1);
  assertTrue(L.sha256Hex(s + "A") !== h1);
});

done();
