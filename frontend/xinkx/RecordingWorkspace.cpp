#include "RecordingEngine.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QSizePolicy>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QSet>
#include <algorithm>
#include <array>
#include <QVBoxLayout>
#include <QUuid>

namespace xinkx {
namespace {

struct SourceChoice {
	QString title;
	QString uuid;
};

bool enumerateScene(void *data, obs_source_t *source)
{
	auto *choices = static_cast<QVector<SourceChoice> *>(data);
	choices->append({QString::fromUtf8(obs_source_get_name(source)), QString::fromUtf8(obs_source_get_uuid(source))});
	return true;
}

bool enumerateVideoSource(void *data, obs_source_t *source)
{
	if (obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT ||
	    !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO)) return true;
	return enumerateScene(data, source);
}

// Icons are painted explicitly: font glyphs such as "record" / "stop" vary
// by Windows font and style and may become blank or oversized.
QIcon recordingActionIcon(bool stopping)
{
	QPixmap pixmap(20, 20);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	if (stopping) {
		painter.setBrush(QColor(245, 245, 245));
		painter.drawRect(QRect(5, 5, 10, 10));
	} else {
		painter.setBrush(QColor(230, 50, 55));
		painter.drawEllipse(QRect(4, 4, 12, 12));
	}
	return QIcon(pixmap);
}

// Each row selects its job on a click while leaving the embedded start/stop
// button independently clickable.
class RecordingJobRow final : public QWidget {
public:
	RecordingJobRow(QListWidget *owner, QListWidgetItem *item)
		: QWidget(owner), list(owner), entry(item) {}
protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		list->setCurrentItem(entry);
		QWidget::mousePressEvent(event);
	}
private:
	QListWidget *list;
	QListWidgetItem *entry;
};

