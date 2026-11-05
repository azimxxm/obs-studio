#include "AudioFilterOrder.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QWidgetAction>
#include <QListWidget>
#include <QStringList>
#include <QMetaObject>

#include "moc_AudioFilterOrder.cpp"

namespace {
struct ChainStep {
	const char *id;
	int rank;
};

/* the order a voice-over engineer would use */
const ChainStep kChain[] = {
	{"invert_polarity_filter", 0},   {"narrator_rumble_filter", 1},   {"noise_suppress_filter", 2},
	{"narrator_studio_noise_filter", 2},
	{"noise_gate_filter", 2},            {"expander_filter", 2},        {"basic_eq_filter", 3},
	{"vst_filter", 3},                   {"vst3_filter", 3},            {"narrator_deess_filter", 4},
	{"compressor_filter", 5},            {"upward_compressor_filter", 5}, {"gain_filter", 6},
	{"limiter_filter", 7},
};
/* filters that do the same job: only one of them should run */
bool IsNoiseRemover(const char *id)
{
	return id && (std::strcmp(id, "noise_suppress_filter") == 0 ||
		      std::strcmp(id, "narrator_studio_noise_filter") == 0);
}

/* filters that make no sense twice in a voice chain */
bool MustRunOnce(const char *id)
{
	const char *once[] = {"narrator_rumble_filter", "narrator_deess_filter", "limiter_filter",
			      "noise_suppress_filter", "narrator_studio_noise_filter"};
	for (const char *candidate : once) {
		if (id && std::strcmp(id, candidate) == 0) {
			return true;
		}
	}
	return false;
}
/* why we turned a filter off, kept with the filter so the Filters window can explain it later */
constexpr const char *kReasonKey = "narrator_auto_off_reason";

void SetOffReason(obs_source_t *filter, const QString &reason)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(filter);
	obs_data_set_string(priv, kReasonKey, reason.toUtf8().constData());
}

QString OffReason(obs_source_t *filter)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(filter);
	return QString::fromUtf8(obs_data_get_string(priv, kReasonKey));
}

/* turns off every enabled filter that clashes with an earlier one in the chain */
QStringList TurnOffClashes(const std::vector<obs_source_t *> &chain)
{
	QStringList notices;
	std::vector<obs_source_t *> kept;
	for (obs_source_t *filter : chain) {
		if (!obs_source_enabled(filter)) {
			continue;
		}
		const char *id = obs_source_get_unversioned_id(filter);
		obs_source_t *clash = nullptr;
		for (obs_source_t *earlier : kept) {
			const char *earlierId = obs_source_get_unversioned_id(earlier);
			bool sameJob = IsNoiseRemover(id) && IsNoiseRemover(earlierId);
			bool sameFilter = MustRunOnce(id) && earlierId && std::strcmp(id, earlierId) == 0;
			if (sameJob || sameFilter) {
				clash = earlier;
				break;
			}
		}
		if (!clash) {
			kept.push_back(filter);
			continue;
		}
		QString reason = QObject::tr("Turned off automatically: \"%1\" already does this job, and two at once "
					     "make the voice sound worse. To use this one instead, turn it on with the "
					     "eye button and turn \"%1\" off.")
					 .arg(QString::fromUtf8(obs_source_get_name(clash)));
		obs_source_set_enabled(filter, false);
		SetOffReason(filter, reason);
		notices << reason;
	}
	return notices;
}
} // namespace

AudioFilterOrder::AudioFilterOrder(QObject *parent) : QObject(parent) {}

AudioFilterOrder::~AudioFilterOrder()
{
	addedSignal.Disconnect();
}

int AudioFilterOrder::Rank(const char *filterId)
{
	if (!filterId) {
		return -1;
	}
	for (const ChainStep &step : kChain) {
		if (std::strcmp(step.id, filterId) == 0) {
			return step.rank;
		}
	}
	return -1;
}

void AudioFilterOrder::Attach(obs_source_t *source)
{
	addedSignal.Disconnect();
	weakSource = OBSGetWeakRef(source);
	if (source) {
		addedSignal.Connect(obs_source_get_signal_handler(source), "filter_add", &AudioFilterOrder::FilterAdded,
				    this);
	}
}

/* libobs may send this from any thread; do the work on the UI thread */
void AudioFilterOrder::FilterAdded(void *data, calldata_t *params)
{
	auto *self = static_cast<AudioFilterOrder *>(data);
	obs_source_t *filter = static_cast<obs_source_t *>(calldata_ptr(params, "filter"));
	OBSWeakSource weakFilter = OBSGetWeakRef(filter);
	QMetaObject::invokeMethod(self, "PlaceFilter", Qt::QueuedConnection, Q_ARG(OBSWeakSource, weakFilter));
}

