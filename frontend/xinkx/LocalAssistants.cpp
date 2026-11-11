#include "LocalAssistants.hpp"

#include "MultiStreamManager.hpp"
#include "StudioAutomation.hpp"
#include "StudioWorkspace.hpp"
#include "StreamAccounts.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <string>

#ifdef _WIN32
#include <Windows.h>
#include <sapi.h>
#endif

namespace xinkx {

namespace {

QString cleanSpeech(QString text)
{
	text.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f]")), QStringLiteral(" "));
	return text.simplified().left(220);
}

QJsonObject message(const QString &role, const QString &content)
{
	return {{QStringLiteral("role"), role}, {QStringLiteral("content"), content}};
}

bool liveOutputs(OBSBasic *window)
{
	if (obs_frontend_streaming_active()) return true;
	if (auto *outputs = window->findChild<MultiStreamManager *>()) return outputs->anythingActive();
	return false;
}

} // namespace

LocalLLM::LocalLLM(OBSBasic *mainWindow) : QObject(mainWindow), window(mainWindow)
{
	setObjectName(QStringLiteral("xinkxLocalLLM"));
}

void LocalLLM::reset()
{
	if (active) return;
	memory = {};
}

void LocalLLM::ask(const QString &model, const QString &prompt, bool allowLive)
{
	if (active) { emit failed(QStringLiteral("Модель уже отвечает")); return; }
	if (!allowLive && liveOutputs(window)) {
		emit failed(QStringLiteral("LLM отключена во время эфира: снимай нагрузку с GPU или разреши вручную"));
		return;
	}
	const QString selected = model.trimmed();
	if (selected.isEmpty() || selected.size() > 100 || prompt.trimmed().isEmpty() || prompt.size() > 6000) {
		emit failed(QStringLiteral("Нужно имя локальной модели и вопрос до 6000 символов"));
		return;
	}
	QJsonArray thread;
	thread.append(message(QStringLiteral("system"), QStringLiteral(
		"Ты локальный помощник XINKX Studio, работаешь без доступа к ключам и сети. "
		"Отвечай кратко по-русски. Если предлагаешь действие OBS, покажи только "
		"понятный JSON с полем command и параметрами. Никогда не утверждай, "
		"что ты выполнил команду: пользователь подтверждает действия вручную.")));
	for (const auto &entry : memory) thread.append(entry);
	thread.append(message(QStringLiteral("user"), prompt));
	const QJsonObject payload{{QStringLiteral("model"), selected},
		{QStringLiteral("stream"), false},
		{QStringLiteral("messages"), thread},
		{QStringLiteral("options"), QJsonObject{{QStringLiteral("num_ctx"), 4096}}}};
	QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:11434/api/chat")));
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	request.setTransferTimeout(120000);
	active = network.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
	emit busyChanged(true);
	connect(active, &QNetworkReply::finished, this, [this, prompt] {
		QNetworkReply *reply = active;
		active = nullptr;
		if (!reply) return;
		const QByteArray body = reply->readAll();
		const auto error = reply->error();
		const QString errorText = reply->errorString();
		reply->deleteLater();
		emit busyChanged(false);
		if (error != QNetworkReply::NoError || body.size() > 1024 * 1024) {
			emit failed(QStringLiteral("Ollama localhost: %1").arg(errorText));
			return;
		}
		const auto data = QJsonDocument::fromJson(body).object();
		const QString text = data.value(QStringLiteral("message")).toObject()
			.value(QStringLiteral("content")).toString().left(12000);
		if (text.isEmpty()) { emit failed(QStringLiteral("Ollama вернула пустой ответ")); return; }
		memory.append(message(QStringLiteral("user"), prompt));
		memory.append(message(QStringLiteral("assistant"), text));
		while (memory.size() > 12) memory.removeFirst();
		emit answer(text);
	});
}

SpeechEngine::SpeechEngine(OBSBasic *window) : QObject(window)
{
	setObjectName(QStringLiteral("xinkxSpeechEngine"));
	workers.setMaxThreadCount(1);
	QSettings settings(studioSettingsPath(), QSettings::IniFormat);
	paidEnabled = settings.value(QStringLiteral("ai/speakPaidAlerts"), false).toBool();
	if (auto *events = window->findChild<EventCore *>(QStringLiteral("xinkxEventCore"))) {
		connect(events, &EventCore::delivered, this, [this](const StudioEvent &event) {
			if (paidEnabled && event.paid &&
			    (event.type == QStringLiteral("donation") || event.type == QStringLiteral("sub")))
				say(QStringLiteral("%1, %2. %3").arg(event.name, event.amount, event.message));
		});
	}
}

void SpeechEngine::setAnnouncePaid(bool enabled)
{
	paidEnabled = enabled;
	QSettings settings(studioSettingsPath(), QSettings::IniFormat);
	settings.setValue(QStringLiteral("ai/speakPaidAlerts"), enabled);
}

