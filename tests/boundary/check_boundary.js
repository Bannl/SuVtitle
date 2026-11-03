// =============================================================================
// SuVtitle - tests/boundary/check_boundary.js   (AudioPB's routing core never enters this repository)
//
//   node tests\boundary\check_boundary.js            scan the repository; exit 1 on any hit
//   node tests\boundary\check_boundary.js --selftest  prove the matcher (known hits and known passes)
//
// SuVtitle 1.1.0 (2026-10-25). The live translator came back from AudioPB, where it read its audio from AudioPB's
// shared-memory routing core -- NEXTALE's closed product. The user's rule (24/10): that core never goes into this
// open-source repository, "its comments included" (docs/LIVE_PLAN.md, D-L12). Ported code is purged of it by hand;
// this check makes the rule MECHANICAL: it fails when a file of the repository (tracked, or new and not ignored)
// names one of the core's identifiers or includes one of its headers.
//
// Tolerated: the core's FILE NAMES in prose (a "NOT linked: shm.cpp, ring_buffer.cpp" note says what stayed out, not
// how it works). Not tolerated: its types, fields, constants and functions -- the layout IS the secret.
// =============================================================================
'use strict';
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..', '..');

// The core's identifiers (word-bounded) -- types, fields, constants, functions, as AudioPB's live translator used them.
const IDENTS = [
    'AppSlot', 'FabricHeader', 'SharedFabric', 'AsyncSrc', 'RingBuffer',
    'inputSources', 'inputGains', 'inputDelayFrames', 'matrixEpoch', 'globalSampleRate', 'globalRate',
    'kRoleConsumes', 'kAppChannels', 'kMaxInputSrc', 'kMaxApps', 'kSlotLive', 'kRingFrames', 'kFabricName',
    'kDefaultRate', 'unpackInputSource', 'fabricSlots', 'fabricRingBase', 'attachConsumer', 'detachConsumer',
    'openAndClaim', 'mySlot', 'slotIndex', 'inLatencyUs', 'outLatencyUs', 'gainCol_', 'inRing_', 'srcs_',
    'fabric_', 'updateInputBinding', 'loadGains',
];
// The core's headers, included.
const INCLUDE_RE = /#\s*include\s*["<](?:[^">]*[\\/])?(shm|ring_buffer|async_src|patchbus_driver|FxBus|FxPipe)\.h[">]/;
// The core's namespace, qualified ("pb::" -- SuVtitle's own namespaces are pbt::, pbr::, pbtrf::, pbsub::, pbdocx::).
const NS_RE = /(^|[^A-Za-z0-9_])pb::/;
const IDENT_RE = new RegExp('(^|[^A-Za-z0-9_])(' + IDENTS.map(s => s.replace(/[$]/g, '\\$')).join('|') + ')(?![A-Za-z0-9_])');

const TEXT_EXT = new Set(['.h', '.hpp', '.c', '.cc', '.cpp', '.inc', '.rc', '.js', '.mjs', '.cjs', '.html', '.css',
                          '.md', '.txt', '.cmd', '.bat', '.py', '.iss', '.json', '.cmake', '']);

function hitsInText(text) {
    const out = [];
    const lines = text.split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
        const l = lines[i];
        let m;
        if ((m = INCLUDE_RE.exec(l))) out.push({ line: i + 1, what: '#include ' + m[1] + '.h' });
        if ((m = NS_RE.exec(l))) out.push({ line: i + 1, what: 'pb::' });
        if ((m = IDENT_RE.exec(l))) out.push({ line: i + 1, what: m[2] });
    }
    return out;
}

function selftest() {
    const must = [
        ['    pb::AppSlot* me = fabric_.mySlot();', 'AppSlot'],
        ['#include "shm.h"', '#include shm.h'],
        ['#include "../engine/ring_buffer.h"', '#include ring_buffer.h'],
        ['double gainCol_[4][32];', 'gainCol_'],
        ['// reads AppSlot::inputSources[] like a driver', 'AppSlot'],
        ['x = hdr->matrixEpoch.load();', 'matrixEpoch'],
        ['if (k < kMaxInputSrc)', 'kMaxInputSrc'],
    ];
    const pass = [
        '//  NOT linked (S5): the fabric core (PatchBus engine/shm.cpp, ring_buffer.cpp, async_src.cpp)',
        'namespace pbt { constexpr uint32_t kMaxLiveInputs = 4; }',
        'pbr::TranslatorManager mgr; pbtrf::cacheKey(); pbsub::langTag(); pbdocx::buildTranscript();',
        'std::string mySlotName;  int slotIndexes;  // longer words are not the identifiers',
        'the fabric is not here; the hub reads WASAPI',
    ];
    let bad = 0;
    for (const [s, want] of must) {
        const h = hitsInText(s);
        if (!h.some(x => x.what === want)) { console.log('SELFTEST MISS: ' + JSON.stringify(s) + ' should hit ' + want); bad++; }
    }
    for (const s of pass) {
        const h = hitsInText(s);
        if (h.length) { console.log('SELFTEST FALSE HIT: ' + JSON.stringify(s) + ' -> ' + h.map(x => x.what).join(',')); bad++; }
    }
    console.log(bad ? `check_boundary selftest: ${bad} FAILED` : `check_boundary selftest: ${must.length + pass.length}/${must.length + pass.length} ok`);
    return bad ? 1 : 0;
}

function main() {
    if (process.argv.includes('--selftest')) process.exit(selftest());
    let files;
    try {
        files = execFileSync('git', ['-C', ROOT, 'ls-files', '-co', '--exclude-standard', '-z'], { encoding: 'utf8' })
            .split('\0').filter(Boolean);
    } catch (e) {
        console.log('check_boundary: git ls-files failed (' + e.message + ')');
        process.exit(2);
    }
    const self = path.relative(ROOT, __filename).replace(/\\/g, '/');
    let total = 0, scanned = 0;
    for (const f of files) {
        if (f === self) continue;                                 // this file names them on purpose
        if (!TEXT_EXT.has(path.extname(f).toLowerCase())) continue;
        const p = path.join(ROOT, f);
        let text;
        try { text = fs.readFileSync(p, 'utf8'); } catch { continue; }
        scanned++;
        for (const h of hitsInText(text)) { console.log(`${f}:${h.line}: ${h.what}`); total++; }
    }
    console.log(total ? `check_boundary: ${total} HIT(S) in ${scanned} files -- AudioPB's routing core must not enter this repository`
                      : `check_boundary: 0 hits in ${scanned} files`);
    process.exit(total ? 1 : 0);
}

main();
