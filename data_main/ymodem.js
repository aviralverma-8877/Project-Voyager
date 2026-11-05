/*
 * Project Voyager - YMODEM protocol for Web Serial API.
 *
 * Implements YMODEM batch file transfer with CRC-16 for reliable serial
 * communication over LoRa links (browser -> ESP32 -> LoRa -> ESP32 -> browser).
 *
 * No DOM, no external dependencies. Uses Uint8Array throughout for binary data.
 * Loaded by index.html; exposes a single global: YMODEM.
 */
var YMODEM = (function () {
  "use strict";

  /* ---------- Protocol constants ---------- */

  var SOH = 0x01; // Start of 128-byte block
  var STX = 0x02; // Start of 1024-byte block
  var EOT = 0x04; // End of transmission
  var ACK = 0x06; // Acknowledge
  var NAK = 0x15; // Negative acknowledge
  var CAN = 0x18; // Cancel transfer
  var C = 0x43; // 'C' - Request CRC mode

  var BLOCK_SIZE_SOH = 128; // SOH block data size
  var BLOCK_SIZE_STX = 1024; // STX block data size

  var DEFAULT_TIMEOUT_MS = 30000; // Long timeout for LoRa latency
  var MAX_RETRIES = 10; // Maximum retries before failure
  var MAX_CONSECUTIVE_NAK = 3; // Consecutive NAKs before abort

  /* ---------- CRC-16 (XMODEM polynomial 0x1021) ---------- */

  function crc16(data) {
    var crc = 0;
    for (var i = 0; i < data.length; i++) {
      crc ^= data[i] << 8;
      for (var j = 0; j < 8; j++) {
        if (crc & 0x8000) {
          crc = ((crc << 1) ^ 0x1021) & 0xffff;
        } else {
          crc = (crc << 1) & 0xffff;
        }
      }
    }
    return crc & 0xffff;
  }

  /* ---------- Helper functions ---------- */

  function toUint8Array(data) {
    if (data instanceof Uint8Array) return data;
    if (Array.isArray(data)) return new Uint8Array(data);
    if (typeof data === "string") {
      var arr = new Uint8Array(data.length);
      for (var i = 0; i < data.length; i++) {
        arr[i] = data.charCodeAt(i) & 0xff;
      }
      return arr;
    }
    return new Uint8Array(0);
  }

  function padBlock(data, size) {
    var arr = toUint8Array(data);
    if (arr.length >= size) return arr.slice(0, size);
    var padded = new Uint8Array(size);
    padded.set(arr);
    // Remaining bytes are 0x00 by default (SUB padding not needed for YMODEM)
    return padded;
  }

  function concatUint8Arrays(arrays) {
    var totalLen = 0;
    for (var i = 0; i < arrays.length; i++) {
      totalLen += arrays[i].length;
    }
    var result = new Uint8Array(totalLen);
    var offset = 0;
    for (var j = 0; j < arrays.length; j++) {
      result.set(arrays[j], offset);
      offset += arrays[j].length;
    }
    return result;
  }

  // Build YMODEM block: header + blockNum + ~blockNum + data + CRC16
  function buildBlock(header, blockNum, data) {
    var blockSize = header === SOH ? BLOCK_SIZE_SOH : BLOCK_SIZE_STX;
    var paddedData = padBlock(data, blockSize);
    var crc = crc16(paddedData);

    var block = new Uint8Array(3 + blockSize + 2);
    block[0] = header;
    block[1] = blockNum & 0xff;
    block[2] = (~blockNum) & 0xff;
    block.set(paddedData, 3);
    block[3 + blockSize] = (crc >> 8) & 0xff;
    block[3 + blockSize + 1] = crc & 0xff;

    return block;
  }

  // Build block 0 (filename block): filename\0 + filesize\0 + padding
  function buildBlock0(filename, filesize) {
    var nameBytes = [];
    for (var i = 0; i < filename.length; i++) {
      nameBytes.push(filename.charCodeAt(i) & 0xff);
    }
    nameBytes.push(0); // NUL terminator

    var sizeStr = String(filesize);
    for (var j = 0; j < sizeStr.length; j++) {
      nameBytes.push(sizeStr.charCodeAt(j) & 0xff);
    }
    nameBytes.push(0); // NUL terminator

    return buildBlock(SOH, 0, new Uint8Array(nameBytes));
  }

  // Build empty block 0 (batch end marker)
  function buildEmptyBlock0() {
    return buildBlock(SOH, 0, new Uint8Array(BLOCK_SIZE_SOH));
  }

  // Parse block 0 to extract filename and size
  function parseBlock0(data) {
    if (data.length < BLOCK_SIZE_SOH) return null;

    // Find filename (NUL-terminated)
    var nameEnd = 0;
    while (nameEnd < data.length && data[nameEnd] !== 0) {
      nameEnd++;
    }
    if (nameEnd === 0) {
      // Empty filename = batch end
      return { filename: "", size: 0, batchEnd: true };
    }

    var filename = "";
    for (var i = 0; i < nameEnd; i++) {
      filename += String.fromCharCode(data[i]);
    }

    // Find filesize (after the NUL, also NUL-terminated or space-terminated)
    var sizeStart = nameEnd + 1;
    var sizeEnd = sizeStart;
    while (sizeEnd < data.length && data[sizeEnd] !== 0 && data[sizeEnd] !== 0x20) {
      sizeEnd++;
    }

    var sizeStr = "";
    for (var j = sizeStart; j < sizeEnd; j++) {
      sizeStr += String.fromCharCode(data[j]);
    }

    var size = parseInt(sizeStr, 10);
    if (isNaN(size)) size = 0;

    return { filename: filename, size: size, batchEnd: false };
  }

  /* ---------- State machine for parsing incoming data ---------- */

  function createParser() {
    var buffer = [];
    var callbacks = [];

    return {
      // Add incoming bytes to buffer
      feed: function (bytes) {
        var arr = toUint8Array(bytes);
        for (var i = 0; i < arr.length; i++) {
          buffer.push(arr[i]);
        }
        this._process();
      },

      // Register callback for when specific byte(s) are received
      expect: function (expected, timeout, callback) {
        var expectedArr = Array.isArray(expected) ? expected : [expected];
        var timer = null;
        var self = this;

        var entry = {
          expected: expectedArr,
          callback: callback,
          timer: null,
          resolved: false,
        };

        if (timeout > 0) {
          entry.timer = setTimeout(function () {
            if (!entry.resolved) {
              entry.resolved = true;
              var idx = callbacks.indexOf(entry);
              if (idx >= 0) callbacks.splice(idx, 1);
              callback(null, true); // null byte, timeout=true
            }
          }, timeout);
        }

        callbacks.push(entry);
        this._process();
      },

      // Expect a full block and return parsed data
      expectBlock: function (timeout, callback) {
        var self = this;
        var accum = [];
        var header = null;
        var expectedSize = 0;

        function waitForHeader() {
          self.expect([SOH, STX, EOT, CAN], timeout, function (byte, timedOut) {
            if (timedOut) {
              callback(null, "timeout waiting for block header");
              return;
            }
            if (byte === EOT) {
              callback({ type: "eot" }, null);
              return;
            }
            if (byte === CAN) {
              // Check for double CAN
              self.expect([CAN], 1000, function (byte2, timedOut2) {
                if (!timedOut2 && byte2 === CAN) {
                  callback({ type: "cancel" }, null);
                } else {
                  // Single CAN, treat as cancel anyway
                  callback({ type: "cancel" }, null);
                }
              });
              return;
            }
            header = byte;
            expectedSize = byte === SOH ? BLOCK_SIZE_SOH : BLOCK_SIZE_STX;
            waitForBlockData();
          });
        }

        function waitForBlockData() {
          // Need: blockNum(1) + ~blockNum(1) + data(128/1024) + crc(2)
          var needed = 2 + expectedSize + 2;
          self._collectBytes(needed, timeout, function (data, err) {
            if (err) {
              callback(null, err);
              return;
            }

            var blockNum = data[0];
            var blockNumComp = data[1];
            var blockData = data.slice(2, 2 + expectedSize);
            var crcHi = data[2 + expectedSize];
            var crcLo = data[2 + expectedSize + 1];
            var receivedCrc = (crcHi << 8) | crcLo;

            // Verify block number complement
            if (((blockNum + blockNumComp) & 0xff) !== 0xff) {
              callback(null, "block number complement mismatch");
              return;
            }

            // Verify CRC
            var calculatedCrc = crc16(blockData);
            if (calculatedCrc !== receivedCrc) {
              callback(null, "crc mismatch");
              return;
            }

            callback(
              {
                type: "data",
                header: header,
                blockNum: blockNum,
                data: blockData,
              },
              null
            );
          });
        }

        waitForHeader();
      },

      // Internal: collect N bytes
      _collectBytes: function (count, timeout, callback) {
        var collected = [];
        var self = this;
        var timedOut = false;
        var timer = null;

        if (timeout > 0) {
          timer = setTimeout(function () {
            timedOut = true;
            callback(null, "timeout collecting bytes");
          }, timeout);
        }

        function collect() {
          if (timedOut) return;

          while (buffer.length > 0 && collected.length < count) {
            collected.push(buffer.shift());
          }

          if (collected.length >= count) {
            if (timer) clearTimeout(timer);
            callback(new Uint8Array(collected), null);
          } else {
            // Schedule another check
            setTimeout(collect, 10);
          }
        }

        collect();
      },

      // Internal: process buffer against waiting callbacks
      _process: function () {
        if (buffer.length === 0 || callbacks.length === 0) return;

        var byte = buffer[0];
        for (var i = 0; i < callbacks.length; i++) {
          var entry = callbacks[i];
          if (entry.resolved) continue;
          if (entry.expected.indexOf(byte) >= 0) {
            entry.resolved = true;
            if (entry.timer) clearTimeout(entry.timer);
            callbacks.splice(i, 1);
            buffer.shift();
            entry.callback(byte, false);
            return;
          }
        }
      },

      clear: function () {
        buffer = [];
      },
    };
  }

  /* ---------- YMODEM Sender ---------- */

  /*
   * createSender(options)
   *
   * options:
   *   file: { name: string, size: number, data: Uint8Array } or File object
   *   write: function(Uint8Array) - write bytes to serial port
   *   onData: function(callback) - register for incoming serial data
   *   onProgress: function(sent, total) - progress callback
   *   onComplete: function(success, error) - completion callback
   *   timeout: number (ms, default 30000)
   */
  function createSender(options) {
    var file = options.file;
    var write = options.write;
    var onData = options.onData;
    var onProgress = options.onProgress || function () {};
    var onComplete = options.onComplete || function () {};
    var timeout = options.timeout || DEFAULT_TIMEOUT_MS;

    var parser = createParser();
    var cancelled = false;
    var dataCleanup = null;

    // Extract file info
    var fileName = file.name || "file";
    var fileSize = file.size || 0;
    var fileData = file.data || null;

    // If File object, we need to read it first
    function getFileData(callback) {
      if (fileData) {
        callback(toUint8Array(fileData));
        return;
      }
      if (file instanceof Blob) {
        var reader = new FileReader();
        reader.onload = function () {
          callback(new Uint8Array(reader.result));
        };
        reader.onerror = function () {
          callback(null);
        };
        reader.readAsArrayBuffer(file);
      } else {
        callback(null);
      }
    }

    function cleanup() {
      if (dataCleanup) {
        dataCleanup();
        dataCleanup = null;
      }
    }

    function fail(error) {
      cleanup();
      if (!cancelled) {
        cancelled = true;
        onComplete(false, error);
      }
    }

    function succeed() {
      cleanup();
      if (!cancelled) {
        cancelled = true;
        onComplete(true, null);
      }
    }

    function sendCancel() {
      write(new Uint8Array([CAN, CAN, CAN]));
    }

    return {
      start: function () {
        // Register for incoming data
        dataCleanup = onData(function (bytes) {
          parser.feed(bytes);
        });

        getFileData(function (data) {
          if (!data) {
            fail("Failed to read file data");
            return;
          }

          fileData = data;
          fileSize = data.length;

          // Step 1: Wait for receiver's 'C' (CRC mode request)
          var retries = 0;

          function waitForC() {
            if (cancelled) return;
            parser.expect([C, NAK, CAN], timeout, function (byte, timedOut) {
              if (timedOut) {
                retries++;
                if (retries >= MAX_RETRIES) {
                  fail("Timeout waiting for receiver");
                  return;
                }
                waitForC();
                return;
              }
              if (byte === CAN) {
                fail("Transfer cancelled by receiver");
                return;
              }
              if (byte === NAK) {
                // Receiver wants checksum mode - we only support CRC
                fail("Receiver requested checksum mode, only CRC supported");
                return;
              }
              // Received 'C' - send block 0 (filename)
              sendBlock0();
            });
          }

          function sendBlock0() {
            if (cancelled) return;
            var block0 = buildBlock0(fileName, fileSize);
            write(block0);
            waitForBlock0Ack();
          }

          function waitForBlock0Ack() {
            var nakCount = 0;
            function wait() {
              if (cancelled) return;
              parser.expect([ACK, NAK, CAN], timeout, function (byte, timedOut) {
                if (timedOut) {
                  nakCount++;
                  if (nakCount >= MAX_CONSECUTIVE_NAK) {
                    sendCancel();
                    fail("Timeout waiting for block 0 ACK");
                    return;
                  }
                  sendBlock0();
                  return;
                }
                if (byte === CAN) {
                  fail("Transfer cancelled by receiver");
                  return;
                }
                if (byte === NAK) {
                  nakCount++;
                  if (nakCount >= MAX_CONSECUTIVE_NAK) {
                    sendCancel();
                    fail("Too many NAKs on block 0");
                    return;
                  }
                  sendBlock0();
                  return;
                }
                // ACK received, wait for 'C' to start data
                waitForDataC();
              });
            }
            wait();
          }

          function waitForDataC() {
            if (cancelled) return;
            parser.expect([C, CAN], timeout, function (byte, timedOut) {
              if (timedOut) {
                fail("Timeout waiting for data start");
                return;
              }
              if (byte === CAN) {
                fail("Transfer cancelled by receiver");
                return;
              }
              // Start sending data blocks
              sendDataBlocks(0, 1);
            });
          }

          function sendDataBlocks(offset, blockNum) {
            if (cancelled) return;

            if (offset >= fileSize) {
              // All data sent, send EOT
              sendEOT();
              return;
            }

            // Determine block size: use STX (1024) when possible, SOH (128) for last partial
            var remaining = fileSize - offset;
            var useSTX = remaining > BLOCK_SIZE_SOH;
            var blockSize = useSTX ? BLOCK_SIZE_STX : BLOCK_SIZE_SOH;
            var header = useSTX ? STX : SOH;

            // For the last block, if remaining < blockSize, still use SOH
            if (remaining < BLOCK_SIZE_SOH) {
              header = SOH;
              blockSize = BLOCK_SIZE_SOH;
            }

            var chunk = fileData.slice(offset, offset + blockSize);
            var block = buildBlock(header, blockNum & 0xff, chunk);
            var nakCount = 0;

            function sendAndWait() {
              if (cancelled) return;
              write(block);

              parser.expect([ACK, NAK, CAN], timeout, function (byte, timedOut) {
                if (timedOut) {
                  nakCount++;
                  if (nakCount >= MAX_RETRIES) {
                    sendCancel();
                    fail("Timeout waiting for block ACK");
                    return;
                  }
                  sendAndWait();
                  return;
                }
                if (byte === CAN) {
                  fail("Transfer cancelled by receiver");
                  return;
                }
                if (byte === NAK) {
                  nakCount++;
                  if (nakCount >= MAX_CONSECUTIVE_NAK) {
                    sendCancel();
                    fail("Too many NAKs");
                    return;
                  }
                  sendAndWait();
                  return;
                }
                // ACK received, move to next block
                var sentBytes = Math.min(offset + blockSize, fileSize);
                onProgress(sentBytes, fileSize);
                sendDataBlocks(offset + blockSize, blockNum + 1);
              });
            }

            sendAndWait();
          }

          function sendEOT() {
            if (cancelled) return;
            var nakCount = 0;

            function send() {
              write(new Uint8Array([EOT]));
              parser.expect([ACK, NAK, CAN], timeout, function (byte, timedOut) {
                if (timedOut || byte === NAK) {
                  nakCount++;
                  if (nakCount >= MAX_RETRIES) {
                    fail("Timeout waiting for EOT ACK");
                    return;
                  }
                  send();
                  return;
                }
                if (byte === CAN) {
                  fail("Transfer cancelled by receiver");
                  return;
                }
                // EOT acknowledged, send empty block 0 (batch end)
                sendBatchEnd();
              });
            }

            send();
          }

          function sendBatchEnd() {
            if (cancelled) return;
            // Wait for 'C' before batch end block
            parser.expect([C, CAN], timeout, function (byte, timedOut) {
              if (timedOut) {
                // Some receivers don't send C before batch end, try sending anyway
                doSendBatchEnd();
                return;
              }
              if (byte === CAN) {
                fail("Transfer cancelled by receiver");
                return;
              }
              doSendBatchEnd();
            });
          }

          function doSendBatchEnd() {
            if (cancelled) return;
            var emptyBlock = buildEmptyBlock0();
            write(emptyBlock);

            parser.expect([ACK, NAK, CAN], timeout, function (byte, timedOut) {
              if (timedOut) {
                // Consider success even on timeout for batch end
                succeed();
                return;
              }
              if (byte === CAN) {
                fail("Transfer cancelled by receiver");
                return;
              }
              // Transfer complete
              succeed();
            });
          }

          // Start the transfer
          waitForC();
        });
      },

      cancel: function () {
        if (!cancelled) {
          cancelled = true;
          sendCancel();
          cleanup();
        }
      },
    };
  }

  /* ---------- YMODEM Receiver ---------- */

  /*
   * createReceiver(options)
   *
   * options:
   *   write: function(Uint8Array) - write bytes to serial port
   *   onData: function(callback) - register for incoming serial data
   *   onFileStart: function(name, size) - called when file header received
   *   onProgress: function(received, total) - progress callback
   *   onComplete: function(success, fileData, error) - completion callback
   *   timeout: number (ms, default 30000)
   */
  function createReceiver(options) {
    var write = options.write;
    var onData = options.onData;
    var onFileStart = options.onFileStart || function () {};
    var onProgress = options.onProgress || function () {};
    var onComplete = options.onComplete || function () {};
    var timeout = options.timeout || DEFAULT_TIMEOUT_MS;

    var parser = createParser();
    var cancelled = false;
    var dataCleanup = null;

    var fileName = "";
    var fileSize = 0;
    var fileData = [];
    var receivedBytes = 0;
    var expectedBlockNum = 1;

    function cleanup() {
      if (dataCleanup) {
        dataCleanup();
        dataCleanup = null;
      }
    }

    function fail(error) {
      cleanup();
      if (!cancelled) {
        cancelled = true;
        onComplete(false, null, error);
      }
    }

    function succeed() {
      cleanup();
      if (!cancelled) {
        cancelled = true;
        // Trim fileData to actual file size
        var result = new Uint8Array(fileSize);
        var offset = 0;
        for (var i = 0; i < fileData.length && offset < fileSize; i++) {
          var chunk = fileData[i];
          var toCopy = Math.min(chunk.length, fileSize - offset);
          for (var j = 0; j < toCopy; j++) {
            result[offset++] = chunk[j];
          }
        }
        onComplete(true, result, null);
      }
    }

    function sendByte(b) {
      write(new Uint8Array([b]));
    }

    function sendCancel() {
      write(new Uint8Array([CAN, CAN, CAN]));
    }

    return {
      start: function () {
        // Register for incoming data
        dataCleanup = onData(function (bytes) {
          parser.feed(bytes);
        });

        // Step 1: Send 'C' to request CRC mode
        var retries = 0;

        function requestCRC() {
          if (cancelled) return;
          sendByte(C);
          receiveBlock0();
        }

        function receiveBlock0() {
          if (cancelled) return;
          parser.expectBlock(timeout, function (block, err) {
            if (err) {
              retries++;
              if (retries >= MAX_RETRIES) {
                fail("Timeout waiting for block 0: " + err);
                return;
              }
              requestCRC();
              return;
            }

            if (block.type === "cancel") {
              fail("Transfer cancelled by sender");
              return;
            }

            if (block.type !== "data" || block.blockNum !== 0) {
              sendByte(NAK);
              retries++;
              if (retries >= MAX_RETRIES) {
                fail("Invalid block 0");
                return;
              }
              requestCRC();
              return;
            }

            // Parse block 0
            var info = parseBlock0(block.data);
            if (!info) {
              sendByte(NAK);
              retries++;
              if (retries >= MAX_RETRIES) {
                fail("Failed to parse block 0");
                return;
              }
              requestCRC();
              return;
            }

            if (info.batchEnd) {
              // Empty filename = batch end, transfer complete
              sendByte(ACK);
              succeed();
              return;
            }

            fileName = info.filename;
            fileSize = info.size;
            onFileStart(fileName, fileSize);

            // ACK block 0 and send 'C' to start data
            sendByte(ACK);
            setTimeout(function () {
              sendByte(C);
              receiveDataBlocks();
            }, 10);
          });
        }

        function receiveDataBlocks() {
          if (cancelled) return;

          parser.expectBlock(timeout, function (block, err) {
            if (err) {
              // CRC error or timeout - send NAK
              sendByte(NAK);
              receiveDataBlocks();
              return;
            }

            if (block.type === "cancel") {
              fail("Transfer cancelled by sender");
              return;
            }

            if (block.type === "eot") {
              // End of file data
              sendByte(ACK);
              // Wait for batch end (empty block 0)
              setTimeout(function () {
                sendByte(C);
                receiveBatchEnd();
              }, 10);
              return;
            }

            if (block.type === "data") {
              // Verify block number
              if (block.blockNum === ((expectedBlockNum - 1) & 0xff)) {
                // Duplicate block, ACK but don't store
                sendByte(ACK);
                receiveDataBlocks();
                return;
              }

              if (block.blockNum !== (expectedBlockNum & 0xff)) {
                // Out of sequence
                sendByte(NAK);
                receiveDataBlocks();
                return;
              }

              // Store data
              fileData.push(block.data);
              receivedBytes += block.data.length;
              expectedBlockNum++;

              onProgress(Math.min(receivedBytes, fileSize), fileSize);

              sendByte(ACK);
              receiveDataBlocks();
            }
          });
        }

        function receiveBatchEnd() {
          if (cancelled) return;

          parser.expectBlock(timeout, function (block, err) {
            if (err) {
              // Timeout on batch end is OK, consider transfer complete
              succeed();
              return;
            }

            if (block.type === "cancel") {
              fail("Transfer cancelled by sender");
              return;
            }

            if (block.type === "data" && block.blockNum === 0) {
              var info = parseBlock0(block.data);
              if (info && info.batchEnd) {
                sendByte(ACK);
                succeed();
                return;
              }
              // Another file in batch - we only support single file
              sendByte(ACK);
              succeed();
              return;
            }

            // Unexpected, but complete anyway
            succeed();
          });
        }

        // Start by requesting CRC mode
        requestCRC();
      },

      cancel: function () {
        if (!cancelled) {
          cancelled = true;
          sendCancel();
          cleanup();
        }
      },
    };
  }

  /* ---------- Public API ---------- */

  return {
    // Constants
    SOH: SOH,
    STX: STX,
    EOT: EOT,
    ACK: ACK,
    NAK: NAK,
    CAN: CAN,
    C: C,
    BLOCK_SIZE_SOH: BLOCK_SIZE_SOH,
    BLOCK_SIZE_STX: BLOCK_SIZE_STX,
    DEFAULT_TIMEOUT_MS: DEFAULT_TIMEOUT_MS,

    // Core functions
    crc16: crc16,
    createSender: createSender,
    createReceiver: createReceiver,

    // Utilities (exposed for testing)
    buildBlock: buildBlock,
    buildBlock0: buildBlock0,
    buildEmptyBlock0: buildEmptyBlock0,
    parseBlock0: parseBlock0,
    toUint8Array: toUint8Array,
    padBlock: padBlock,
  };
})();