class RecordingDialog final : public QDialog {
public:
	explicit RecordingDialog(OBSBasic *parent) : QDialog(parent), main(parent), jobs(loadRecordingJobs())
	{
		setWindowTitle(QStringLiteral("XINKX · Настройки записи"));
		resize(900, 650);
		setMinimumSize(760, 560);
		auto *outer = new QVBoxLayout(this);
		auto *explanation = new QLabel(QStringLiteral(
			"Кнопка «Запись» запускает только отмеченные задания. "
			"Каждое задание записывается в отдельный файл. Основная запись OBS необязательна."), this);
		explanation->setWordWrap(true);
		outer->addWidget(explanation);
		auto *body = new QHBoxLayout();
		outer->addLayout(body, 1);
		auto *left = new QVBoxLayout();
		body->addLayout(left, 0);
		list = new QListWidget(this);
		list->setMinimumWidth(280);
		left->addWidget(list, 1);
		auto *add = new QPushButton(QStringLiteral("Добавить"), this);
		auto *duplicate = new QPushButton(QStringLiteral("Дублировать"), this);
		auto *remove = new QPushButton(QStringLiteral("Удалить"), this);
		left->addWidget(add);
		left->addWidget(duplicate);
		left->addWidget(remove);
		// Controls live next to each recording title, not below the list.
		auto *refresh = new QTimer(this);
		refresh->setInterval(350);
		connect(refresh, &QTimer::timeout, this, [this] { refreshJobButtons(); });
		refresh->start();
		auto *right = new QGroupBox(QStringLiteral("Параметры задания"), this);
		body->addWidget(right, 1);
		auto *form = new QFormLayout(right);
		name = new QLineEdit(right);
		form->addRow(QStringLiteral("Название"), name);
		enabled = new QCheckBox(QStringLiteral("Запускать общей кнопкой записи"), right);
		form->addRow(enabled);
		kind = new QComboBox(right);
		kind->addItem(QStringLiteral("Штатная запись OBS"), QStringLiteral("obs"));
		kind->addItem(QStringLiteral("Конкретная сцена"), QStringLiteral("scene"));
		kind->addItem(QStringLiteral("Отдельный видеоисточник"), QStringLiteral("source"));
		kind->addItem(QStringLiteral("Вертикальная сцена XINKX"), QStringLiteral("portrait"));
		form->addRow(QStringLiteral("Что записывать"), kind);
		target = new QComboBox(right);
		target->setMinimumWidth(300);
		form->addRow(QStringLiteral("Сцена / источник"), target);
		inherit = new QCheckBox(QStringLiteral("Наследовать кодировщик и качество OBS"), right);
		form->addRow(inherit);
		auto *recommended = new QPushButton(QStringLiteral("Локальная запись: CQP + NVENC P4"), right);
		recommended->setToolTip(QStringLiteral("Игра/сцена: CQP 21; отдельный источник (например камера): CQP 27"));
		form->addRow(recommended);
		encoder = new QComboBox(right);
		const struct { const char *id; const char *title; } encoders[] = {
			{"obs_nvenc_h264_tex", "NVIDIA NVENC H.264"},
			{"obs_nvenc_hevc_tex", "NVIDIA NVENC HEVC"},
			{"obs_nvenc_av1_tex", "NVIDIA NVENC AV1"},
			{"obs_x264", "x264 (CPU)"},
		};
		for (const auto &candidate : encoders) {
			if (obs_get_encoder_codec(candidate.id))
				encoder->addItem(QString::fromUtf8(candidate.title), QString::fromUtf8(candidate.id));
		}
		form->addRow(QStringLiteral("Кодировщик"), encoder);
		preset = new QComboBox(right);
		for (int i = 1; i <= 7; ++i)
			preset->addItem(QStringLiteral("P%1").arg(i), QStringLiteral("p%1").arg(i));
		form->addRow(QStringLiteral("NVENC пресет"), preset);
		rateControl = new QComboBox(right);
		rateControl->addItem(QStringLiteral("По качеству (CQP / CRF)"), QStringLiteral("quality"));
		rateControl->addItem(QStringLiteral("По битрейту (CBR)"), QStringLiteral("cbr"));
		form->addRow(QStringLiteral("Режим качества"), rateControl);
		quality = new QSpinBox(right);
		quality->setRange(12, 35);
		quality->setToolTip(QStringLiteral("NVENC: CQP, x264: CRF; меньше — выше качество"));
		form->addRow(QStringLiteral("Качество (CQP / CRF)"), quality);
		bitrate = new QSpinBox(right);
		bitrate->setRange(500, 100000);
		bitrate->setSuffix(QStringLiteral(" кбит/с"));
		form->addRow(QStringLiteral("Битрейт (CBR)"), bitrate);
		auto *sizeRow = new QHBoxLayout();
		width = new QSpinBox(right);
		width->setRange(0, 7680);
		width->setSingleStep(2);
		width->setSpecialValueText(QStringLiteral("Исходное"));
		height = new QSpinBox(right);
		height->setRange(0, 4320);
		height->setSingleStep(2);
		height->setSpecialValueText(QStringLiteral("Исходное"));
		sizeRow->addWidget(width);
		sizeRow->addWidget(new QLabel(QStringLiteral("×"), right));
		sizeRow->addWidget(height);
		form->addRow(QStringLiteral("Разрешение"), sizeRow);
		fps = new QSpinBox(right);
		fps->setRange(0, 120);
		fps->setSpecialValueText(QStringLiteral("Как в OBS"));
		fps->setToolTip(QStringLiteral("Для отдельной записи доступны частоты, кратные FPS OBS: например, при 60 FPS — 60/30/20/15"));
		form->addRow(QStringLiteral("FPS"), fps);
		outputSummary = new QLabel(right);
		outputSummary->setWordWrap(true);
		outputSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
		outputSummary->setToolTip(QStringLiteral("Параметры задания; CQP не задаёт постоянный битрейт. "
			"Реальные параметры записанного MKV можно проверить через ffprobe или MediaInfo."));
		form->addRow(QStringLiteral("Параметры записи"), outputSummary);
		recordMarkers = new QCheckBox(QStringLiteral("Вести метки"), right);
		recordMarkers->setToolTip(QStringLiteral("Метки «Отметить момент» и включённых событий сохраняются "
			"рядом с файлом как *.markers.json для Kdenlive. По умолчанию выключено."));
		form->addRow(recordMarkers);
		audioPanel = new QWidget(right);
		auto *audioLayout = new QVBoxLayout(audioPanel);
		audioLayout->setContentsMargins(0, 0, 0, 0);
		auto *audioGrid = new QGridLayout();
		for (int i = 0; i < 6; ++i) {
			const auto label = QStringLiteral("Дорожка %1").arg(i + 1);
			audioTracks[size_t(i)] = new QCheckBox(label, audioPanel);
			audioGrid->addWidget(audioTracks[size_t(i)], i / 3, i % 3);
		}
		audioLayout->addLayout(audioGrid);
		auto *audioPresets = new QHBoxLayout();
		auto *montageAudio = new QPushButton(QStringLiteral("Для монтажа: 2 + 3 + 4"), audioPanel);
		auto *noAudio = new QPushButton(QStringLiteral("Без звука"), audioPanel);
		audioPresets->addWidget(montageAudio);
		audioPresets->addWidget(noAudio);
		audioLayout->addLayout(audioPresets);
		form->addRow(QStringLiteral("Аудиодорожки OBS"), audioPanel);
		connect(montageAudio, &QPushButton::clicked, this, [this] {
			for (int i = 0; i < 6; ++i)
				audioTracks[size_t(i)]->setChecked(i >= 1 && i <= 3);
		});
		connect(noAudio, &QPushButton::clicked, this, [this] {
			for (auto *track : audioTracks) track->setChecked(false);
		});
		format = new QComboBox(right);
		format->addItem(QStringLiteral("MKV (рекомендуется)"), QStringLiteral("mkv"));
		format->addItem(QStringLiteral("MP4 (риск потери при сбое)"), QStringLiteral("mp4"));
		form->addRow(QStringLiteral("Контейнер"), format);
		auto *folderRow = new QHBoxLayout();
		folder = new QLineEdit(right);
		folder->setPlaceholderText(QStringLiteral("Папка штатных записей OBS"));
		folderRow->addWidget(folder, 1);
		auto *browse = new QPushButton(QStringLiteral("…"), right);
		browse->setMaximumWidth(35);
		folderRow->addWidget(browse);
		form->addRow(QStringLiteral("Корневая папка"), folderRow);
		sessionFolder = new QCheckBox(QStringLiteral("Группировать записи: дата → время запуска"), right);
		sessionFolder->setToolTip(QStringLiteral("Например: Записи/2026-11-18/12-49-00-000/Игра.mkv. "
			"Все задания одного запуска с общей корневой папкой попадут в один каталог."));
		form->addRow(sessionFolder);
		auto *hint = new QLabel(QStringLiteral(
			"Разрешение 0×0 — исходное. Каждый отмеченный микшер OBS попадёт "
			"в отдельную звуковую дорожку файла. Маршрутизация источников настраивается "
			"в «Расширенных свойствах звука» OBS. Без галочек — видео без звука."), right);
		hint->setWordWrap(true);
		form->addRow(hint);
		buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
		outer->addWidget(buttons);

		connect(add, &QPushButton::clicked, this, [this] {
			store();
			RecordingJob job;
			job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
			job.name = QStringLiteral("Новая запись");
			job.enabled = false;
			job.recordMarkers = false;
			jobs.append(job);
			rebuild(jobs.size() - 1);
		});
		connect(duplicate, &QPushButton::clicked, this, [this] {
			const int index = list->currentRow();
			if (index < 0) return;
			store();
			auto job = jobs.at(index);
			job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
			job.name += QStringLiteral(" (копия)");
			job.enabled = false;
			job.recordMarkers = false;
			jobs.append(job);
			rebuild(jobs.size() - 1);
		});
		connect(remove, &QPushButton::clicked, this, [this] {
			const int index = list->currentRow();
			if (index < 0) return;
			jobs.removeAt(index);
			rebuild(std::min(index, static_cast<int>(jobs.size()) - 1));
		});
		connect(list, &QListWidget::currentRowChanged, this, [this](int index) {
			if (changing) return;
			store();
			showJob(index);
		});
		connect(name, &QLineEdit::textChanged, this, [this] {
			if (changing || list->currentRow() < 0) return;
			if (auto *rowLabel = rowLabels.at(list->currentRow()).data()) rowLabel->setText(name->text());
		});
		connect(kind, &QComboBox::currentIndexChanged, this, [this] {
			if (changing) return;
			populateSources({});
			updateEnabled();
		});
		connect(inherit, &QCheckBox::toggled, this, [this] { updateEnabled(); });
		connect(recommended, &QPushButton::clicked, this, [this] {
			if (kind->currentData() == QStringLiteral("obs")) return;
			const bool cameraLike = kind->currentData() == QStringLiteral("source");
			inherit->setChecked(false);
			// Keep HEVC/AV1 if already selected; don't silently change a codec
			// when the user only asks for better local recording quality.
			if (!encoder->currentData().toString().startsWith(QStringLiteral("obs_nvenc_"))) {
				const int nvencIndex = encoder->findData(QStringLiteral("obs_nvenc_h264_tex"));
				if (nvencIndex >= 0) encoder->setCurrentIndex(nvencIndex);
			}
			rateControl->setCurrentIndex(std::max(0, rateControl->findData(QStringLiteral("quality"))));
			preset->setCurrentIndex(std::max(0, preset->findData(QStringLiteral("p4"))));
			quality->setValue(cameraLike ? 27 : 21);
			if (cameraLike) fps->setValue(30);
			updateEnabled();
		});
		// One technical summary for all named outputs: resolution, requested FPS,
		// codec, quality mode and audio-track count.  Show configured parameters,
		// not an invented measured bitrate for variable-quality CQP jobs.
		const auto refreshSummary = [this] { updateSummary(); };
		connect(encoder, &QComboBox::currentIndexChanged, this, refreshSummary);
		connect(preset, &QComboBox::currentIndexChanged, this, refreshSummary);
		connect(rateControl, &QComboBox::currentIndexChanged, this, refreshSummary);
		connect(format, &QComboBox::currentIndexChanged, this, refreshSummary);
		connect(width, &QSpinBox::valueChanged, this, refreshSummary);
		connect(height, &QSpinBox::valueChanged, this, refreshSummary);
		connect(fps, &QSpinBox::valueChanged, this, refreshSummary);
		connect(quality, &QSpinBox::valueChanged, this, refreshSummary);
		connect(bitrate, &QSpinBox::valueChanged, this, refreshSummary);
		for (auto *track : audioTracks)
			connect(track, &QCheckBox::toggled, this, refreshSummary);
		connect(kind, &QComboBox::currentIndexChanged, this, refreshSummary);
		connect(inherit, &QCheckBox::toggled, this, refreshSummary);
		connect(encoder, &QComboBox::currentIndexChanged, this, [this] { updateEnabled(); });
		connect(rateControl, &QComboBox::currentIndexChanged, this, [this] { updateEnabled(); });
		connect(browse, &QPushButton::clicked, this, [this] {
			const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Папка записей"), folder->text());
			if (!dir.isEmpty()) folder->setText(dir);
		});
		connect(buttons, &QDialogButtonBox::accepted, this, [this] {
			store();
			QSet<QString> names;
			for (const auto &job : jobs) {
				if (job.name.trimmed().isEmpty() || names.contains(job.name.trimmed())) {
					QMessageBox::warning(this, QStringLiteral("Записи"),
						QStringLiteral("У каждого задания должно быть уникальное непустое название."));
					return;
				}
				names.insert(job.name.trimmed());
				if (job.enabled && (job.kind == QStringLiteral("scene") || job.kind == QStringLiteral("source")) &&
				    job.sourceUuid.isEmpty()) {
					QMessageBox::warning(this, QStringLiteral("Записи"),
						QStringLiteral("Для «%1» не выбран источник.").arg(job.name));
					return;
				}
				if (!job.inherit && ((job.width == 0) != (job.height == 0) ||
					(job.width && (job.width % 2 || job.height % 2)))) {
					QMessageBox::warning(this, QStringLiteral("Записи"),
						QStringLiteral("Для «%1» разрешение должно быть чётным или 0×0.").arg(job.name));
					return;
				}
			}
			QString error;
			if (!saveRecordingJobs(jobs, &error)) {
				QMessageBox::critical(this, QStringLiteral("Ошибка сохранения"), error);
				return;
			}
			accept();
		});
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		rebuild(jobs.isEmpty() ? -1 : 0);
	}

private:
	QPointer<OBSBasic> main;
	QVector<RecordingJob> jobs;
	QListWidget *list{};
	QVector<QPointer<QLabel>> rowLabels;
	QVector<QPointer<QPushButton>> rowButtons;
	QLineEdit *name{};
	QLineEdit *folder{};
	QCheckBox *enabled{};
	QCheckBox *recordMarkers{};
	QCheckBox *inherit{};
	QCheckBox *sessionFolder{};
	QComboBox *kind{};
	QComboBox *target{};
	QComboBox *encoder{};
	QComboBox *preset{};
	QWidget *audioPanel{};
	std::array<QCheckBox *, 6> audioTracks{};
	QComboBox *format{};
	QComboBox *rateControl{};
	QSpinBox *quality{};
	QSpinBox *bitrate{};
	QSpinBox *width{};
	QSpinBox *height{};
	QSpinBox *fps{};
	QLabel *outputSummary{};
	QDialogButtonBox *buttons{};
	bool changing = false;
	int oldIndex = -1;