void SpeechEngine::say(const QString &text)
{
	const QString safe = cleanSpeech(text);
	if (safe.isEmpty()) return;
#ifdef _WIN32
	if (pendingSpeeches.fetch_add(1) >= 5) {
		pendingSpeeches.fetch_sub(1);
		return;
	}
	workers.start([this, safe] {
		const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		if (SUCCEEDED(initialized)) {
			ISpVoice *voice = nullptr;
			if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
				IID_ISpVoice, reinterpret_cast<void **>(&voice))) && voice) {
				const std::wstring speech = safe.toStdWString();
				voice->Speak(speech.c_str(), SPF_DEFAULT, nullptr);
				voice->Release();
			}
			CoUninitialize();
		}
		pendingSpeeches.fetch_sub(1);
	});
#else
	Q_UNUSED(safe);
#endif
}

void installAssistantWorkspace(OBSBasic *window)
{
	auto *llm = new LocalLLM(window);
	auto *speech = new SpeechEngine(window);
	auto *tabs = studioTabs(window);
	if (!tabs) return;
	auto *page = new QWidget(tabs);
	auto *layout = new QVBoxLayout(page);
	auto *status = new QLabel(QStringLiteral(
		"Ollama на 127.0.0.1:11434 · модель запускается только по нажатию. "
		"Команды OBS требуют отдельного подтверждения."), page);
	status->setWordWrap(true);
	layout->addWidget(status);
	QSettings settings(studioSettingsPath(), QSettings::IniFormat);
	auto *model = new QLineEdit(settings.value(QStringLiteral("ai/model"),
		QStringLiteral("qwen3-vl:8b-instruct")).toString(), page);
	layout->addWidget(model);
	auto *allowLive = new QCheckBox(QStringLiteral("Разрешить нагрузку LLM во время трансляции"), page);
	layout->addWidget(allowLive);
	auto *prompt = new QPlainTextEdit(page);
	prompt->setPlaceholderText(QStringLiteral("Вопрос об оформлении или управлении студией…"));
	prompt->setMaximumHeight(100);
	layout->addWidget(prompt);
	auto *actions = new QHBoxLayout();
	auto *send = new QPushButton(QStringLiteral("Спросить локальную модель"), page);
	auto *reset = new QPushButton(QStringLiteral("Очистить контекст"), page);
	actions->addWidget(send);
	actions->addWidget(reset);
	layout->addLayout(actions);
	auto *response = new QPlainTextEdit(page);
	response->setReadOnly(true);
	layout->addWidget(response, 1);
	auto *candidate = new QPlainTextEdit(page);
	candidate->setPlaceholderText(QStringLiteral("Предложенная моделью команда JSON — только после проверки"));
	candidate->setMaximumHeight(80);
	layout->addWidget(candidate);
	auto *apply = new QPushButton(QStringLiteral("Выполнить проверенную команду OBS"), page);
	layout->addWidget(apply);
	auto *speak = new QCheckBox(QStringLiteral("Озвучивать платные события (Windows SAPI)"), page);
	speak->setChecked(speech->announcePaid());
	layout->addWidget(speak);
	auto *testVoice = new QPushButton(QStringLiteral("Проверить голос"), page);
	layout->addWidget(testVoice);
	QObject::connect(send, &QPushButton::clicked, page, [=] {
		candidate->clear();
		QSettings store(studioSettingsPath(), QSettings::IniFormat);
		store.setValue(QStringLiteral("ai/model"), model->text());
		llm->ask(model->text(), prompt->toPlainText(), allowLive->isChecked());
	});
	QObject::connect(reset, &QPushButton::clicked, page, [=] {
		llm->reset();
		response->clear();
		candidate->clear();
	});
	QObject::connect(llm, &LocalLLM::busyChanged, send, [send](bool busy) { send->setEnabled(!busy); });
	QObject::connect(llm, &LocalLLM::failed, status, &QLabel::setText);
	QObject::connect(llm, &LocalLLM::answer, response, [=](const QString &text) {
		response->setPlainText(text);
		candidate->clear();
		status->setText(QStringLiteral("Локальная модель ответила; OBS не изменён"));
		const QRegularExpression json(QStringLiteral("\\{\\s*\\\"command\\\"[\\s\\S]*?\\}"));
		const auto found = json.match(text);
		if (found.hasMatch()) candidate->setPlainText(found.captured());
	});
	QObject::connect(apply, &QPushButton::clicked, page, [=] {
		const auto parsed = QJsonDocument::fromJson(candidate->toPlainText().toUtf8());
		if (!parsed.isObject()) { status->setText(QStringLiteral("Неверный формат команды JSON")); return; }
		if (auto *automation = window->findChild<StudioAutomation *>(QStringLiteral("xinkxStudioAutomation"))) {
			const auto result = automation->execute(parsed.object());
			status->setText(result.value(QStringLiteral("message")).toString());
		}
	});
	QObject::connect(speak, &QCheckBox::toggled, speech, &SpeechEngine::setAnnouncePaid);
	QObject::connect(testVoice, &QPushButton::clicked, speech, [speech] {
		speech->say(QStringLiteral("Привет. Это локальный голос XINKX Studio."));
	});
	tabs->addTab(page, QStringLiteral("AI и голос"));
}

} // namespace xinkx
