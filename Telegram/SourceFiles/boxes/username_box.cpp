/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/username_box.h"

#include "boxes/peers/edit_peer_usernames_list.h"
#include "base/timer.h"
#include "boxes/peers/edit_peer_common.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/text/text_variant.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/special_fields.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"
#include "styles/style_settings.h"

namespace {

struct CheckInfo final {
	enum class Type {
		Good,
		Error,
		Default,
		PurchaseAvailable,
	};
	Type type;
	v::text::data text;
};

class UsernameEditor final : public Ui::RpWidget {
public:
	UsernameEditor(not_null<Ui::RpWidget*>, not_null<Main::Session*> session);

	void setInnerFocus();
	[[nodiscard]] rpl::producer<> submitted() const;
	[[nodiscard]] rpl::producer<> save();
	[[nodiscard]] rpl::producer<CheckInfo> checkInfoChanged() const;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	void updateFail(const QString &error);
	void checkFail(const QString &error);

	void checkInfoPurchaseAvailable();

	void check();
	void changed();

	void checkInfoChange();

	QString getName() const;

	const not_null<Main::Session*> _session;
	const style::margins &_padding;
	MTP::Sender _api;

	object_ptr<Ui::UsernameInput> _username;

	mtpRequestId _saveRequestId = 0;
	mtpRequestId _checkRequestId = 0;
	QString _sentUsername, _checkUsername, _errorText, _goodText;

	base::Timer _checkTimer;

	rpl::event_stream<> _saved;
	rpl::event_stream<CheckInfo> _checkInfoChanged;

};

UsernameEditor::UsernameEditor(
	not_null<Ui::RpWidget*>,
	not_null<Main::Session*> session)
: _session(session)
, _padding(st::usernamePadding)
, _api(&_session->mtp())
, _username(
	this,
	st::defaultInputField,
	rpl::single(qsl("@username")),
	session->user()->editableUsername(),
	QString())
, _checkTimer([=] { check(); }) {
	_goodText = _session->user()->editableUsername().isEmpty()
		? QString()
		: tr::lng_username_available(tr::now);

	connect(_username, &Ui::MaskedInputField::changed, [=] { changed(); });

	resize(width(), (_padding.top() + _username->height()));
}

rpl::producer<> UsernameEditor::submitted() const {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		QObject::connect(
			_username,
			&Ui::MaskedInputField::submitted,
			[=] { consumer.put_next({}); });
		return lifetime;
	};
}

void UsernameEditor::setInnerFocus() {
	_username->setFocusFast();
}

void UsernameEditor::resizeEvent(QResizeEvent *e) {
	_username->resize(
		width() - _padding.left() - _padding.right(),
		_username->height());
	_username->moveToLeft(_padding.left(), _padding.top());
}

rpl::producer<> UsernameEditor::save() {
	if (_saveRequestId) {
		return _saved.events();
	}

	_sentUsername = getName();
	_saveRequestId = _api.request(MTPaccount_UpdateUsername(
		MTP_string(_sentUsername)
	)).done([=](const MTPUser &result) {
		_saveRequestId = 0;
		_session->data().processUser(result);
		_saved.fire_done();
	}).fail([=](const MTP::Error &error) {
		_saveRequestId = 0;
		updateFail(error.type());
	}).send();
	return _saved.events();
}

rpl::producer<CheckInfo> UsernameEditor::checkInfoChanged() const {
	return _checkInfoChanged.events();
}

void UsernameEditor::check() {
	_api.request(base::take(_checkRequestId)).cancel();

	const auto name = getName();
	if (name.size() < Ui::EditPeer::kMinUsernameLength) {
		return;
	}
	_checkUsername = name;
	_checkRequestId = _api.request(MTPaccount_CheckUsername(
		MTP_string(name)
	)).done([=](const MTPBool &result) {
		_checkRequestId = 0;

		_errorText = (mtpIsTrue(result)
				|| _checkUsername == _session->user()->editableUsername())
			? QString()
			: tr::lng_username_occupied(tr::now);
		_goodText = _errorText.isEmpty()
			? tr::lng_username_available(tr::now)
			: QString();

		checkInfoChange();
	}).fail([=](const MTP::Error &error) {
		_checkRequestId = 0;
		checkFail(error.type());
	}).send();
}

void UsernameEditor::changed() {
	const auto name = getName();
	if (name.isEmpty()) {
		if (!_errorText.isEmpty() || !_goodText.isEmpty()) {
			_errorText = _goodText = QString();
			checkInfoChange();
		}
		_checkTimer.cancel();
	} else {
		const auto len = int(name.size());
		for (auto i = 0; i < len; ++i) {
			const auto ch = name.at(i);
			if ((ch < 'A' || ch > 'Z')
				&& (ch < 'a' || ch > 'z')
				&& (ch < '0' || ch > '9')
				&& ch != '_'
				&& (ch != '@' || i > 0)) {
				if (_errorText != tr::lng_username_bad_symbols(tr::now)) {
					_errorText = tr::lng_username_bad_symbols(tr::now);
					checkInfoChange();
				}
				_checkTimer.cancel();
				return;
			}
		}
		if (name.size() < Ui::EditPeer::kMinUsernameLength) {
			if (_errorText != tr::lng_username_too_short(tr::now)) {
				_errorText = tr::lng_username_too_short(tr::now);
				checkInfoChange();
			}
			_checkTimer.cancel();
		} else {
			if (!_errorText.isEmpty() || !_goodText.isEmpty()) {
				_errorText = _goodText = QString();
				checkInfoChange();
			}
			_checkTimer.callOnce(Ui::EditPeer::kUsernameCheckTimeout);
		}
	}
}

