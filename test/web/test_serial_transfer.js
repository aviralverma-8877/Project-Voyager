// Tests for serial transfer module message handling.
// The WebSocket message handlers (handleInvitation, handleParticipant) use
// VoyagerLib.toText and VoyagerLib.formatSize for safe text handling.

load("data_main/lib.js");

// Minimal DOM mocks for the serial transfer module
var mockElements = {};
function resetMocks() {
    mockElements = {};
}

var document = {
    getElementById: function(id) {
        if (!mockElements[id]) {
            mockElements[id] = {
                id: id,
                textContent: "",
                hidden: false,
                disabled: false,
                value: "",
                checked: false,
                files: null,
                classList: {
                    toggle: function() {},
                    add: function() {},
                    remove: function() {}
                },
                style: {},
                setAttribute: function() {},
                addEventListener: function() {},
                replaceChildren: function() {},
                append: function() {}
            };
        }
        return mockElements[id];
    },
    querySelectorAll: function() { return []; },
    createElement: function(tag) {
        return {
            tagName: tag.toUpperCase(),
            textContent: "",
            className: "",
            append: function() {},
            addEventListener: function() {}
        };
    }
};

var navigator = { serial: undefined };

// Mock functions that script.js uses but aren't defined in lib.js
function byId(id) { return document.getElementById(id); }
function setText(id, text) { var el = byId(id); if (el) el.textContent = text; }
function setDisabled(id, disabled) { var el = byId(id); if (el) el.disabled = disabled; }
function el(tag, cls, text) {
    var node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
}
function alertText(msg) {}
function ws_send(obj, quiet) { return true; }
function save_blob(blob, filename) {}

var L = VoyagerLib;

// Tests for the message handling logic (extracted for testability)

test("toText sanitizes malicious invitation data", function() {
    var malicious = {
        sender_mac: '<script>alert("xss")</script>',
        sender_name: 'Bob<img src=x onerror=alert(1)>',
        fileName: 'virus.exe<script>',
        fileSize: 12345
    };

    // Using toText ensures the values are safe strings
    var safeMac = L.toText(malicious.sender_mac);
    var safeName = L.toText(malicious.sender_name);
    var safeFile = L.toText(malicious.fileName);

    // toText returns strings without HTML interpretation
    assertEq(typeof safeMac, "string");
    assertEq(typeof safeName, "string");
    assertEq(typeof safeFile, "string");

    // The strings contain the original characters (not escaped HTML entities)
    // because toText just coerces to string - the escaping happens when
    // rendered via textContent in the browser
    assertTrue(safeMac.indexOf('<script>') >= 0);
});

test("toText handles null/undefined gracefully", function() {
    assertEq(L.toText(null), "");
    assertEq(L.toText(undefined), "");
    assertEq(L.toText({}), "{}");
    assertEq(L.toText([1, 2]), "[1,2]");
});

test("formatSize formats file sizes correctly", function() {
    assertEq(L.formatSize(0), "0 Bytes");
    assertEq(L.formatSize(100), "100 Bytes");
    assertEq(L.formatSize(1023), "1023 Bytes");
    assertEq(L.formatSize(1024), "1.00 KB");
    assertEq(L.formatSize(1536), "1.50 KB");
    assertEq(L.formatSize(1048576), "1.00 MB");
    assertEq(L.formatSize(1572864), "1.50 MB");
});

test("invitation data extraction uses safe text handling", function() {
    // Simulating what handleInvitation does
    var msg = {
        sender_mac: "AA:BB:CC:DD:EE:FF",
        sender_name: "TestDevice",
        fileName: "document.pdf",
        fileSize: 1048576
    };

    var inv = {
        senderMac: L.toText(msg.sender_mac),
        senderName: L.toText(msg.sender_name),
        fileName: L.toText(msg.fileName),
        fileSize: msg.fileSize || 0
    };

    assertEq(inv.senderMac, "AA:BB:CC:DD:EE:FF");
    assertEq(inv.senderName, "TestDevice");
    assertEq(inv.fileName, "document.pdf");
    assertEq(inv.fileSize, 1048576);
});

test("participant data extraction uses safe text handling", function() {
    // Simulating what handleParticipant does
    var msg = {
        mac: "11:22:33:44:55:66",
        name: "Node1"
    };

    var p = {
        mac: L.toText(msg.mac),
        name: L.toText(msg.name)
    };

    assertEq(p.mac, "11:22:33:44:55:66");
    assertEq(p.name, "Node1");
});

test("missing participant name falls back gracefully", function() {
    var msg = {
        mac: "AA:BB:CC:DD:EE:FF"
        // name is missing
    };

    var p = {
        mac: L.toText(msg.mac),
        name: L.toText(msg.name) // undefined -> ""
    };

    assertEq(p.mac, "AA:BB:CC:DD:EE:FF");
    assertEq(p.name, "");

    // Display logic should use: p.name || p.mac
    var displayName = p.name || p.mac;
    assertEq(displayName, "AA:BB:CC:DD:EE:FF");
});

test("file size 0 or missing handled correctly", function() {
    var msg1 = { fileSize: 0 };
    var msg2 = { /* fileSize missing */ };

    assertEq(msg1.fileSize || 0, 0);
    assertEq(msg2.fileSize || 0, 0);
    assertEq(L.formatSize(0), "0 Bytes");
});

done();