	void refreshJobButtons()
	{
		for (int index = 0; index < jobs.size() && index < rowButtons.size(); ++index) {
			auto *button = rowButtons.at(index).data();
			if (!button) continue;
			const bool active = recordingJobActive(main, jobs.at(index).id);
			static const QIcon recordIcon = recordingActionIcon(false);
			static const QIcon stopIcon = recordingActionIcon(true);
			button->setIcon(active ? stopIcon : recordIcon);
			button->setToolTip(active ? QStringLiteral("Остановить запись")
			                          : QStringLiteral("Начать запись"));
			button->setAccessibleName(button->toolTip());
		}
	}

	void rebuild(int selection)
	{
		changing = true;
		list->clear();
		rowLabels.clear();
		rowButtons.clear();
		for (const auto &job : jobs) {
			auto *item = new QListWidgetItem(list);
			// QListWidgetItem must not paint its own text behind our custom row.
			// All visible text is owned by the QLabel below.
			auto *row = new RecordingJobRow(list, item);
			auto *layout = new QHBoxLayout(row);
			layout->setContentsMargins(6, 1, 2, 1);
			layout->setSpacing(4);
			auto *title = new QLabel(job.name, row);
			title->setAttribute(Qt::WA_TransparentForMouseEvents);
			title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
			layout->addWidget(title, 1);
			auto *button = new QPushButton(row);
			button->setObjectName(QStringLiteral("xinkxRecordingJobToggle"));
			// QListWidget's hover/selection palette otherwise paints this button
			// almost the same color as the selected row (the icon vanishes).
			// Use a stable button surface for both states, not text glyphs.
			button->setStyleSheet(QStringLiteral(
				"QPushButton#xinkxRecordingJobToggle {background-color: #444a5a; "
				"border: 1px solid #646b7e; border-radius: 4px;}"
				"QPushButton#xinkxRecordingJobToggle:hover {background-color: #68748f; "
				"border-color: #a5aec1;}"
				"QPushButton#xinkxRecordingJobToggle:pressed {background-color: #283348;}"));
			button->setFixedSize(34, 26);
			button->setIconSize(QSize(20, 20));
			button->setFocusPolicy(Qt::NoFocus);
			layout->addWidget(button);
			const QString jobId = job.id;
			connect(button, &QPushButton::clicked, this, [this, item, jobId] {
				list->setCurrentItem(item);
				store();
				QString error;
				if (!saveRecordingJobs(jobs, &error)) {
					QMessageBox::warning(this, QStringLiteral("Записи"), error);
					return;
				}
				toggleRecordingJob(main, jobId);
				refreshJobButtons();
			});
			item->setSizeHint(QSize(0, std::max(32, row->sizeHint().height())));
			list->setItemWidget(item, row);
			rowLabels.append(title);
			rowButtons.append(button);
		}
		changing = false;
		oldIndex = -1;
		list->setCurrentRow(selection);
		showJob(selection);
		refreshJobButtons();
	}