void UsernameEditor::checkInfoChange() {
	if (!_errorText.isEmpty()) {
		_checkInfoChanged.fire({
			.type = CheckInfo::Type::Error,
			.text = _errorText,
		});
	} else if (!_goodText.isEmpty()) {
		_checkInfoChanged.fire({
			.type = CheckInfo::Type::Good,
			.text = _goodText,
		});
	} else {
		_checkInfoChanged.fire({
			.type = CheckInfo::Type::Default,
			.text = tr::lng_username_choose(tr::now),
		});
	}
}

void UsernameEditor::checkInfoPurchaseAvailable() {
	constexpr auto kUsernameAuction = "auction";
	const auto text = tr::lng_username_purchase_available(
		tr::now,
		lt_link,
		Ui::Text::Link(
			'@' + QString(kUsernameAuction),
			u"https://t.me/"_q + kUsernameAuction),
		Ui::Text::RichLangValue);
	_username->setFocus();
	_username->showError();
	_errorText = text.text;

	_checkInfoChanged.fire({
		.type = CheckInfo::Type::PurchaseAvailable,
		.text = text,
	});
}

void UsernameEditor::updateFail(const QString &error) {
	const auto self = _session->user();
	if ((error == qstr("USERNAME_NOT_MODIFIED"))
		|| (_sentUsername == self->editableUsername())) {
		self->setName(
			TextUtilities::SingleLine(self->firstName),
			TextUtilities::SingleLine(self->lastName),
			TextUtilities::SingleLine(self->nameOrPhone),
			TextUtilities::SingleLine(_sentUsername));
		_saved.fire_done();
	} else if (error == qstr("USERNAME_INVALID")) {
		_username->setFocus();
		_username->showError();
		_errorText = tr::lng_username_invalid(tr::now);
		checkInfoChange();
	} else if ((error == qstr("USERNAME_OCCUPIED"))
		|| (error == qstr("USERNAMES_UNAVAILABLE"))) {
		_username->setFocus();
		_username->showError();
		_errorText = tr::lng_username_occupied(tr::now);
		checkInfoChange();
	} else if (error == qstr("USERNAME_PURCHASE_AVAILABLE")) {
		checkInfoPurchaseAvailable();
	} else {
		_username->setFocus();
	}
}

void UsernameEditor::checkFail(const QString &error) {
	if (error == qstr("USERNAME_INVALID")) {
		_errorText = tr::lng_username_invalid(tr::now);
		checkInfoChange();
	} else if ((error == qstr("USERNAME_OCCUPIED"))
		&& (_checkUsername != _session->user()->editableUsername())) {
		_errorText = tr::lng_username_occupied(tr::now);
		checkInfoChange();
	} else if (error == qstr("USERNAME_PURCHASE_AVAILABLE")) {
		checkInfoPurchaseAvailable();
	} else {
		_goodText = QString();
		_username->setFocus();
	}
}

QString UsernameEditor::getName() const {
	return _username->text().replace('@', QString()).trimmed();
}

} // namespace

void UsernamesBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	box->setTitle(tr::lng_username_title());

	const auto container = box->verticalLayout();

	const auto editor = box->addRow(
		object_ptr<UsernameEditor>(box, session),
		{});
	box->setFocusCallback([=] { editor->setInnerFocus(); });

	{
		const auto padding = st::boxRowPadding;
		const auto &st = st::aboutRevokePublicLabel;
		const auto skip = (st::usernameSkip - st.style.font->height) / 2;

		box->addSkip(skip);
		const auto wrap = box->addRow(
			object_ptr<Ui::SlideWrap<Ui::RpWidget>>(
				box,
				object_ptr<Ui::RpWidget>(box)),
			padding);
		wrap->setMinimalHeight(st.style.font->height);

		const auto maxHeightWrap = wrap->entity();
		const auto label = Ui::CreateChild<Ui::FlatLabel>(
			maxHeightWrap,
			editor->checkInfoChanged(
			) | rpl::map([](CheckInfo info) {
				return v::text::take_marked(base::take(info.text));
			}) | rpl::flatten_latest(),
			st);
		label->heightValue(
		) | rpl::start_with_next([=](int height) {
			if (height > maxHeightWrap->height()) {
				maxHeightWrap->resize(maxHeightWrap->width(), height);
			}
		}, maxHeightWrap->lifetime());

		editor->checkInfoChanged(
		) | rpl::start_with_next([=](CheckInfo info) {
			const auto &color = (info.type == CheckInfo::Type::Good)
				? st::boxTextFgGood
				: (info.type == CheckInfo::Type::Error)
				? st::boxTextFgError
				: st::usernameDefaultFg;
			label->setTextColorOverride(color->c);
			label->resizeToWidth(container->width()
				- padding.left()
				- padding.right());

			wrap->toggle(
				info.type == CheckInfo::Type::PurchaseAvailable,
				anim::type::normal);
		}, label->lifetime());
		box->addSkip(skip);
	}

	container->add(object_ptr<Ui::DividerLabel>(
		container,
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_username_description(Ui::Text::RichLangValue),
			st::boxDividerLabel),
		st::settingsDividerLabelPadding));

	const auto list = box->addRow(
		object_ptr<UsernamesList>(
			box,
			session->user(),
			std::make_shared<Ui::BoxShow>(box),
			[=] {
				box->scrollToY(0);
				editor->setInnerFocus();
			}),
		{});

	const auto finish = [=] {
		list->save(
		) | rpl::start_with_done([=] {
			editor->save(
			) | rpl::start_with_done([=] {
				box->closeBox();
			}, box->lifetime());
		}, box->lifetime());
	};
	editor->submitted(
	) | rpl::start_with_next(finish, editor->lifetime());

	box->addButton(tr::lng_settings_save(), finish);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}
