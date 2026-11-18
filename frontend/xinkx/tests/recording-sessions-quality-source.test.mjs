import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const read = file => readFileSync(new URL(`../${file}`, import.meta.url), 'utf8');
const header = read('RecordingEngine.hpp');
const engine = read('RecordingEngine.cpp');
const dialog = read('RecordingWorkspace.cpp');

// Saved jobs keep their existing 'inherit' flag; only newly added jobs default to quality-based NVENC.
assert.match(header, /bool inherit = false;/);
assert.match(engine, /obj\.value\(QStringLiteral\("inherit"\)\)\.toBool\(true\)/);
assert.match(header, /QString preset = QStringLiteral\("p4"\)/);
assert.match(engine, /obs_data_set_string\(vSettings, "preset", job\.preset\.toUtf8\(\)\.constData\(\)\)/);
assert.match(engine, /cbr \? "CBR" : \(x264 \? "CRF" : "CQP"\)/);
assert.match(dialog, /cameraLike \? 27 : 21/);
assert.match(dialog, /if \(cameraLike\) fps->setValue\(30\);/);
assert.match(dialog, /Локальная запись: CQP \+ NVENC P4/);
assert.match(dialog, /preset->findData\(QStringLiteral\("p4"\)\)/);

// Session folders are a per-job option. All concurrently active jobs share currentSession.
assert.match(header, /bool sessionFolder = true;/);
assert.match(engine, /QString currentSession;/);
assert.match(engine, /if \(!anyRunning\) currentSession\.clear\(\);/);
assert.match(engine, /if \(currentSession\.isEmpty\(\)\)/);
assert.match(engine, /yyyy-MM-dd\/HH-mm-ss-zzz/);
assert.match(engine, /job\.sessionFolder \? QDir\(root\)\.filePath\(currentSession\) : root/);
assert.match(engine, /QFileInfo::exists\(QDir\(directory\)\.filePath\(filename\)\)/);
assert.match(engine, /obj\.value\(QStringLiteral\("sessionFolder"\)\)\.toBool\(true\)/);
assert.match(engine, /obj\[QStringLiteral\("sessionFolder"\)\] = job\.sessionFolder/);
assert.match(dialog, /sessionFolder->setChecked\(job\.sessionFolder\)/);
assert.match(dialog, /job\.sessionFolder = sessionFolder->isChecked\(\)/);

console.log('Record Engine session folders, saved job compatibility, NVENC P4/CQP defaults: PASS');
