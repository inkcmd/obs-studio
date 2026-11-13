// Regression contract for the OBS-native Controls dock and shared 16:9/9:16 editor.
// C++ builds and interactive Qt testing remain separate from these source checks.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const read = rel => readFileSync(new URL(rel, import.meta.url), 'utf8');
const controls = read('../../widgets/OBSBasicControls.cpp');
const recording = read('../RecordingWorkspace.cpp');
const preview = read('../../widgets/OBSBasicPreview.cpp');

// OBS-sized push buttons rather than separately styled/hard-coded 28px tool buttons.
assert.match(controls, /ui->virtualCamConfigButton->sizeHint\(\)/);
assert.match(controls, /accounts->setFixedSize\(auxiliaryButtonSize\)/);
assert.match(controls, /recordOptions->setFixedSize\(auxiliaryButtonSize\)/);
assert.match(controls, /recordOptions->setIcon\(ui->virtualCamConfigButton->icon\(\)\)/);
assert.match(controls, /QPushButton::clicked, this, \[main\] \{ xinkx::showRecordingSettings\(main\); \}/);
assert.doesNotMatch(controls, /recordMenu->addAction/);

// Every saved job has its own button and accurate running/stopped text.
assert.match(recording, /class RecordingJobRow final : public QWidget/);
assert.match(recording, /list->setItemWidget\(item, row\)/);
assert.match(recording, /recordingJobActive\(main, jobs\.at\(index\)\.id\)/);
assert.match(recording, /toggleRecordingJob\(main, jobId\)/);
assert.match(recording, /saveRecordingJobs\(jobs, &error\)/);
assert.match(recording, /rowLabel->setText\(name->text\(\)\)/);
assert.doesNotMatch(recording, /new QPushButton\(QStringLiteral\("Запустить выбранное"\)/);

// An out-of-bounds portrait resize handle or selected source must be tested
// before switching context (which would clear the portrait selection).
const press = preview.slice(preview.indexOf('void OBSBasicPreview::mousePressEvent('),
  preview.indexOf('void OBSBasicPreview::UpdateCursor('));
assert.match(press, /if \(!portraitHit && IsPortraitEditing\(\) && event->button\(\) == Qt::LeftButton\)/);
assert.match(press, /GetStretchHandleData\(relativePos, true\)/);
assert.match(press, /stretchHandle != ItemHandle::None/);
assert.match(press, /portraitHit = SelectedAtPos\(scenePos\)/);
assert.ok(press.indexOf('GetStretchHandleData(relativePos, true)') < press.indexOf('SetPortraitEditing(portraitHit)'));
// Do not change existing overlap-selection cycling behavior.
assert.match(preview, /GetItemAtPos\(pos, true\)/);

console.log('XINKX controls sizing, inline per-job recording and off-canvas portrait handles OK');
