/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_details_widgets.h"

#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/event_filter.h"
#include "base/timer_rpl.h"
#include "base/unixtime.h"
#include "boxes/peers/edit_contact_box.h"
#include "boxes/star_gift_box.h"
#include "data/business/data_business_info.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "info/profile/info_profile_text.h"
#include "info/profile/info_profile_values.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/effects/toggle_arrow.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_info_profile_actions.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_window.h" // mainMenuToggleFourStrokes.

namespace Info::Profile {
namespace {

constexpr auto kDay = Data::WorkingInterval::kDay;

[[nodiscard]] bool AreNonTrivialHours(const Data::WorkingHours &hours) {
	if (!hours) {
		return false;
	}
	const auto &intervals = hours.intervals.list;
	for (auto i = 0; i != 7; ++i) {
		const auto day = Data::WorkingInterval{ i * kDay, (i + 1) * kDay };
		for (const auto &interval : intervals) {
			const auto intersection = interval.intersected(day);
			if (intersection && intersection != day) {
				return true;
			}
		}
	}
	return false;
}

[[nodiscard]] TimeId OpensIn(
		const Data::WorkingIntervals &intervals,
		TimeId now) {
	using namespace Data;

	while (now < 0) {
		now += WorkingInterval::kWeek;
	}
	while (now > WorkingInterval::kWeek) {
		now -= WorkingInterval::kWeek;
	}
	auto closest = WorkingInterval::kWeek;
	for (const auto &interval : intervals.list) {
		if (interval.start <= now && interval.end > now) {
			return TimeId(0);
		} else if (interval.start > now && interval.start - now < closest) {
			closest = interval.start - now;
		} else if (interval.start < now) {
			const auto next = interval.start + WorkingInterval::kWeek - now;
			if (next < closest) {
				closest = next;
			}
		}
	}
	return closest;
}

[[nodiscard]] rpl::producer<QString> OpensInText(
		rpl::producer<TimeId> in,
		rpl::producer<bool> hoursExpanded,
		rpl::producer<QString> fallback) {
	return rpl::combine(
		std::move(in),
		std::move(hoursExpanded),
		std::move(fallback)
	) | rpl::map([](TimeId in, bool hoursExpanded, QString fallback) {
		return (!in || hoursExpanded)
			? std::move(fallback)
			: (in >= 86400)
			? tr::lng_info_hours_opens_in_days(tr::now, lt_count, in / 86400)
			: (in >= 3600)
			? tr::lng_info_hours_opens_in_hours(tr::now, lt_count, in / 3600)
			: tr::lng_info_hours_opens_in_minutes(
				tr::now,
				lt_count,
				std::max(in / 60, 1));
	});
}

[[nodiscard]] QString FormatDayTime(TimeId time) {
	const auto wrap = [](TimeId value) {
		const auto hours = value / 3600;
		const auto minutes = (value % 3600) / 60;
		return QString::number(hours).rightJustified(2, u'0')
			+ ':'
			+ QString::number(minutes).rightJustified(2, u'0');
	};
	return (time > kDay)
		? tr::lng_info_hours_next_day(tr::now, lt_time, wrap(time - kDay))
		: wrap(time == kDay ? 0 : time);
}

[[nodiscard]] QString JoinIntervals(const Data::WorkingIntervals &data) {
	auto result = QStringList();
	result.reserve(data.list.size());
	for (const auto &interval : data.list) {
		const auto start = FormatDayTime(interval.start);
		const auto end = FormatDayTime(interval.end);
		result.push_back(start + u" - "_q + end);
	}
	return result.join('\n');
}

[[nodiscard]] QString FormatDayHours(
		const Data::WorkingHours &hours,
		const Data::WorkingIntervals &mine,
		bool my,
		int day) {
	using namespace Data;

	const auto local = ExtractDayIntervals(hours.intervals, day);
	if (IsFullOpen(local)) {
		return tr::lng_info_hours_open_full(tr::now);
	}
	const auto use = my ? ExtractDayIntervals(mine, day) : local;
	if (!use) {
		return tr::lng_info_hours_closed(tr::now);
	}
	return JoinIntervals(use);
}

[[nodiscard]] Data::WorkingIntervals ShiftedIntervals(
		Data::WorkingIntervals intervals,
		int delta) {
	auto &list = intervals.list;
	if (!delta || list.empty()) {
		return { std::move(list) };
	}
	for (auto &interval : list) {
		interval.start += delta;
		interval.end += delta;
	}
	while (list.front().start < 0) {
		constexpr auto kWeek = Data::WorkingInterval::kWeek;
		const auto first = list.front();
		if (first.end > 0) {
			list.push_back({ first.start + kWeek, kWeek });
			list.front().start = 0;
		} else {
			list.push_back(first.shifted(kWeek));
			list.erase(list.begin());
		}
	}
	return intervals.normalized();
}

void DeleteContactNote(
		not_null<UserData*> user,
		Fn<void(const QString &)> showError = nullptr) {
	user->session().api().request(MTPcontacts_UpdateContactNote(
		user->inputUser(),
		MTP_textWithEntities(MTP_string(), MTP_vector<MTPMessageEntity>())
	)).done([=] {
		user->setNote(TextWithEntities());
	}).fail([=](const MTP::Error &error) {
		if (showError) {
			showError(error.description());
		}
	}).send();
}

} // namespace

[[nodiscard]] object_ptr<Ui::SlideWrap<>> CreateWorkingHours(
		not_null<QWidget*> parent,
		not_null<UserData*> user) {
	using namespace Data;

	auto result = object_ptr<Ui::SlideWrap<Ui::RoundButton>>(
		parent,
		object_ptr<Ui::RoundButton>(
			parent,
			rpl::single(QString()),
			st::infoHoursOuter),
		st::infoProfileLabeledPadding - st::infoHoursOuterMargin);
	const auto button = result->entity();
	button->setTextTransform(Ui::RoundButtonTextTransform::ToUpper);
	const auto inner = Ui::CreateChild<Ui::VerticalLayout>(button);
	button->widthValue() | rpl::on_next([=](int width) {
		const auto margin = st::infoHoursOuterMargin;
		inner->resizeToWidth(width - margin.left() - margin.right());
		inner->move(margin.left(), margin.top());
	}, inner->lifetime());
	inner->heightValue() | rpl::on_next([=](int height) {
		const auto margin = st::infoHoursOuterMargin;
		height += margin.top() + margin.bottom();
		button->resize(button->width(), height);
	}, inner->lifetime());

	const auto info = &user->owner().businessInfo();

	struct State {
		rpl::variable<WorkingHours> hours;
		rpl::variable<TimeId> time;
		rpl::variable<int> day;
		rpl::variable<int> timezoneDelta;

		rpl::variable<WorkingIntervals> mine;
		rpl::variable<WorkingIntervals> mineByDays;
		rpl::variable<TimeId> opensIn;
		rpl::variable<bool> opened;
		rpl::variable<bool> expanded;
		rpl::variable<bool> nonTrivial;
		rpl::variable<bool> myTimezone;

		rpl::event_stream<> recounts;
	};
	const auto state = inner->lifetime().make_state<State>();

	auto recounts = state->recounts.events_starting_with_copy(rpl::empty);
	const auto recount = [=] {
		state->recounts.fire({});
	};

	state->hours = user->session().changes().peerFlagsValue(
		user,
		PeerUpdate::Flag::BusinessDetails
	) | rpl::map([=] {
		return user->businessDetails().hours;
	});
	state->nonTrivial = state->hours.value() | rpl::map(AreNonTrivialHours);

	const auto seconds = QTime::currentTime().msecsSinceStartOfDay() / 1000;
	const auto inMinute = seconds % 60;
	const auto firstTick = inMinute ? (61 - inMinute) : 1;
	state->time = rpl::single(rpl::empty) | rpl::then(
		base::timer_once(firstTick * crl::time(1000))
	) | rpl::then(
		base::timer_each(60 * crl::time(1000))
	) | rpl::map([] {
		const auto local = QDateTime::currentDateTime();
		const auto day = local.date().dayOfWeek() - 1;
		const auto seconds = local.time().msecsSinceStartOfDay() / 1000;
		return day * kDay + seconds;
	});

	state->day = state->time.value() | rpl::map([](TimeId time) {
		return time / kDay;
	});
	state->timezoneDelta = rpl::combine(
		state->hours.value(),
		info->timezonesValue()
	) | rpl::filter([](
			const WorkingHours &hours,
			const Timezones &timezones) {
		return ranges::contains(
			timezones.list,
			hours.timezoneId,
			&Timezone::id);
	}) | rpl::map([](WorkingHours &&hours, const Timezones &timezones) {
		const auto &list = timezones.list;
		const auto closest = FindClosestTimezoneId(list);
		const auto i = ranges::find(list, closest, &Timezone::id);
		const auto j = ranges::find(list, hours.timezoneId, &Timezone::id);
		Assert(i != end(list));
		Assert(j != end(list));
		return i->utcOffset - j->utcOffset;
	});

	state->mine = rpl::combine(
		state->hours.value(),
		state->timezoneDelta.value()
	) | rpl::map([](WorkingHours &&hours, int delta) {
		return ShiftedIntervals(hours.intervals, delta);
	});

	state->opensIn = rpl::combine(
		state->mine.value(),
		state->time.value()
	) | rpl::map([](const WorkingIntervals &mine, TimeId time) {
		return OpensIn(mine, time);
	});
	state->opened = state->opensIn.value() | rpl::map(rpl::mappers::_1 == 0);

	state->mineByDays = rpl::combine(
		state->hours.value(),
		state->timezoneDelta.value()
	) | rpl::map([](WorkingHours &&hours, int delta) {
		auto full = std::array<bool, 7>();
		auto withoutFullDays = hours.intervals;
		for (auto i = 0; i != 7; ++i) {
			if (IsFullOpen(ExtractDayIntervals(hours.intervals, i))) {
				full[i] = true;
				withoutFullDays = ReplaceDayIntervals(
					withoutFullDays,
					i,
					Data::WorkingIntervals());
			}
		}
		auto result = ShiftedIntervals(withoutFullDays, delta);
		for (auto i = 0; i != 7; ++i) {
			if (full[i]) {
				result = ReplaceDayIntervals(
					result,
					i,
					Data::WorkingIntervals{ { { 0, kDay } } });
			}
		}
		return result;
	});

	const auto dayHoursText = [=](int day) {
		return rpl::combine(
			state->hours.value(),
			state->mineByDays.value(),
			state->myTimezone.value()
		) | rpl::map([=](
				const WorkingHours &hours,
				const WorkingIntervals &mine,
				bool my) {
			return FormatDayHours(hours, mine, my, day);
		});
	};
	const auto dayHoursTextValue = [=](rpl::producer<int> day) {
		return std::move(day)
			| rpl::map(dayHoursText)
			| rpl::flatten_latest();
	};

	const auto openedWrap = inner->add(object_ptr<Ui::RpWidget>(inner));
	const auto opened = Ui::CreateChild<Ui::FlatLabel>(
		openedWrap,
		rpl::conditional(
			state->opened.value(),
			tr::lng_info_work_open(),
			tr::lng_info_work_closed()
		) | rpl::after_next(recount),
		st::infoHoursState);
	opened->setAttribute(Qt::WA_TransparentForMouseEvents);
	const auto timing = Ui::CreateChild<Ui::FlatLabel>(
		openedWrap,
		OpensInText(
			state->opensIn.value(),
			state->expanded.value(),
			dayHoursTextValue(state->day.value())
		) | rpl::after_next(recount),
		st::infoHoursValue);
	const auto timingArrow = Ui::CreateChild<Ui::RpWidget>(openedWrap);
	timingArrow->resize(Size(timing->st().style.font->height));
	timing->setAttribute(Qt::WA_TransparentForMouseEvents);
	state->opened.value() | rpl::on_next([=](bool value) {
		opened->setTextColorOverride(value
			? st::boxTextFgGood->c
			: st::boxTextFgError->c);
	}, opened->lifetime());

	rpl::combine(
		openedWrap->widthValue(),
		opened->heightValue(),
		timing->sizeValue()
	) | rpl::on_next([=](int width, int h1, QSize size) {
		opened->moveToLeft(0, 0, width);
		timingArrow->moveToRight(0, 0, width);
		timing->moveToRight(timingArrow->width(), 0, width);

		const auto margins = opened->getMargins();
		const auto added = margins.top() + margins.bottom();
		openedWrap->resize(width, std::max(h1, size.height()) - added);
	}, openedWrap->lifetime());

	rpl::combine(
		state->opened.value(),
		state->opensIn.value(),
		state->expanded.value(),
		dayHoursTextValue(state->day.value())
	) | rpl::on_next([=](
			bool opened,
			TimeId opensIn,
			bool expanded,
			const QString &timing) {
		const auto status = (opened
			? tr::lng_info_work_open
			: tr::lng_info_work_closed)(tr::now);
		const auto when = (!opensIn || expanded)
			? timing
			: (opensIn >= 86400)
			? tr::lng_info_hours_opens_in_days(tr::now, lt_count, opensIn / 86400)
			: (opensIn >= 3600)
			? tr::lng_info_hours_opens_in_hours(tr::now, lt_count, opensIn / 3600)
			: tr::lng_info_hours_opens_in_minutes(
				tr::now,
				lt_count,
				std::max(opensIn / 60, 1));
		button->setAccessibleName(
			tr::lng_info_hours_label(tr::now) + ": " + status + ", " + when);
	}, inner->lifetime());

	const auto labelWrap = inner->add(object_ptr<Ui::RpWidget>(inner));
	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		labelWrap,
		tr::lng_info_hours_label(),
		st::infoLabel);
	label->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto linkText = rpl::combine(
		state->nonTrivial.value(),
		state->hours.value(),
		state->mine.value(),
		state->myTimezone.value()
	) | rpl::map([=](
			bool complex,
			const WorkingHours &hours,
			const WorkingIntervals &mine,
			bool my) {
		return (!complex || hours.intervals == mine)
			? rpl::single(QString())
			: my
			? tr::lng_info_hours_my_time()
			: tr::lng_info_hours_local_time();
	}) | rpl::flatten_latest();
	const auto link = Ui::CreateChild<Ui::RoundButton>(
		labelWrap,
		std::move(linkText),
		st::defaultTableSmallButton);
	link->setClickedCallback([=] {
		state->myTimezone = !state->myTimezone.current();
		state->expanded = true;
	});

