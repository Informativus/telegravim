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

[[nodiscard]] QString JsonText(const QJsonObject &value) {
	return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Indented));
}

} // namespace

std::span<const ConfigOption> ConfigOptions() {
	static const auto result = std::vector<ConfigOption>{
		{ kOptionVimKeymap, u"Включает Vim-навигацию. false отключает Vim-обработчики, но не удаляет остальные настройки. Поля ввода сохраняют обычный набор текста."_q },
		{ kOptionVimKeymapEscapeClosesComposer, u"Разрешает Esc сначала отменить ответ, редактирование, пересылку и другое временное состояние редактора сообщения, а затем менять режим Vim. Отдельные привязки отмены ответа и редактирования действуют независимо от этого переключателя."_q },
		{ kOptionVimKeymapScrollStep, u"Шаг одиночного нажатия клавиши прокрутки, в пикселях. Относится к чату и прокручиваемым модалкам; содержимое, которое помещается целиком, прокручивать некуда."_q, ScrollStepBounds() },
		{ kOptionVimKeymapHoldScrollSpeed, u"Скорость удерживаемой прокрутки чата, в пикселях в секунду. Не является длительностью анимации или шагом одиночного нажатия."_q, HoldScrollSpeedBounds() },
		{ kOptionVimKeymapHintSize, u"Размер шрифта буквенных подсказок, в пунктах QFont. Большие значения требуют больше места возле целей."_q, HintSizeBounds() },
		{ kOptionVimKeymapHintAlphabet, u"Алфавит меток: russian: русские буквы, english: латинские буквы и знаки. Не меняет раскладку клавиатуры или привязки команд. Метки удлиняются только при нехватке коротких."_q, {}, { u"russian"_q, u"english"_q } },
		{ kOptionVimKeymapComposeCursorStyle, u"Форма курсора в поле сообщения в режиме просмотра: block: блок символа, bar: вертикальная черта, underline: подчёркивание. Не меняет системный курсор в режиме ввода."_q, {}, { u"block"_q, u"bar"_q, u"underline"_q } },
		{ kOptionVimKeymapComposeCursorWidth, u"Ширина курсора поля сообщения, в пикселях. Блочный курсор подстраивается под символ; эта величина не задаёт ширину букв текста."_q, ComposeCursorWidthBounds() },
		{ kOptionVimKeymapComposeCursorHeight, u"Высота курсора поля сообщения, в процентах высоты строки. Не изменяет размер самого текста."_q, ComposeCursorHeightBounds() },
		{ kOptionVimKeymapComposeCursorBlink, u"Интервал мигания курсора поля сообщения, в миллисекундах. 0 отключает мигание и оставляет курсор видимым постоянно."_q, ComposeCursorBlinkBounds() },
		{ kOptionVimKeymapKeyToggleMode, u"Переключение режима ввода и режима просмотра. Esc сначала обрабатывает открытые подсказки, вложенные окна и отмену состояния редактора; это не безусловное закрытие всего интерфейса."_q },
		{ kOptionVimKeymapKeyCancelReply, u"Отмена текущего ответа на сообщение. При назначении Esc отмена ответа имеет приоритет перед переключением режима."_q },
		{ kOptionVimKeymapKeyCancelEdit, u"Отмена редактирования отправленного сообщения. Пустая строка отключает отдельную команду, но общий переключатель обработки Esc может по-прежнему отменять редактирование."_q },
		{ kOptionVimKeymapKeyHelp, u"Открывает справку по командам Vim и журнал последних клавиш. Полное руководство по JSON доступно отдельной кнопкой на экране настроек."_q },
		{ kOptionVimKeymapKeyScrollDown, u"Прокрутка вниз в режиме просмотра. В модалках действует вне редактируемых полей независимо от режима чата за окном. В поле ввода j остаётся буквой."_q },
		{ kOptionVimKeymapKeyScrollUp, u"Прокрутка вверх; правила режима и полей ввода такие же, как у прокрутки вниз."_q },
		{ kOptionVimKeymapKeyJumpBottom, u"Выполняет действие нижней угловой кнопки чата: переход к актуальной позиции или возврат по истории переходов. Это не просто прокрутка до конца загруженных сообщений."_q },
		{ kOptionVimKeymapKeyCopyMessage, u"Показывает метки для копирования сообщений. Выбранное сообщение копируется с учётом ограничений чата; фотографии копируются как изображение после загрузки. В выделенном тексте y имеет свою контекстную роль."_q },
		{ kOptionVimKeymapKeySelectMessageText, u"Показывает метки текстовых сообщений. После выбора можно двигаться по тексту без ввода, включить visual клавишей v и скопировать выделение клавишей y."_q },
		{ kOptionVimKeymapKeyReplyToMessage, u"Показывает метки сообщений для ответа. Выбор метки открывает ответ на конкретное сообщение."_q },
		{ kOptionVimKeymapKeyEditMessage, u"Показывает метки сообщений, которые разрешено редактировать. Обычно это доступные для редактирования собственные сообщения."_q },
		{ kOptionVimKeymapKeyDeleteMessage, u"Показывает метки сообщений для удаления. Выбор ведёт к штатному действию Telegram с его проверками и подтверждением; JSON не обходит ограничения доступа."_q },
		{ kOptionVimKeymapKeyFocusHints, u"Показывает метки ссылок, медиа и доступных элементов интерфейса, включая верхнюю панель воспроизведения. Кнопка получает фокус, Enter активирует её. Вложенные окна ограничивают набор целей своим содержимым."_q },
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

QString ConfigGuide() {
	auto result = uR"(# Руководство по настройкам Vim keymap

## UI и JSON

Откройте «Настройки → Vim keymap». UI и JSON редактируют один набор настроек,
общий для аккаунтов этой установки Telegram. Отдельного режима исполнения
JSON нет. Настройки сохраняются штатным локальным механизмом Telegram;
серверу они не передаются, перезапуск для применения не нужен.

- **UI**: привычные переключатели и строки параметров. Изменения сохраняются сразу.
- **JSON**: текстовый редактор с полным снимком текущих значений, включая значения
  по умолчанию и отключённые сочетания. Сам ввод текста ничего не применяет.
- **Применить**: проверяет весь JSON и только после успешной проверки обновляет
  настройки. Неизвестные ключи, неверные типы, диапазоны и сочетания отклоняют
  весь документ. Ошибка показывает ключ либо строку и столбец; текст остаётся в редакторе.
- **Перечитать**: загружает действующие значения. При наличии правок запрашивает
  подтверждение их отбрасывания. Это не сброс к заводским значениям.
- **Руководство**: открывает этот документ внутри приложения, без браузера и сети.
- Переключение из JSON в UI, кнопки назад/закрыть и Esc не должны молча терять
  правки: подтвердите их отбрасывание либо вернитесь и нажмите «Применить».
  Принудительное завершение процесса не сохраняет черновик JSON.
- Если настройки изменены в другом окне во время редактирования JSON,
  применение останавливается. Скопируйте нужный текст, перечитайте действующие
  значения и внесите изменения заново, чтобы не перезаписать чужие правки.

## Формат

В корне находится обычный JSON-объект. Ключи совпадают с идентификаторами
настроек и чувствительны к регистру. Допустимы только ключи из справочника ниже.
Пропущенный ключ сохраняет действующее значение; `{}` ничего не сбрасывает.
`null` не означает сброс и не принимается. Для возврата параметра к стандартному
значению укажите его значение по умолчанию из справочника.

Комментарии, завершающие запятые, JSONC и JSON5 не поддерживаются. Не повторяйте
ключи: стандартный парсер Qt сохраняет последнее значение повторённого ключа.
Максимальный размер документа: 64 КиБ UTF-8. Внутренний маркер миграции
`vim-keymap-defaults-migrated` не является пользовательским параметром и в JSON
не допускается. Остальные экспериментальные настройки импорт не затрагивает.

Рядом с этим документом в репозитории находятся `vim-keymap.example.json`
(полный стандартный конфиг) и `vim-keymap.schema.json` (типы, диапазоны,
описания и значения по умолчанию). Схема не исполняет команды и не загружается
из сети. Приложение дополнительно проверяет синтаксис сочетаний клавиш.

## Сочетания клавиш

Все ключи `vim-keymap-key-*` принимают строку длиной до 1024 символов.
Несколько вариантов разделяются запятыми, например `"j, о"`.
Пустая строка `""` отключает конкретную настраиваемую команду.
Пробелы вокруг частей и регистр записи несущественны.

Модификаторы: `Ctrl`/`Control`, `Cmd`/`Command`/`Meta`, `Shift`, `Alt`/`Option`.
Комбинируйте их знаком `+`: `Ctrl+Shift+V`. Последняя часть должна быть одной
буквой/цифрой/знаком либо именем клавиши: `Esc`/`Escape`, `Tab`, `Backtab`,
`Enter`/`Return`, `Space`, `Backspace`, `Delete`, `Up`, `Down`, `Left`, `Right`,
`Home`, `End`, `PageUp`, `PageDown`. F1 и многоклавишные последовательности
вроде `gg` в этих полях не поддерживаются. Литералы `+` и `,` служат разделителями
и не задаются как отдельная клавиша. `Backtab`: Shift+Tab.

Указывайте варианты для русской и английской раскладок явно, как в стандартных
значениях. Алфавит меток не переводит привязки автоматически. На macOS для
предпросмотра чата используется физический Ctrl; Cmd+V сохраняет штатную вставку.
Некоторые другие команды исторически допускают эквивалентные модификаторы
Ctrl/Cmd: полная независимость этих модификаторов для всех команд не гарантируется.

Контекстные команды (например, `viw`, `y` в visual, `h/l` на ползунке, навигация
медиапросмотрщика) не превращаются в отдельные JSON-параметры. Совпадения
сочетаний допустимы: действие зависит от активного окна, поля и режима.
Не назначайте нескольким действиям одну клавишу в одном контексте, если
не хотите зависеть от приоритета обработчиков.

## Клавиатура редактора

В JSON-редакторе буквы, включая j/k/f/v, вводятся как текст. Cmd+V на macOS
вставляет JSON; Cmd+A выделяет документ; Cmd+Z отменяет правку текста, а не
настройки, уже сохранённые кнопкой «Применить». Tab/Shift+Tab переводят фокус
между вкладками, редактором и кнопками. Enter в редакторе добавляет строку,
Enter на кнопке выполняет её действие. Отступы в JSON можно вводить пробелами.
В руководстве можно выделять и копировать текст; j/k прокручивают документ.

## Справочник всех ключей

)"_q;
	const auto properties = ConfigSchema().value(u"properties"_q).toObject();
	for (const auto &field : ConfigOptions()) {
		const auto &option = base::options::details::Lookup(field.id);
		const auto schema = properties.value(option.id()).toObject();
		result += u"### `%1`\n\nUI: **%2**.\n\n%3\n\nТип: `%4`. "_q.arg(
			option.id(), option.name(), field.description, schema.value(u"type"_q).toString());
		if (field.bounds) {
			result += u"Диапазон: %1–%2. "_q.arg(field.bounds->min).arg(field.bounds->max);
		} else if (!field.choices.empty()) {
			result += u"Допустимые значения: `%1`. "_q.arg(field.choices.join(u"`, `"_q));
		} else if (v::is<QString>(option.value())) {
			result += u"Сочетания клавиш; пустая строка отключает команду. "_q;
		}
		result += u"Значение по умолчанию и пример:\n\n```json\n%1```\n\n"_q.arg(
			JsonText({ { option.id(), JsonValue(option.defaultValue()) } }));
	}
	return result;
}

} // namespace Core::VimKeymap
