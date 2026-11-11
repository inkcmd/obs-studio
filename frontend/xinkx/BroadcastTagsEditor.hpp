#pragma once

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <utility>

namespace xinkx {

// A small native Qt chip editor. Strict tags follow Twitch's 10 x 25
// alphanumeric rules (also valid as Kick custom_tags); YouTube tags use its
// separate 500-character aggregate budget and may contain spaces.
class BroadcastTagsEditor final : public QWidget {
public:
	explicit BroadcastTagsEditor(bool strict, QWidget *parent = nullptr) : QWidget(parent), strict(strict)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(4);
		auto *entry = new QHBoxLayout();
		input = new QLineEdit(this);
		input->setObjectName(strict ? QStringLiteral("xinkxTagsInputTwitchKick") :
			QStringLiteral("xinkxTagsInputYouTube"));
		input->setPlaceholderText(QStringLiteral("Введите тег и нажмите Enter"));
		input->setMaxLength(strict ? 25 : 500);
		entry->addWidget(input, 1);
		auto *add = new QPushButton(QStringLiteral("Добавить"), this);
		entry->addWidget(add);
		layout->addLayout(entry);
		chips = new QGridLayout();
		chips->setContentsMargins(0, 0, 0, 0);
		chips->setSpacing(3);
		layout->addLayout(chips);
		hint = new QLabel(this);
		hint->setWordWrap(true);
		layout->addWidget(hint);
		connect(input, &QLineEdit::returnPressed, this, [this] { append(); });
		connect(input, &QLineEdit::textChanged, this, [this] { refreshHint(); });
		connect(add, &QPushButton::clicked, this, [this] { append(); });
		refresh();
	}

	QStringList tags() const { return values; }
	void setOnChanged(std::function<void(const QStringList &)> callback) { onChanged = std::move(callback); }
	void setTags(const QStringList &next)
	{
		QStringList cleaned;
		for (const QString &value : next) {
			const QString tag = value.trimmed();
			if (tag.isEmpty() || !valid(tag, cleaned)) continue;
			cleaned.append(tag);
		}
		if (cleaned == values) return;
		values = cleaned;
		refresh();
		if (onChanged) onChanged(values);
	}

private:
	bool valid(const QString &tag, const QStringList &existing) const
	{
		for (const QString &item : existing)
			if (item.compare(tag, Qt::CaseInsensitive) == 0) return false;
		if (strict) {
			static const QRegularExpression permitted(QStringLiteral("^[\\p{L}\\p{M}\\p{N}]+$"));
			return existing.size() < 10 && tag.size() <= 25 && permitted.match(tag).hasMatch();
		}
		// YouTube counts commas and enclosing quotes around multiword tags.
		int length = tag.size() + (tag.contains(QLatin1Char(' ')) ? 2 : 0);
		for (const QString &item : existing)
			length += item.size() + (item.contains(QLatin1Char(' ')) ? 2 : 0) + 1;
		return length <= 500;
	}
	void append()
	{
		const QString tag = input->text().trimmed();
		if (tag.isEmpty()) return;
		if (!valid(tag, values)) {
			hint->setText(strict ? QStringLiteral("Не более 10 тегов, 25 букв/цифр в каждом, без пробелов и повторов") :
				QStringLiteral("Максимум 500 символов суммарно, повторяющиеся теги запрещены"));
			return;
		}
		values.append(tag);
		input->clear();
		refresh();
		if (onChanged) onChanged(values);
	}
	void refresh()
	{
		while (auto *item = chips->takeAt(0)) {
			if (auto *widget = item->widget()) widget->deleteLater();
			delete item;
		}
		for (int i = 0; i < values.size(); ++i) {
			const QString tag = values.at(i);
			auto *chip = new QPushButton(tag + QStringLiteral(" ×"), this);
			chip->setToolTip(QStringLiteral("Удалить тег: ") + tag);
			chip->setMaximumWidth(190);
			chips->addWidget(chip, i / 2, i % 2);
			connect(chip, &QPushButton::clicked, this, [this, tag] {
				values.removeAll(tag);
				refresh();
				if (onChanged) onChanged(values);
			});
		}
		refreshHint();
	}
	void refreshHint()
	{
		if (strict) {
			hint->setText(QStringLiteral("%1/10 тегов · %2/25 символов · без пробелов")
				.arg(values.size()).arg(input->text().size()));
		} else {
			int total = 0;
			for (const QString &tag : values) total += tag.size() + (tag.contains(QLatin1Char(' ')) ? 2 : 0) + 1;
			if (!values.isEmpty()) --total;
			hint->setText(QStringLiteral("%1/500 символов · теги видео YouTube, можно с пробелами").arg(total));
		}
	}

	bool strict = false;
	QLineEdit *input = nullptr;
	QGridLayout *chips = nullptr;
	QLabel *hint = nullptr;
	QStringList values;
	std::function<void(const QStringList &)> onChanged;
};

} // namespace xinkx
