/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_config.h"

#include "core/vim_keymap_options.h"
#include "core/vim_keymap_bindings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>

#include <algorithm>

namespace Core::VimKeymap {
namespace {

constexpr auto kMaxConfigBytes = 64 * 1024;

[[nodiscard]] QJsonValue JsonValue(const base::options::details::ValueType &value) {
	return v::match(value, [](const auto &value) { return QJsonValue(value); });
}

} // namespace

std::span<const ConfigOption> ConfigOptions() {
	static const auto result = std::vector<ConfigOption>{
		{ kOptionVimKeymap, u"Включает Vim-навигацию. false отключает Vim-обработчики, но не удаляет остальные настройки. Поля ввода сохраняют обычный набор текста."_q },
		{ kOptionVimKeymapEscapeClosesComposer, u"Разрешает Esc отменять ответ, пересылку и другое временное состояние редактора сообщения. Редактирование отменяется только из Vim view; Esc из insert сначала возвращает в view. Отдельные привязки отмены ответа и редактирования действуют независимо от этого переключателя."_q },
		{ kOptionVimKeymapScrollStep, u"Шаг одиночного нажатия клавиши прокрутки, в пикселях. Относится к чату и прокручиваемым модалкам; содержимое, которое помещается целиком, прокручивать некуда."_q, ScrollStepBounds() },
		{ kOptionVimKeymapHoldScrollSpeed, u"Скорость удерживаемой прокрутки чата, в пикселях в секунду. Не является длительностью анимации или шагом одиночного нажатия."_q, HoldScrollSpeedBounds() },
		{ kOptionVimKeymapHintSize, u"Размер шрифта буквенных подсказок, в пунктах QFont. Большие значения требуют больше места возле целей."_q, HintSizeBounds() },
		{ kOptionVimKeymapHintAlphabet, u"Алфавит меток: russian: русские буквы, english: латинские буквы. Не меняет раскладку клавиатуры или привязки команд. Метки удлиняются только при нехватке коротких."_q, {}, { u"russian"_q, u"english"_q } },
		{ kOptionVimKeymapComposeCursorStyle, u"Форма курсора в поле сообщения в режиме просмотра: block: блок символа, bar: вертикальная черта, underline: подчёркивание. Не меняет системный курсор в режиме ввода."_q, {}, { u"block"_q, u"bar"_q, u"underline"_q } },
		{ kOptionVimKeymapComposeCursorWidth, u"Ширина курсора поля сообщения, в пикселях. Блочный курсор подстраивается под символ; эта величина не задаёт ширину букв текста."_q, ComposeCursorWidthBounds() },
		{ kOptionVimKeymapComposeCursorHeight, u"Высота курсора поля сообщения, в процентах высоты строки. Не изменяет размер самого текста."_q, ComposeCursorHeightBounds() },
		{ kOptionVimKeymapComposeCursorBlink, u"Интервал мигания курсора поля сообщения, в миллисекундах. 0 отключает мигание и оставляет курсор видимым постоянно."_q, ComposeCursorBlinkBounds() },
		{ kOptionVimKeymapKeyToggleMode, u"Переключение режима ввода и режима просмотра. Esc сначала обрабатывает открытые подсказки, вложенные окна и отмену состояния редактора; это не безусловное закрытие всего интерфейса."_q },
		{ kOptionVimKeymapKeyCancelReply, u"Отмена текущего ответа на сообщение. При назначении Esc отмена ответа имеет приоритет перед переключением режима."_q },
		{ kOptionVimKeymapKeyCancelEdit, u"Отмена редактирования отправленного сообщения. Пустая строка отключает отдельную команду, но общий переключатель обработки Esc может по-прежнему отменять редактирование."_q },
		{ kOptionVimKeymapKeyHelp, u"Открывает краткую справку по командам Vim и журнал последних клавиш. Полное руководство находится в файле docs/vim-keymap.md и Wiki проекта."_q },
		{ kOptionVimKeymapKeyScrollDown, u"Прокрутка вниз в режиме просмотра. В модалках действует вне редактируемых полей независимо от режима чата за окном. В поле ввода j остаётся буквой."_q },
		{ kOptionVimKeymapKeyScrollUp, u"Прокрутка вверх; правила режима и полей ввода такие же, как у прокрутки вниз."_q },
		{ kOptionVimKeymapKeyJumpBottom, u"Выполняет действие нижней угловой кнопки чата: переход к актуальной позиции или возврат по истории переходов. Это не просто прокрутка до конца загруженных сообщений."_q },
		{ kOptionVimKeymapKeyCopyMessage, u"Показывает метки для копирования сообщений. Выбранное сообщение копируется с учётом ограничений чата; фотографии копируются как изображение после загрузки. В альбоме отдельная метка у каждой фотографии. В выделенном тексте y имеет свою контекстную роль."_q },
		{ kOptionVimKeymapKeySelectMessageText, u"Показывает метки текстовых сообщений. После выбора можно двигаться по тексту без ввода, включить visual клавишей v и скопировать выделение клавишей y."_q },
		{ kOptionVimKeymapKeyReplyToMessage, u"Показывает метки сообщений для ответа. Выбор метки открывает ответ на конкретное сообщение."_q },
		{ kOptionVimKeymapKeyEditMessage, u"Показывает метки сообщений, которые разрешено редактировать. Обычно это доступные для редактирования собственные сообщения."_q },
		{ kOptionVimKeymapKeyDeleteMessage, u"Показывает метки сообщений для удаления. Выбор ведёт к штатному действию Telegram с его проверками и подтверждением; JSON не обходит ограничения доступа."_q },
		{ kOptionVimKeymapKeyFocusHints, u"Показывает метки ссылок, медиа и элементов основного чата, включая верхнюю панель воспроизведения. Боковая панель профиля не добавляет метки в чат. Кнопка получает фокус, Enter активирует её. Вложенные окна ограничивают набор целей своим содержимым."_q },
		{ kOptionVimKeymapKeyOpenChatHints, u"Показывает метки видимых чатов для обычного открытия. В отличие от предпросмотра, обычное открытие может отметить чат прочитанным."_q },
		{ kOptionVimKeymapKeyChatPreview, u"Показывает метки чатов для предпросмотра без прочтения. На macOS физический Ctrl+V отличается от Cmd+V: Cmd+V остаётся вставкой из буфера."_q },
		{ kOptionVimKeymapKeySearch, u"Открывает поиск в режиме просмотра. Внутри перемещения или выделения текста символ / имеет контекстную роль начала строки; привязка не отменяет эту роль."_q },
		{ kOptionVimKeymapKeyGlobalSearch, u"Фокусирует глобальный поиск. Можно перечислить варианты для обеих раскладок и модификаторов; в активном поле поиска текстовые буквы не используются для навигации по результатам."_q },
		{ kOptionVimKeymapKeyUndo, u"Отменяет изменение текста в редакторе сообщения в режиме просмотра. Не отменяет отправку, удаление сообщения или действия над чатом."_q },
		{ kOptionVimKeymapKeyRedo, u"Повторяет отменённое изменение текста редактора сообщения. Не является повторной отправкой сообщения."_q },
		{ kOptionVimKeymapKeyNextChat, u"Переход к следующему чату в текущем списке. Не меняет папку; Shift может расширять шаг там, где это поддерживает навигация чатов."_q },
		{ kOptionVimKeymapKeyPreviousChat, u"Переход к предыдущему чату в текущем списке. Позиция вычисляется относительно выбранного чата."_q },
		{ kOptionVimKeymapKeyNextFolder, u"Следующая папка чатов в соответствующем контексте. В модалках Tab сохраняет приоритет обхода элементов интерфейса, а в панелях может переключать их вкладки."_q },
		{ kOptionVimKeymapKeyPreviousFolder, u"Предыдущая папка чатов. Shift+Tab в модалках обходит доступные элементы в обратном порядке."_q },
		{ kOptionVimKeymapKeyEmojiPanel, u"Открывает панель эмодзи и стикеров. По умолчанию отдельная команда отключена, чтобы не конфликтовать с другими сочетаниями."_q },
		{ kOptionVimKeymapKeyFocusChat, u"Возвращает фокус из панели эмодзи и стикеров в чат. В просмотрщике медиа и истории страниц Ctrl+H может выполнять контекстное действие влево или назад."_q },
		{ kOptionVimKeymapKeyFocusEmoji, u"Переносит фокус в панель эмодзи и стикеров. В других контекстах Ctrl+L может выполнять действие вправо или вперёд."_q },
		{ kOptionVimKeymapKeyCall, u"Вызывает штатное действие звонка из текущего чата. Доступность и дальнейшие подтверждения определяются Telegram."_q },
	};
	return result;
}

QJsonObject ConfigSnapshot(bool defaults) {
	auto result = QJsonObject();
	for (const auto &field : ConfigOptions()) {
		const auto &option = base::options::details::Lookup(field.id);
		result.insert(option.id(), JsonValue(defaults ? option.defaultValue() : option.value()));
	}
	return result;
}

QJsonObject ConfigSchema() {
	auto properties = QJsonObject();
	for (const auto &field : ConfigOptions()) {
		const auto &option = base::options::details::Lookup(field.id);
		auto property = QJsonObject{
			{ u"title"_q, option.name() },
			{ u"description"_q, field.description },
			{ u"default"_q, JsonValue(option.defaultValue()) },
			{ u"type"_q, v::is<bool>(option.value()) ? u"boolean"_q
				: v::is<int>(option.value()) ? u"integer"_q : u"string"_q },
		};
		if (field.bounds) {
			property.insert(u"minimum"_q, field.bounds->min);
			property.insert(u"maximum"_q, field.bounds->max);
		} else if (!field.choices.empty()) {
			property.insert(u"enum"_q, QJsonArray::fromStringList(field.choices));
		} else if (v::is<QString>(option.value())) {
			property.insert(u"maxLength"_q, 1024);
		}
		properties.insert(option.id(), property);
	}
	return {
		{ u"$schema"_q, u"https://json-schema.org/draft/2020-12/schema"_q },
		{ u"title"_q, u"Telegravim Vim keymap"_q },
		{ u"type"_q, u"object"_q },
		{ u"additionalProperties"_q, false },
		{ u"properties"_q, properties },
	};
}

ConfigValidation ValidateConfig(const QByteArray &json) {
	if (json.size() > kMaxConfigBytes) {
		return { {}, u"Конфигурация превышает 64 КиБ."_q };
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError) {
		const auto before = QString::fromUtf8(json.left(error.offset));
		const auto line = before.count('\n') + 1;
		const auto column = before.size() - before.lastIndexOf('\n');
		return { {}, u"JSON, строка %1, столбец %2: %3"_q.arg(line).arg(column).arg(error.errorString()),
			int(line), int(column) };
	} else if (!document.isObject()) {
		return { {}, u"В корне JSON должен быть объект с ключами настроек Vim."_q };
	}
	const auto values = document.object();
	const auto fields = ConfigOptions();
	for (auto i = values.begin(); i != values.end(); ++i) {
		const auto field = std::find_if(fields.begin(), fields.end(), [&](const ConfigOption &field) {
			return i.key() == QLatin1String(field.id);
		});
		if (field == fields.end()) {
			return { {}, u"Неизвестный ключ: %1. Разрешены только параметры Vim из руководства."_q.arg(i.key()) };
		}
		const auto &current = base::options::details::Lookup(field->id).value();
		const auto &value = i.value();
		auto problem = QString();
		if (v::is<bool>(current)) {
			if (!value.isBool()) {
				problem = u"ожидается true или false"_q;
			}
		} else if (v::is<int>(current)) {
			if (!value.isDouble() || value.toDouble() != value.toInt()
				|| value.toInt() < field->bounds->min || value.toInt() > field->bounds->max) {
				problem = u"ожидается целое число от %1 до %2"_q.arg(field->bounds->min).arg(field->bounds->max);
			}
		} else if (!value.isString()) {
			problem = u"ожидается строка"_q;
		} else if (!field->choices.empty()) {
			if (!field->choices.contains(value.toString())) {
				problem = u"допустимы только: %1"_q.arg(field->choices.join(u", "_q));
			}
		} else if (value.toString().size() > 1024 || !Bindings::ValidBindings(value.toString())) {
			problem = u"неверная запись сочетаний клавиш; максимум 1024 символа. См. руководство"_q;
		}
		if (!problem.isEmpty()) {
			return { {}, u"%1: %2."_q.arg(i.key(), problem) };
		}
	}
	return { values };
}

QString ApplyConfig(const QByteArray &json) {
	const auto validated = ValidateConfig(json);
	if (!validated.error.isEmpty()) {
		return validated.error;
	}
	if (!validated.values.isEmpty()) {
		VimKeymapDefaultsMigratedOption.set(true);
	}
	for (const auto &field : ConfigOptions()) {
		auto &option = base::options::details::Lookup(field.id);
		const auto value = validated.values.value(option.id());
		if (value.isUndefined()) {
			continue;
		} else if (value.isBool()) {
			option.set(value.toBool());
		} else if (value.isDouble()) {
			option.set(value.toInt());
		} else {
			option.set(value.toString());
		}
	}
	return {};
}
} // namespace Core::VimKeymap
