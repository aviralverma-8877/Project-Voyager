// L3: YMODEM protocol - CRC-16, block building, block parsing
load("data_main/ymodem.js");
var Y = YMODEM;

/* ---------- CRC-16 tests ---------- */

test("crc16: '123456789' produces 0x31C3", function () {
  var data = new Uint8Array([0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39]);
  assertEq(Y.crc16(data), 0x31c3);
});

test("crc16: empty data produces 0x0000", function () {
  assertEq(Y.crc16(new Uint8Array(0)), 0x0000);
});

test("crc16: single 0x00 byte", function () {
  // CRC of single 0x00: after XOR with 0, 8 shifts with no polynomial XOR
  assertEq(Y.crc16(new Uint8Array([0x00])), 0x0000);
});

test("crc16: single 0xFF byte produces 0x1EF0", function () {
  // Known test vector
  assertEq(Y.crc16(new Uint8Array([0xff])), 0x1ef0);
});

test("crc16: 128 bytes of 0x1A (SUB padding)", function () {
  var data = new Uint8Array(128);
  for (var i = 0; i < 128; i++) data[i] = 0x1a;
  // Just verify it doesn't throw and returns a 16-bit value
  var crc = Y.crc16(data);
  assertTrue(crc >= 0 && crc <= 0xffff, "CRC in valid range");
});

/* ---------- Block building tests ---------- */

test("buildBlock0: creates 133-byte block with SOH header", function () {
  var block = Y.buildBlock0("test.txt", 1234);
  assertEq(block.length, 133); // SOH + num + ~num + 128 data + 2 CRC
  assertEq(block[0], Y.SOH);
  assertEq(block[1], 0x00); // block number 0
  assertEq(block[2], 0xff); // ~0 = 255
});

test("buildBlock0: filename and size encoded correctly", function () {
  var block = Y.buildBlock0("hello.bin", 5678);
  // Data starts at offset 3
  var data = block.slice(3, 3 + 128);
  // "hello.bin" + NUL + "5678" + NUL
  var expected = "hello.bin";
  for (var i = 0; i < expected.length; i++) {
    assertEq(data[i], expected.charCodeAt(i), "filename char " + i);
  }
  assertEq(data[9], 0, "NUL after filename");
  assertEq(String.fromCharCode(data[10], data[11], data[12], data[13]), "5678", "size");
  assertEq(data[14], 0, "NUL after size");
});

test("buildEmptyBlock0: creates batch end marker", function () {
  var block = Y.buildEmptyBlock0();
  assertEq(block.length, 133);
  assertEq(block[0], Y.SOH);
  assertEq(block[1], 0x00);
  assertEq(block[2], 0xff);
  // Data should be all zeros
  var allZero = true;
  for (var i = 3; i < 3 + 128; i++) {
    if (block[i] !== 0) allZero = false;
  }
  assertTrue(allZero, "data is all zeros");
});

test("buildBlock: STX block is 1029 bytes", function () {
  var data = new Uint8Array(1024);
  var block = Y.buildBlock(Y.STX, 1, data);
  assertEq(block.length, 1029); // STX + num + ~num + 1024 data + 2 CRC
  assertEq(block[0], Y.STX);
  assertEq(block[1], 1);
  assertEq(block[2], 0xfe); // ~1
});

test("buildBlock: SOH block is 133 bytes", function () {
  var data = new Uint8Array(128);
  var block = Y.buildBlock(Y.SOH, 5, data);
  assertEq(block.length, 133);
  assertEq(block[0], Y.SOH);
  assertEq(block[1], 5);
  assertEq(block[2], 0xfa); // ~5
});

test("buildBlock: block number wraps at 256", function () {
  var data = new Uint8Array(128);
  var block = Y.buildBlock(Y.SOH, 256, data);
  assertEq(block[1], 0); // 256 & 0xFF = 0
  assertEq(block[2], 0xff); // ~0
});

test("buildBlock: CRC is appended correctly", function () {
  var data = new Uint8Array(128);
  for (var i = 0; i < 128; i++) data[i] = i;
  var block = Y.buildBlock(Y.SOH, 1, data);
  var expectedCrc = Y.crc16(data);
  var blockCrc = (block[131] << 8) | block[132];
  assertEq(blockCrc, expectedCrc, "CRC matches");
});

/* ---------- Block parsing tests ---------- */