/*
 * ---------- Test vectors (run in browser console) ----------
 *
 * // CRC-16 test vectors from XMODEM specification
 * (function() {
 *   // "123456789" should produce CRC 0x31C3
 *   var testData = new Uint8Array([0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39]);
 *   var crc = YMODEM.crc16(testData);
 *   console.log("CRC of '123456789':", crc.toString(16), crc === 0x31C3 ? "PASS" : "FAIL");
 *
 *   // Empty data should produce CRC 0x0000
 *   var emptyCrc = YMODEM.crc16(new Uint8Array(0));
 *   console.log("CRC of empty:", emptyCrc.toString(16), emptyCrc === 0x0000 ? "PASS" : "FAIL");
 *
 *   // Single byte 0x00 should produce CRC 0x0000
 *   var zeroCrc = YMODEM.crc16(new Uint8Array([0x00]));
 *   console.log("CRC of 0x00:", zeroCrc.toString(16));
 *
 *   // Block building
 *   var block0 = YMODEM.buildBlock0("test.txt", 1234);
 *   console.log("Block 0 length:", block0.length, block0.length === 133 ? "PASS" : "FAIL");
 *   console.log("Block 0 header:", block0[0] === YMODEM.SOH ? "PASS" : "FAIL");
 *   console.log("Block 0 num:", block0[1] === 0 && block0[2] === 0xFF ? "PASS" : "FAIL");
 *
 *   // Block 0 parsing
 *   var parsed = YMODEM.parseBlock0(block0.slice(3, 3 + 128));
 *   console.log("Parsed filename:", parsed.filename, parsed.filename === "test.txt" ? "PASS" : "FAIL");
 *   console.log("Parsed size:", parsed.size, parsed.size === 1234 ? "PASS" : "FAIL");
 *
 *   // Empty block 0 (batch end)
 *   var emptyBlock = YMODEM.buildEmptyBlock0();
 *   var emptyParsed = YMODEM.parseBlock0(emptyBlock.slice(3, 3 + 128));
 *   console.log("Batch end:", emptyParsed.batchEnd ? "PASS" : "FAIL");
 * })();
 */