	rpl::combine(
		labelWrap->widthValue(),
		label->heightValue(),
		link->sizeValue()
	) | rpl::on_next([=](int width, int h1, QSize size) {
		label->moveToLeft(0, 0, width);
		link->moveToRight(0, 0, width);

		const auto margins = label->getMargins();
		const auto added = margins.top() + margins.bottom();
		labelWrap->resize(width, std::max(h1, size.height()) - added);
	}, labelWrap->lifetime());

	const auto other = inner->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			inner,
			object_ptr<Ui::VerticalLayout>(inner)));
	other->toggleOn(state->expanded.value(), anim::type::normal);
	constexpr auto kSlideDuration = float64(st::slideWrapDuration);
	other->setDuration(kSlideDuration);
	{
		const auto arrowAnimation
			= other->lifetime().make_state<Ui::Animations::Basic>();
		arrowAnimation->init([=] {
			timingArrow->update();
			if (!other->animating()) {
				arrowAnimation->stop();
			}
		});
		timingArrow->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(timingArrow);
			const auto progress = other->animating()
				? (crl::now() - arrowAnimation->started()) / kSlideDuration
				: 1.;

			const auto path = Ui::ToggleUpDownArrowPath(
				timingArrow->width() / 2,
				timingArrow->height() / 2,
				st::infoHoursArrowSize,
				st::mainMenuToggleFourStrokes,
				other->toggled() ? progress : 1 - progress);

			auto hq = PainterHighQualityEnabler(p);
			p.fillPath(path, timing->st().textFg);
		}, timingArrow->lifetime());
		state->expanded.value() | rpl::on_next([=] {
			arrowAnimation->start();
		}, other->lifetime());
	}

	other->finishAnimating();
	const auto days = other->entity();

	for (auto i = 1; i != 7; ++i) {
		const auto dayWrap = days->add(
			object_ptr<Ui::RpWidget>(other),
			QMargins(0, st::infoHoursDaySkip, 0, 0));
		auto label = state->day.value() | rpl::map([=](int day) {
			switch ((day + i) % 7) {
			case 0: return tr::lng_hours_monday();
			case 1: return tr::lng_hours_tuesday();
			case 2: return tr::lng_hours_wednesday();
			case 3: return tr::lng_hours_thursday();
			case 4: return tr::lng_hours_friday();
			case 5: return tr::lng_hours_saturday();
			case 6: return tr::lng_hours_sunday();
			}
			Unexpected("Index in working hours.");
		}) | rpl::flatten_latest();
		const auto dayLabel = Ui::CreateChild<Ui::FlatLabel>(
			dayWrap,
			std::move(label),
			st::infoHoursDayLabel);
		dayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
		const auto dayHours = Ui::CreateChild<Ui::FlatLabel>(
			dayWrap,
			dayHoursTextValue(state->day.value()
				| rpl::map((rpl::mappers::_1 + i) % 7)),
			st::infoHoursValue);
		dayHours->setAttribute(Qt::WA_TransparentForMouseEvents);
		rpl::combine(
			dayWrap->widthValue(),
			dayLabel->heightValue(),
			dayHours->sizeValue()
		) | rpl::on_next([=](int width, int h1, QSize size) {
			dayLabel->moveToLeft(0, 0, width);
			dayHours->moveToRight(0, 0, width);

			const auto margins = dayLabel->getMargins();
			const auto added = margins.top() + margins.bottom();
			dayWrap->resize(width, std::max(h1, size.height()) - added);
		}, dayWrap->lifetime());
	}

	button->setClickedCallback([=] {
		state->expanded = !state->expanded.current();
	});

	result->toggleOn(state->hours.value(
	) | rpl::map([](const WorkingHours &data) {
		return bool(data);
	}));

	return result;
}

