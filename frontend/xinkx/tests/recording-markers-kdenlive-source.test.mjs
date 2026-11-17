// Source-contract test. Native OBS + Kdenlive integration still requires a runtime test.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const read = file => readFileSync(new URL(`../${file}`, import.meta.url), 'utf8');
const engine = read('RecordingEngine.cpp');
const recording = read('RecordingWorkspace.cpp');
const markers = read('MarkerEngine.cpp');
const markerHeader = read('MarkerEngine.hpp');
const settings = read('StudioSettingsPages.cpp');
const hotkeys = read('../widgets/OBSBasic_Hotkeys.cpp');
const output = read('../../libobs/obs-output.c');
const native = read('../widgets/OBSBasic_Recording.cpp');

assert.match(engine, /job\.recordMarkers = obj\.value\(QStringLiteral\("recordMarkers"\)\)\.toBool\(false\)/);
assert.match(engine, /obj\[QStringLiteral\("recordMarkers"\)\] = job\.recordMarkers/);
assert.match(recording, /recordMarkers = new QCheckBox\(QStringLiteral\("Вести метки"\)/);
assert.match(recording, /job\.recordMarkers = recordMarkers->isChecked\(\)/);
assert.match(recording, /recordMarkers->setChecked\(job\.recordMarkers\)/);
assert.match(hotkeys, /obs_hotkey_register_frontend\([\s\S]*"xinkx\.MarkMoment"/);
assert.match(hotkeys, /LoadHotkey\(xinkxMarkMomentHotkey, "xinkx.MarkMoment"\)/);
assert.match(hotkeys, /obs_hotkey_unregister\(xinkxMarkMomentHotkey\)/);
assert.match(markers, /if \(!window \|\| !hasMarkedRecording\(window\)\) return false/);
assert.match(markers, /if \(requireRecording && recordings\.isEmpty\(\)\) return false/);
assert.match(engine, /obs_output_add_packet_callback\(output, packet, this\)/);
assert.match(engine, /obs_output_remove_packet_callback\(output, packet, this\)/);
assert.match(engine, /time->cts >= request\.requestedNs/);
assert.match(engine, /obs_output_paused\(output\)/);
assert.match(engine, /for \(const auto &run : runs\) \{[\s\S]*run->marker->append\(label, type, alsoCsv, item\)/);
assert.match(engine, /nativeMarker->append\(label, type, alsoCsv, item\)/);
assert.match(engine, /QSaveFile file\(filePath \+ QStringLiteral\("\.markers\.json"\)\)/);
assert.match(engine, /const qint64 pos = std::max<qint64>\(0, static_cast<qint64>\(std::llround\(frames\)\)\)/);
assert.doesNotMatch(engine, /frames \+= double\(now - cts\)/, 'Must not infer output frames from wall time');
assert.match(engine, /filePath \+ QStringLiteral\("\.markers\.json"\)/);
assert.match(engine, /QJsonDocument\(markers\)\.toJson/);
assert.match(engine, /static_cast<double>\(pos\)/);
assert.match(engine, /QStringLiteral\("comment"\), label/);
assert.match(engine, /QStringLiteral\("type"\), type/);
assert.match(engine, /if \(job\.recordMarkers\)[\s\S]*std::make_unique<MarkerOutput>/);
assert.match(engine, /void nativeStarted\(obs_output_t \*output, const QString &path\)/);
assert.match(native, /xinkx::nativeRecordingStarted\(this, outputHandler->fileOutput/);
assert.match(native, /xinkx::nativeRecordingStopped\(this\)/);
assert.match(native, /void OBSBasic::RecordingFileChanged\([\s\S]*xinkx::nativeRecordingStarted/);
assert.match(output, /Video-only encoded outputs bypass send_interleaved/);
assert.match(output, /cb->packet_cb\(output, packet, packet_time, cb->param\)/);
assert.match(markerHeader, /bool markMoment\(\)/);
assert.match(settings, /add\(QStringLiteral\("Метки"\), markerSettingsPage/);

// Matching the clip FPS matters: the integer is a frame, not milliseconds.
const videoFps = 60;
const position = 47100;
const fixture = JSON.stringify([{pos: position, comment: 'Момент 1', type: 0}]);
const parsed = JSON.parse(fixture);
assert.deepEqual(parsed, [{pos: position, comment: 'Момент 1', type: 0}]);
assert.equal(parsed[0].pos / videoFps, 785);
console.log('Recording markers: opt-in, OBS hotkey, per-output packet PTS, pause, sidecars and Kdenlive schema');