	void store()
	{
		if (changing || oldIndex < 0 || oldIndex >= jobs.size()) return;
		auto &job = jobs[oldIndex];
		job.name = name->text().trimmed();
		job.enabled = enabled->isChecked();
		job.recordMarkers = recordMarkers->isChecked();
		job.kind = kind->currentData().toString();
		job.sourceUuid = target->currentData().toString();
		job.inherit = inherit->isChecked();
		job.encoder = encoder->currentData().toString();
		job.width = width->value();
		job.height = height->value();
		job.fps = fps->value();
		job.quality = quality->value();
		job.preset = preset->currentData().toString();
		job.rateControl = rateControl->currentData().toString();
		job.bitrate = bitrate->value();
		job.audioTracks = 0;
		for (int i = 0; i < 6; ++i)
			if (audioTracks[size_t(i)]->isChecked()) job.audioTracks |= (1 << i);
		job.folder = folder->text().trimmed();
		job.sessionFolder = sessionFolder->isChecked();
		job.format = format->currentData().toString();
		if (oldIndex < rowLabels.size())
			if (auto *label = rowLabels.at(oldIndex).data()) label->setText(job.name);
	}

	void populateSources(const QString &uuid)
	{
		QVector<SourceChoice> choices;
		const auto currentKind = kind->currentData().toString();
		if (currentKind == QStringLiteral("scene")) obs_enum_scenes(enumerateScene, &choices);
		if (currentKind == QStringLiteral("source")) obs_enum_sources(enumerateVideoSource, &choices);
		target->clear();
		for (const auto &choice : choices) target->addItem(choice.title, choice.uuid);
		const int index = target->findData(uuid);
		if (index >= 0) target->setCurrentIndex(index);
		if (index < 0 && !uuid.isEmpty()) {
			target->addItem(QStringLiteral("Недоступный источник: %1").arg(uuid), uuid);
			target->setCurrentIndex(target->count() - 1);
		}
	}