[[nodiscard]] object_ptr<Ui::SlideWrap<>> CreateNotes(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<UserData*> user) {
	auto allNotesText = user->session().changes().peerFlagsValue(
		user,
		Data::PeerUpdate::Flag::FullInfo
	) | rpl::map([=] {
		return user->note();
	});

	auto notesText = rpl::duplicate(
		allNotesText
	) | rpl::filter([](const TextWithEntities &note) {
		return !note.text.isEmpty();
	});

	auto result = object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		parent,
		object_ptr<Ui::VerticalLayout>(parent));
	result->toggleOn(rpl::duplicate(
		allNotesText
	) | rpl::map([](const TextWithEntities &note) {
		return !note.text.isEmpty();
	}));
	result->finishAnimating();

	const auto notesContainer = result->entity();

	const auto context = Ui::Text::MarkedContext{
		.customEmojiFactory = user->owner().customEmojiManager().factory(
			Data::CustomEmojiManager::SizeTag::Normal)
	};

	auto notesLine = CreateTextWithLabel(
		notesContainer,
		tr::lng_info_notes_label(TextWithEntities::Simple),
		rpl::duplicate(notesText),
		st::infoLabel,
		st::infoLabeled,
		st::infoProfileLabeledPadding);

	std::move(
		notesText
	) | rpl::on_next([=, raw = notesLine.text](
			TextWithEntities note) {
		TextUtilities::ParseEntities(note, TextParseLinks);
		raw->setMarkedText(note, context);
	}, notesLine.text->lifetime());

	notesLine.text->setContextMenuHook([=, raw = notesLine.text](
			Ui::FlatLabel::ContextMenuRequest request) {
		raw->fillContextMenu(request);
		const auto addAction = Ui::Menu::CreateAddActionCallback(
			request.menu);
		addAction({
			.text = tr::lng_edit_note(tr::now),
			.handler = [=] {
				controller->window().show(
					Box(EditContactNoteBox, controller, user));
			},
		});
		addAction({
			.text = tr::lng_delete_note(tr::now),
			.handler = [=] {
				DeleteContactNote(user, [=](const QString &error) {
					controller->showToast(error);
				});
			},
			.isAttention = true,
		});
	});

	rpl::merge(
		notesLine.wrap->events(),
		notesLine.subtext->events()
	) | rpl::on_next([=, raw = notesLine.text](not_null<QEvent*> e) {
		if (e->type() == QEvent::ContextMenu) {
			const auto ce = static_cast<QContextMenuEvent*>(e.get());
			QCoreApplication::postEvent(
				raw,
				new QContextMenuEvent(
					ce->reason(),
					ce->pos(),
					ce->globalPos()));
		}
	}, notesLine.wrap->lifetime());

	const auto subtextLabel = Ui::CreateChild<Ui::FlatLabel>(
		notesLine.wrap->entity(),
		tr::lng_info_notes_private(tr::now),
		st::infoLabel);
	subtextLabel->setAttribute(Qt::WA_TransparentForMouseEvents);

	rpl::combine(
		notesLine.wrap->entity()->widthValue(),
		notesLine.subtext->geometryValue()
	) | rpl::on_next([=, skip = st::lineWidth * 5](
			int width,
			const QRect &subtextGeometry) {
		subtextLabel->moveToRight(
			0,
			subtextGeometry.y() + skip,
			width);
	}, subtextLabel->lifetime());

	notesContainer->add(std::move(notesLine.wrap));

	return result;
}

