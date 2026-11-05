// Run jsc-style web tests under Node when the macOS JavaScriptCore shell is not
// available (Linux, Windows, CI). Used by test/web/run.sh as:
//   node test/web/node_jsc_shim.js test/web/harness.js test/web/test_X.js
//
// Each file runs in one fresh vm context that only has the jsc shell globals the
// harness relies on (load, print, quit, console). Node globals such as require,
// process, setTimeout or TextEncoder are deliberately NOT exposed, so a test that
// leans on them fails here the same way it would under jsc.
"use strict";
const fs = require("fs");
const vm = require("vm");

const ctx = vm.createContext({
    console: console,
    print: (...args) => console.log(args.join(" ")),
    quit: (code) => process.exit(code || 0),
});
ctx.load = (file) => vm.runInContext(fs.readFileSync(file, "utf8"), ctx, { filename: file });

const files = process.argv.slice(2);
if (files.length === 0) {
    console.error("usage: node node_jsc_shim.js <file.js>...");
    process.exit(2);
}
for (const f of files) ctx.load(f);