	void showJob(int index)
	{
		changing = true;
		const bool valid = index >= 0 && index < jobs.size();
		name->parentWidget()->setEnabled(valid);
		oldIndex = valid ? index : -1;
		if (valid) {
			const auto &job = jobs.at(index);
			name->setText(job.name);
			enabled->setChecked(job.enabled);
			recordMarkers->setChecked(job.recordMarkers);
			kind->setCurrentIndex(std::max(0, kind->findData(job.kind)));
			populateSources(job.sourceUuid);
			inherit->setChecked(job.inherit);
			encoder->setCurrentIndex(std::max(0, encoder->findData(job.encoder)));
			width->setValue(job.width);
			height->setValue(job.height);
			fps->setValue(job.fps);
			quality->setValue(job.quality);
			preset->setCurrentIndex(std::max(0, preset->findData(job.preset)));
			rateControl->setCurrentIndex(std::max(0, rateControl->findData(job.rateControl)));
			bitrate->setValue(job.bitrate);
			for (int i = 0; i < 6; ++i)
				audioTracks[size_t(i)]->setChecked((job.audioTracks & (1 << i)) != 0);
			folder->setText(job.folder);
			sessionFolder->setChecked(job.sessionFolder);
			format->setCurrentIndex(std::max(0, format->findData(job.format)));
		}
		changing = false;
		updateEnabled();
		updateSummary();
	}