void AudioFilterOrder::PlaceFilter(OBSWeakSource weakFilter)
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	OBSSourceAutoRelease filter = obs_weak_source_get_source(weakFilter);
	if (!source || !filter) {
		return;
	}
	TurnOffDuplicates(source, filter);

	const int rank = Rank(obs_source_get_unversioned_id(filter));
	if (rank < 0) {
		return;
	}

	/* the filters as the Filters window shows them, top (runs first) to bottom */
	std::vector<obs_source_t *> shown;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *child, void *param) {
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(child);
		},
		&shown);

	/* right after the last filter that belongs before (or with) this one */
	size_t position = 0;
	for (size_t i = 0; i < shown.size(); i++) {
		if (shown[i] == filter) {
			continue;
		}
		int other = Rank(obs_source_get_unversioned_id(shown[i]));
		if (other >= 0 && other <= rank) {
			position = i + 1;
		}
	}
	/* shown is in reverse of libobs' own array, so convert the position */
	size_t current = 0;
	for (size_t i = 0; i < shown.size(); i++) {
		if (shown[i] == filter) {
			current = i;
		}
	}
	if (position > current) {
		position--; // the filter itself leaves its old slot first
	}
	obs_source_filter_set_index(source, filter, shown.size() - 1 - position);
}

void AudioFilterOrder::TurnOffDuplicates(obs_source_t *source, obs_source_t *added)
{
	const char *addedId = obs_source_get_unversioned_id(added);
	struct Search {
		obs_source_t *added;
		const char *addedId;
		std::vector<obs_source_t *> clashes;
	} search{added, addedId, {}};

	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *child, void *param) {
			auto *s = static_cast<Search *>(param);
			const char *id = obs_source_get_unversioned_id(child);
			if (child == s->added || !obs_source_enabled(child)) {
				return;
			}
			bool sameJob = IsNoiseRemover(id) && IsNoiseRemover(s->addedId);
			bool sameFilter = MustRunOnce(id) && id && s->addedId && std::strcmp(id, s->addedId) == 0;
			if (sameJob || sameFilter) {
				s->clashes.push_back(child);
			}
		},
		&search);

	for (obs_source_t *old : search.clashes) {
		obs_source_set_enabled(old, false);
		SetOffReason(old, tr("Turned off automatically: \"%1\" was added and does the same job. To use this "
				     "one instead, turn it on with the eye button and turn \"%1\" off.")
					  .arg(QString::fromUtf8(obs_source_get_name(added))));
		emit Notice(tr("\"%1\" was turned off: \"%2\" does the same job, and two at once make the voice sound worse.")
				    .arg(QString::fromUtf8(obs_source_get_name(old)),
					 QString::fromUtf8(obs_source_get_name(added))));
	}
}

void AudioFilterOrder::SortAll(obs_source_t *source)
{
	/* plugins we don't know (most VSTs) are treated like EQ: shaping the voice */
	constexpr int kUnknownRank = 3;

	std::vector<obs_source_t *> shown;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *child, void *param) {
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(child);
		},
		&shown);

	auto rankOf = [](obs_source_t *filter) {
		int rank = Rank(obs_source_get_unversioned_id(filter));
		return rank < 0 ? kUnknownRank : rank;
	};
	/* stable: filters of the same kind keep the order the user gave them */
	std::stable_sort(shown.begin(), shown.end(),
			 [&](obs_source_t *a, obs_source_t *b) { return rankOf(a) < rankOf(b); });

	const size_t count = shown.size();
	for (size_t i = 0; i < count; i++) {
		obs_source_filter_set_index(source, shown[i], count - 1 - i);
	}
	/* the first one in the chain stays on, later clashing ones are turned off */
	TurnOffClashes(shown);
}

QHash<obs_source_t *, QString> AudioFilterOrder::FindConflicts(obs_source_t *source)
{
	QHash<obs_source_t *, QString> conflicts;
	std::vector<obs_source_t *> enabled;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *child, void *param) {
			if (obs_source_enabled(child)) {
				static_cast<std::vector<obs_source_t *> *>(param)->push_back(child);
			}
		},
		&enabled);

	auto name = [](obs_source_t *filter) { return QString::fromUtf8(obs_source_get_name(filter)); };
	auto add = [&](obs_source_t *filter, const QString &text) {
		QString &existing = conflicts[filter];
		existing += existing.isEmpty() ? text : QStringLiteral("\n\n") + text;
	};

	for (size_t i = 0; i < enabled.size(); i++) {
		const char *id = obs_source_get_unversioned_id(enabled[i]);
		for (size_t j = 0; j < enabled.size(); j++) {
			if (i == j) {
				continue;
			}
			const char *otherId = obs_source_get_unversioned_id(enabled[j]);
			if (IsNoiseRemover(id) && IsNoiseRemover(otherId)) {
				add(enabled[i], tr("\"%1\" and \"%2\" both remove noise. Two at once make the voice dull and "
						   "robotic. Turn one of them off with the eye button.")
							.arg(name(enabled[i]), name(enabled[j])));
			} else if (MustRunOnce(id) && otherId && std::strcmp(id, otherId) == 0) {
				add(enabled[i], tr("\"%1\" is on twice. One is enough, turn the other one off.")
							.arg(name(enabled[i])));
			}
		}

		/* order: something that should come later is above this filter */
		int rank = Rank(id);
		for (size_t j = 0; j < i && rank >= 0; j++) {
			int earlier = Rank(obs_source_get_unversioned_id(enabled[j]));
			if (earlier > rank) {
				add(enabled[i], tr("\"%1\" works best before \"%2\". Press Auto to put the filters in the "
						   "right order.")
							.arg(name(enabled[i]), name(enabled[j])));
				break;
			}
		}
	}
	return conflicts;
}

