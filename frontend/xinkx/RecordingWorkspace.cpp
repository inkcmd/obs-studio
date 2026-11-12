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
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QSet>
#include <algorithm>
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

class RecordingDialog final : public QDialog {
public:
	explicit RecordingDialog(OBSBasic *parent) : QDialog(parent), main(parent), jobs(loadRecordingJobs())
	{
		setWindowTitle(QStringLiteral("XINKX · Настройки записи"));
		resize(850, 600);
		setMinimumSize(740, 510);
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
		list->setMinimumWidth(230);
		left->addWidget(list, 1);
		auto *add = new QPushButton(QStringLiteral("Добавить"), this);
		auto *duplicate = new QPushButton(QStringLiteral("Дублировать"), this);
		auto *remove = new QPushButton(QStringLiteral("Удалить"), this);
		left->addWidget(add);
		left->addWidget(duplicate);
		left->addWidget(remove);
		auto *toggle = new QPushButton(QStringLiteral("Запустить выбранное"), this);
		left->addWidget(toggle);
		connect(toggle, &QPushButton::clicked, this, [this] {
			store();
			const int index = list->currentRow();
			if (index < 0) return;
			QString error;
			if (!saveRecordingJobs(jobs, &error)) {
				QMessageBox::warning(this, QStringLiteral("Записи"), error);
				return;
			}
			toggleRecordingJob(main, jobs.at(index).id);
		});
		auto *refresh = new QTimer(this);
		refresh->setInterval(350);
		connect(refresh, &QTimer::timeout, this, [this, toggle] {
			const int index = list->currentRow();
			const bool valid = index >= 0 && index < jobs.size();
			toggle->setEnabled(valid);
			if (valid) toggle->setText(recordingJobActive(main, jobs.at(index).id) ?
				QStringLiteral("Остановить выбранное") : QStringLiteral("Запустить выбранное"));
		});
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
		form->addRow(QStringLiteral("FPS"), fps);
		audio = new QComboBox(right);
		for (int i = 0; i < 6; ++i)
			audio->addItem(QStringLiteral("Микшер OBS · дорожка %1").arg(i + 1), i);
		audio->addItem(QStringLiteral("Без звука"), -1);
		form->addRow(QStringLiteral("Звук"), audio);
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
		form->addRow(QStringLiteral("Папка"), folderRow);
		auto *hint = new QLabel(QStringLiteral(
			"Разрешение 0×0 — исходное. Для отдельного источника звук берётся из "
			"выбранного микшера OBS, а не автоматически только из этого источника. "
			"Индивидуальный видеокодировщик создаётся для каждого файла."), right);
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
			jobs.append(job);
			rebuild(jobs.size() - 1);
		});
		connect(remove, &QPushButton::clicked, this, [this] {
			const int index = list->currentRow();
			if (index < 0) return;
			jobs.removeAt(index);
			rebuild(std::min(index, jobs.size() - 1));
		});
		connect(list, &QListWidget::currentRowChanged, this, [this](int index) {
			if (changing) return;
			store();
			showJob(index);
		});
		connect(name, &QLineEdit::textChanged, this, [this] {
			if (changing || list->currentRow() < 0) return;
			list->currentItem()->setText(name->text());
		});
		connect(kind, &QComboBox::currentIndexChanged, this, [this] {
			if (changing) return;
			populateSources({});
			updateEnabled();
		});
		connect(inherit, &QCheckBox::toggled, this, [this] { updateEnabled(); });
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
	QLineEdit *name{};
	QLineEdit *folder{};
	QCheckBox *enabled{};
	QCheckBox *inherit{};
	QComboBox *kind{};
	QComboBox *target{};
	QComboBox *encoder{};
	QComboBox *audio{};
	QComboBox *format{};
	QComboBox *rateControl{};
	QSpinBox *quality{};
	QSpinBox *bitrate{};
	QSpinBox *width{};
	QSpinBox *height{};
	QSpinBox *fps{};
	QDialogButtonBox *buttons{};
	bool changing = false;
	int oldIndex = -1;

	void rebuild(int selection)
	{
		changing = true;
		list->clear();
		for (const auto &job : jobs) list->addItem(job.name);
		changing = false;
		oldIndex = -1;
		list->setCurrentRow(selection);
		showJob(selection);
	}

	void store()
	{
		if (changing || oldIndex < 0 || oldIndex >= jobs.size()) return;
		auto &job = jobs[oldIndex];
		job.name = name->text().trimmed();
		job.enabled = enabled->isChecked();
		job.kind = kind->currentData().toString();
		job.sourceUuid = target->currentData().toString();
		job.inherit = inherit->isChecked();
		job.encoder = encoder->currentData().toString();
		job.width = width->value();
		job.height = height->value();
		job.fps = fps->value();
		job.quality = quality->value();
		job.rateControl = rateControl->currentData().toString();
		job.bitrate = bitrate->value();
		job.audioMix = audio->currentData().toInt();
		job.folder = folder->text().trimmed();
		job.format = format->currentData().toString();
		if (auto *item = list->item(oldIndex)) item->setText(job.name);
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
			kind->setCurrentIndex(std::max(0, kind->findData(job.kind)));
			populateSources(job.sourceUuid);
			inherit->setChecked(job.inherit);
			encoder->setCurrentIndex(std::max(0, encoder->findData(job.encoder)));
			width->setValue(job.width);
			height->setValue(job.height);
			fps->setValue(job.fps);
			quality->setValue(job.quality);
			rateControl->setCurrentIndex(std::max(0, rateControl->findData(job.rateControl)));
			bitrate->setValue(job.bitrate);
			audio->setCurrentIndex(std::max(0, audio->findData(job.audioMix)));
			folder->setText(job.folder);
			format->setCurrentIndex(std::max(0, format->findData(job.format)));
		}
		changing = false;
		updateEnabled();
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
		quality->setEnabled(advanced && rateControl->currentData() != QStringLiteral("cbr"));
		bitrate->setEnabled(advanced && rateControl->currentData() == QStringLiteral("cbr"));
		audio->setEnabled(separate);
		format->setEnabled(separate);
		folder->setEnabled(separate);
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
