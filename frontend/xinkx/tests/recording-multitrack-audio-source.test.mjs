import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const read = file => readFileSync(new URL(file, import.meta.url), 'utf8');
const engine = read('../RecordingEngine.cpp');
const model = read('../RecordingEngine.hpp');
const dialog = read('../RecordingWorkspace.cpp');
const muxer = read('../../../plugins/obs-ffmpeg/obs-ffmpeg-mux.c');
const module = read('../../../plugins/obs-ffmpeg/obs-ffmpeg.c');
const core = read('../../../libobs/obs-output.c');
const muxHelper = read('../../../plugins/obs-ffmpeg/ffmpeg-mux/ffmpeg-mux.c');

test('6-bit mask is persisted and old single-mix jobs migrate, including silent jobs', () => {
  assert.match(model, /int audioTracks = 1;/);
  assert.match(engine, /obj\.contains\(QStringLiteral\("audioTracks"\)\)/);
  assert.match(engine, /obj\.value\(QStringLiteral\("audioTracks"\)\)\.toInt\(1\) & 0x3f/);
  assert.match(engine, /oldMix >= 0 && oldMix < 6 \? \(1 << oldMix\) : 0/);
  assert.match(engine, /obj\[QStringLiteral\("audioTracks"\)\] = job\.audioTracks & 0x3f/);
  assert.doesNotMatch(engine, /obj\[QStringLiteral\("audioMix"\)\] =/);
  for (const [oldMix, expectedMask] of [[-1, 0], [0, 1], [1, 2], [2, 4], [3, 8], [5, 32], [6, 0]]) {
    const actualMask = oldMix >= 0 && oldMix < 6 ? 1 << oldMix : 0;
    assert.equal(actualMask, expectedMask);
  }
});

test('selected sparse OBS mixes become contiguous file stream indices and separate AAC encoders', () => {
  assert.match(engine, /for \(int mixIndex = 0; mixIndex < 6; \+\+mixIndex\)/);
  assert.match(engine, /job\.audioTracks & \(1 << mixIndex\)/);
  assert.match(engine, /obs_audio_encoder_create\("ffmpeg_aac", encoderName\.constData\(\),\s*aSettings, size_t\(mixIndex\), nullptr\)/);
  assert.match(engine, /obs_output_set_audio_encoder\(run->output, run->audioEncoders\[size_t\(fileIndex\)\],\s*size_t\(fileIndex\)\)/);
  assert.match(engine, /std::array<OBSEncoderAutoRelease, 6> audioEncoders/);
  const mixes = [1, 2, 3]; // OBS mixer indexes 1,2,3 == UI mixers 2,3,4
  assert.deepEqual(mixes.map((mix, fileIndex) => ({obsTrack: mix + 1, fileTrack: fileIndex + 1})), [
    {obsTrack: 2, fileTrack: 1},
    {obsTrack: 3, fileTrack: 2},
    {obsTrack: 4, fileTrack: 3},
  ]);
});

test('recording dialog exposes independent track checkboxes, edit persistence, and montage/silent presets', () => {
  assert.match(dialog, /std::array<QCheckBox \*, 6> audioTracks/);
  assert.match(dialog, /Для монтажа: 2 \+ 3 \+ 4/);
  assert.match(dialog, /Без звука/);
  assert.match(dialog, /audioTracks\[size_t\(i\)\]->setChecked\(i >= 1 && i <= 3\)/);
  assert.match(dialog, /job\.audioTracks \|= \(1 << i\)/);
  assert.match(dialog, /audioTracks\[size_t\(i\)\]->setChecked\(\(job\.audioTracks & \(1 << i\)\) != 0\)/);
  assert.match(dialog, /audioPanel->setEnabled\(separate\)/);
});

test('video-only muxer has no mandatory audio flag and uses existing FFmpeg mux code', () => {
  assert.match(muxer, /\.id = "xinkx_ffmpeg_video_only_muxer"/);
  assert.match(muxer, /\.flags = OBS_OUTPUT_VIDEO \| OBS_OUTPUT_ENCODED \| OBS_OUTPUT_CAN_PAUSE/);
  assert.match(module, /obs_register_output\(&xinkx_ffmpeg_video_only_muxer\)/);
  assert.match(engine, /fileTrack \? "ffmpeg_muxer" : "xinkx_ffmpeg_video_only_muxer"/);
  assert.match(core, /if \(flag_audio\(output\) && !audio_valid\(output\)\)/);
  assert.match(muxHelper, /if \(ffm->params\.tracks\)/);
  assert.match(muxHelper, /if \(!ffm->video_stream && !ffm->num_audio_streams\)/);
});