namespace {
/* turns the chosen filter on and every filter doing the same job off */
void UseInstead(obs_source_t *source, obs_source_t *chosen)
{
	std::vector<obs_source_t *> all;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *child, void *param) {
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(child);
		},
		&all);

	const char *chosenId = obs_source_get_unversioned_id(chosen);
	obs_source_set_enabled(chosen, true);
	SetOffReason(chosen, QString());
	for (obs_source_t *other : all) {
		const char *id = obs_source_get_unversioned_id(other);
		bool sameJob = IsNoiseRemover(id) && IsNoiseRemover(chosenId);
		bool sameFilter = MustRunOnce(id) && id && chosenId && std::strcmp(id, chosenId) == 0;
		if (other != chosen && obs_source_enabled(other) && (sameJob || sameFilter)) {
			obs_source_set_enabled(other, false);
			SetOffReason(other, QObject::tr("Turned off: you chose \"%1\" for this job instead.")
						    .arg(QString::fromUtf8(obs_source_get_name(chosen))));
		}
	}
}

/* the little card that opens when the ⚠ / ⓘ button is clicked */
void ShowExplanation(QWidget *anchor, obs_source_t *source, obs_source_t *filter, const QString &text, bool turnedOff)
{
	constexpr int kCardWidth = 340;
	QMenu menu(anchor);
	auto *label = new QLabel(text);
	label->setWordWrap(true);
	label->setFixedWidth(kCardWidth);
	label->setContentsMargins(12, 10, 12, 10);
	auto *textAction = new QWidgetAction(&menu);
	textAction->setDefaultWidget(label);
	menu.addAction(textAction);
	menu.addSeparator();

	OBSSource keepSource = source;
	OBSSource keepFilter = filter;
	if (turnedOff) {
		menu.addAction(QObject::tr("Use this one instead"), &menu,
			       [keepSource, keepFilter]() { UseInstead(keepSource, keepFilter); });
	} else {
		menu.addAction(QObject::tr("Fix with Auto"), &menu,
			       [keepSource]() { AudioFilterOrder::SortAll(keepSource); });
	}
	menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}
} // namespace

void AudioFilterOrder::ShowConflicts(obs_source_t *source, QListWidget *list)
{
	static constexpr const char *kBadgeName = "audioFilterConflictBadge";
	const QHash<obs_source_t *, QString> conflicts = FindConflicts(source);

	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *item = list->item(i);
		QWidget *row = list->itemWidget(item);
		auto *layout = row ? qobject_cast<QBoxLayout *>(row->layout()) : nullptr;
		if (!layout) {
			continue;
		}
		OBSSource filter = item->data(Qt::UserRole).value<OBSSource>();
		QString message = conflicts.value(filter);
		bool turnedOff = false;
		if (obs_source_enabled(filter)) {
			/* the user turned it back on: the old reason no longer applies */
			if (!OffReason(filter).isEmpty()) {
				SetOffReason(filter, QString());
			}
		} else {
			message = OffReason(filter);
			turnedOff = !message.isEmpty();
		}

		auto *badge = row->findChild<QToolButton *>(kBadgeName);
		if (message.isEmpty()) {
			if (badge) {
				badge->hide();
			}
			continue;
		}
		if (!badge) {
			badge = new QToolButton(row);
			badge->setObjectName(kBadgeName);
			badge->setAutoRaise(true);
			badge->setCursor(Qt::PointingHandCursor);
			layout->addWidget(badge);
		}
		/* ⚠ yellow: a problem right now; ⓘ grey: we turned it off, here is why */
		badge->setText(turnedOff ? QStringLiteral("ⓘ") : QStringLiteral("⚠"));
		badge->setStyleSheet(
			QStringLiteral("QToolButton { color: %1; font-size: 15px; font-weight: 700; padding: 0 6px; border: none; }")
				.arg(turnedOff ? "#a0a0aa" : "#f0b44c"));
		/* rich text so the tooltip wraps instead of running off the screen */
		badge->setToolTip(QStringLiteral("<p style='white-space:normal; width:320px'>%1</p>")
					  .arg(message.toHtmlEscaped().replace("\n", "<br>")));

		/* reconnect every time so the click uses the current text */
		QObject::disconnect(badge, &QToolButton::clicked, nullptr, nullptr);
		OBSSource keepSource = source;
		QObject::connect(badge, &QToolButton::clicked, badge, [badge, keepSource, filter, message, turnedOff]() {
			ShowExplanation(badge, keepSource, filter, message, turnedOff);
		});
		badge->show();
	}
}