	void updateSummary()
	{
		if (!outputSummary) return;
		if (kind->currentData() == QStringLiteral("obs")) {
			outputSummary->setText(QStringLiteral("Штатная запись OBS — параметры в «Настройки → Вывод → Запись»."));
			return;
		}
		if (inherit->isChecked()) {
			outputSummary->setText(QStringLiteral("Наследуются параметры штатной записи OBS; "
				"фактические параметры MKV проверяются после записи."));
			return;
		}
		obs_video_info vi{};
		const bool hasVideo = obs_get_video_info(&vi);
		const QString resolution = width->value() && height->value()
			? QStringLiteral("%1×%2").arg(width->value()).arg(height->value())
			: QStringLiteral("исходное разрешение источника");
		const QString frameRate = fps->value() > 0
			? QString::number(fps->value())
			: hasVideo && vi.fps_den ? QString::number(double(vi.fps_num) / vi.fps_den, 'f', 2)
				: QStringLiteral("как в OBS");
		const QString qualityText = rateControl->currentData() == QStringLiteral("cbr")
			? QStringLiteral("CBR %1 кбит/с").arg(bitrate->value())
			: QStringLiteral("CQP/CRF %1 (битрейт переменный)").arg(quality->value());
		int tracks = 0;
		for (auto *track : audioTracks) tracks += track->isChecked() ? 1 : 0;
		outputSummary->setText(QStringLiteral("%1 · %2 FPS · %3 · %4 · %5 · аудио: %6")
			.arg(resolution, frameRate, encoder->currentText(), qualityText,
				format->currentData().toString().toUpper(), QString::number(tracks)));
	}

	void updateEnabled()
	{
		const bool separate = kind->currentData() != QStringLiteral("obs");
		const bool sourceKind = kind->currentData() == QStringLiteral("source") ||
			kind->currentData() == QStringLiteral("scene");
		target->setEnabled(sourceKind);
		inherit->setEnabled(separate);
		const bool advanced = separate && !inherit->isChecked();
		for (auto *widget : {static_cast<QWidget *>(encoder), static_cast<QWidget *>(rateControl),
			static_cast<QWidget *>(width), static_cast<QWidget *>(height), static_cast<QWidget *>(fps)})
			widget->setEnabled(advanced);
		preset->setEnabled(advanced && encoder->currentData().toString().startsWith(QStringLiteral("obs_nvenc_")));
		quality->setEnabled(advanced && rateControl->currentData() != QStringLiteral("cbr"));
		bitrate->setEnabled(advanced && rateControl->currentData() == QStringLiteral("cbr"));
		audioPanel->setEnabled(separate);
		format->setEnabled(separate);
		folder->setEnabled(separate);
		sessionFolder->setEnabled(separate);
	}
};

} // namespace

void showRecordingSettings(OBSBasic *main)
{
	if (!main) return;
	RecordingDialog dialog(main);
	dialog.exec();
	refreshRecordingButton(main);
}

} // namespace xinkx