test("parseBlock0: extracts filename and size", function () {
  var block = Y.buildBlock0("myfile.dat", 9999);
  var data = block.slice(3, 3 + 128);
  var info = Y.parseBlock0(data);
  assertEq(info.filename, "myfile.dat");
  assertEq(info.size, 9999);
  assertEq(info.batchEnd, false);
});

test("parseBlock0: empty filename is batch end", function () {
  var block = Y.buildEmptyBlock0();
  var data = block.slice(3, 3 + 128);
  var info = Y.parseBlock0(data);
  assertEq(info.filename, "");
  assertEq(info.batchEnd, true);
});

test("parseBlock0: handles large file sizes", function () {
  var block = Y.buildBlock0("large.bin", 123456789);
  var data = block.slice(3, 3 + 128);
  var info = Y.parseBlock0(data);
  assertEq(info.filename, "large.bin");
  assertEq(info.size, 123456789);
});

test("parseBlock0: handles spaces in filename", function () {
  var block = Y.buildBlock0("my file.txt", 100);
  var data = block.slice(3, 3 + 128);
  var info = Y.parseBlock0(data);
  assertEq(info.filename, "my file.txt");
  assertEq(info.size, 100);
});

/* ---------- Utility function tests ---------- */

test("toUint8Array: converts string", function () {
  var arr = Y.toUint8Array("ABC");
  assertEq(arr.length, 3);
  assertEq(arr[0], 65);
  assertEq(arr[1], 66);
  assertEq(arr[2], 67);
});

test("toUint8Array: passes through Uint8Array", function () {
  var orig = new Uint8Array([1, 2, 3]);
  var result = Y.toUint8Array(orig);
  assertTrue(result === orig, "same reference");
});

test("toUint8Array: converts array", function () {
  var arr = Y.toUint8Array([10, 20, 30]);
  assertTrue(arr instanceof Uint8Array, "is Uint8Array");
  assertEq(Array.prototype.slice.call(arr), [10, 20, 30]);
});

test("padBlock: pads short data with zeros", function () {
  var padded = Y.padBlock(new Uint8Array([1, 2, 3]), 128);
  assertEq(padded.length, 128);
  assertEq(padded[0], 1);
  assertEq(padded[1], 2);
  assertEq(padded[2], 3);
  assertEq(padded[3], 0);
  assertEq(padded[127], 0);
});

test("padBlock: truncates long data", function () {
  var long = new Uint8Array(200);
  for (var i = 0; i < 200; i++) long[i] = i;
  var padded = Y.padBlock(long, 128);
  assertEq(padded.length, 128);
  assertEq(padded[127], 127);
});

/* ---------- Constants tests ---------- */

test("protocol constants are correct", function () {
  assertEq(Y.SOH, 0x01);
  assertEq(Y.STX, 0x02);
  assertEq(Y.EOT, 0x04);
  assertEq(Y.ACK, 0x06);
  assertEq(Y.NAK, 0x15);
  assertEq(Y.CAN, 0x18);
  assertEq(Y.C, 0x43);
  assertEq(Y.BLOCK_SIZE_SOH, 128);
  assertEq(Y.BLOCK_SIZE_STX, 1024);
});

/* ---------- Round-trip tests ---------- */

test("block 0 round-trip: build then parse", function () {
  var names = ["a.txt", "test_file_with_long_name.bin", "x", "file.tar.gz"];
  var sizes = [0, 1, 127, 128, 1000, 1048576, 999999999];
  for (var i = 0; i < names.length; i++) {
    for (var j = 0; j < sizes.length; j++) {
      var block = Y.buildBlock0(names[i], sizes[j]);
      var data = block.slice(3, 3 + 128);
      var info = Y.parseBlock0(data);
      assertEq(info.filename, names[i], names[i] + "/" + sizes[j] + " filename");
      assertEq(info.size, sizes[j], names[i] + "/" + sizes[j] + " size");
    }
  }
});

test("CRC verification: built block has valid CRC", function () {
  var data = new Uint8Array(128);
  for (var i = 0; i < 128; i++) data[i] = Math.floor(Math.random() * 256);
  var block = Y.buildBlock(Y.SOH, 42, data);
  // Extract and verify CRC
  var blockData = block.slice(3, 3 + 128);
  var expectedCrc = Y.crc16(blockData);
  var actualCrc = (block[131] << 8) | block[132];
  assertEq(actualCrc, expectedCrc);
});

done();