[[nodiscard]] object_ptr<Ui::SlideWrap<>> CreateBirthday(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<UserData*> user) {
	using namespace Data;

	auto result = object_ptr<Ui::SlideWrap<Ui::RoundButton>>(
		parent,
		object_ptr<Ui::RoundButton>(
			parent,
			rpl::single(QString()),
			st::infoHoursOuter),
		st::infoProfileLabeledPadding - st::infoHoursOuterMargin);
	result->setDuration(st::infoSlideDuration);
	const auto button = result->entity();
	button->setTextTransform(Ui::RoundButtonTextTransform::ToUpper);

	auto outer = Ui::CreateChild<Ui::SlideWrap<Ui::VerticalLayout>>(
		button,
		object_ptr<Ui::VerticalLayout>(button),
		st::infoHoursOuterMargin);
	const auto layout = outer->entity();
	layout->setAttribute(Qt::WA_TransparentForMouseEvents);

	auto birthday = BirthdayValue(
		user
	) | rpl::start_spawning(result->lifetime());

	auto label = BirthdayLabelText(rpl::duplicate(birthday));
	auto text = BirthdayValueText(
		rpl::duplicate(birthday)
	) | rpl::map(tr::marked);

	const auto giftIcon = Ui::CreateChild<Ui::RpWidget>(layout);
	giftIcon->resize(st::birthdayTodayIcon.size());
	layout->sizeValue() | rpl::on_next([=](QSize size) {
		giftIcon->moveToRight(
			0,
			(size.height() - giftIcon->height()) / 2,
			size.width());
	}, giftIcon->lifetime());
	giftIcon->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(giftIcon);
		st::birthdayTodayIcon.paint(p, 0, 0, giftIcon->width());
	}, giftIcon->lifetime());

	rpl::duplicate(
		birthday
	) | rpl::map([](Data::Birthday value) {
		return Data::IsBirthdayTodayValue(value);
	}) | rpl::flatten_latest(
	) | rpl::distinct_until_changed(
	) | rpl::on_next([=](bool today) {
		const auto disable = !today && user->session().premiumCanBuy();
		button->setDisabled(disable);
		button->setAttribute(Qt::WA_TransparentForMouseEvents, disable);
		button->clearState();
		giftIcon->setVisible(!disable);
	}, result->lifetime());

	BirthdayValueText(
		rpl::duplicate(birthday),
		true
	) | rpl::on_next([=](const QString &accessibleText) {
		button->setAccessibleName(
			tr::lng_info_birthday_label(tr::now) + ": " + accessibleText);
	}, button->lifetime());

	auto nonEmptyText = std::move(
		text
	) | rpl::before_next([slide = result.data()](
			const TextWithEntities &value) {
		if (value.text.isEmpty()) {
			slide->hide(anim::type::normal);
		}
	}) | rpl::filter([](const TextWithEntities &value) {
		return !value.text.isEmpty();
	}) | rpl::after_next([slide = result.data()](
			const TextWithEntities &value) {
		slide->show(anim::type::normal);
	});
	layout->add(object_ptr<Ui::FlatLabel>(
		layout,
		std::move(nonEmptyText),
		st::birthdayLabeled));
	layout->add(Ui::CreateSkipWidget(layout, st::infoLabelSkip));
	layout->add(object_ptr<Ui::FlatLabel>(
		layout,
		std::move(
			label
		) | rpl::after_next([=] {
			layout->resizeToWidth(layout->widthNoMargins());
		}),
		st::birthdayLabel));
	result->finishAnimating();

	Ui::ResizeFitChild(button, outer);

	button->setClickedCallback([=] {
		if (!button->isDisabled()) {
			Ui::ShowStarGiftBox(controller, user);
		}
	});

	return result;
}

} // namespace Info::Profile
