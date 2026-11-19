// Regression checks for the observed camera MKV (4 s of video with 8 s of audio)
// and double-painted job names in the native Recording dialog.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const engine = read('RecordingEngine.cpp');
const dialog = read('RecordingWorkspace.cpp');
const shape = read('ShapeEngine.cpp');
const controls = read('../widgets/OBSBasicControls.cpp');
const core = read('../../libobs/obs.c');
const encoderCore = read('../../libobs/obs-encoder.c');
const videoCore = read('../../libobs/media-io/video-io.c');
const muxer = read('../../plugins/obs-ffmpeg/obs-ffmpeg-mux.c');

// OBS forces aux-view graphics scheduling to match the main video clock.
// Creating a 30 FPS video_t in that 60 FPS render path timestamps half the
// expected duration when NVENC encodes the GPU frames.
assert.match(core, /main view graphics thread drives all frame output/);
const start = engine.indexOf('bool startJob(');
const end = engine.indexOf('void reap()', start);
const startJob = engine.slice(start, end);
assert.doesNotMatch(startJob, /videoInfo\.fps_num\s*=\s*uint32_t\(job\.fps\)/);
assert.match(startJob, /video_output_get_info\(mix\)/);
assert.match(startJob, /obs_encoder_set_frame_rate_divisor\(run->videoEncoder, uint32_t\(divisor\)\)/);
assert.match(startJob, /std::abs\(actualFps - job\.fps\) > 0\.1/);

// The OBS muxer must advertise the encoder's effective frame rate, not the
// primary canvas's 60 FPS. Otherwise a valid 30 FPS MKV reports 60 FPS in
// ffprobe/Kdenlive even though packet DTS increments by 33 ms.
assert.match(muxer, /video_t \*video = obs_encoder_video\(vencoder\)/);
assert.doesNotMatch(muxer, /video_t \*video = obs_get_video\(\)/);
assert.match(encoderCore, /return encoder->fps_override \? encoder->fps_override : encoder->media;/);
assert.match(videoCore, /new_video->info\.fps_den \*= divisor;/);

// The row is custom painted; QListWidgetItem must not render text beneath it.
assert.doesNotMatch(dialog, /item->setText\(job\.name\)/);
assert.doesNotMatch(dialog, /item->setText\(name->text\(\)\)/);
assert.match(dialog, /new QLabel\(job\.name, row\)/);
assert.match(dialog, /painter\.drawEllipse\(QRect\(4, 4, 12, 12\)\)/);
assert.match(dialog, /painter\.drawRect\(QRect\(5, 5, 10, 10\)\)/);
assert.match(controls, /accounts->setIcon\(style\(\)->standardIcon\(QStyle::SP_ArrowDown\)\)/);

// Custom-draw textures must be rendered inside an active effect; otherwise
// libobs silently skips the shape and logs "no active effect" every frame.
assert.match(shape, /obs_get_base_effect\(OBS_EFFECT_DEFAULT\)/);
assert.match(shape, /gs_effect_loop\(effect, "Draw"\)/);
assert.match(shape, /obs_source_draw\(shape->texture/);

console.log('Record Engine encoder clock, per-job icons, clean list text and shape effect PASS');
