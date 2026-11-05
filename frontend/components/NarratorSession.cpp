#include "NarratorSession.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <algorithm>
#include <cmath>

#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QTimer>
#include <QWidget>

#include <qt-wrappers.hpp>

#ifdef __APPLE__
#include <utility/MacGlass.hpp>
#endif

#include "moc_NarratorSession.cpp"

namespace {
constexpr int kDefaultCountdownSeconds = 3;
constexpr int kMaxCountdownSeconds = 10;
/* the narrator often breathes in right before "go", keep that out of the room tone */
constexpr double kRoomToneSafetySec = 0.4;
constexpr int kVoiceIntervalMs = 60;
constexpr int kMarkOverlayMs = 1600;
constexpr int kSpeakOverlayMs = 1400;

/* voice detection: this much above the room noise counts as talking */
constexpr float kVoiceAboveNoiseDb = 12.0f;
constexpr float kVoiceMinDb = -55.0f;
constexpr qint64 kVoiceHoldMs = 800;

const char *kOverlayStyle = "QLabel { background-color: rgba(10, 10, 14, 215); color: #ffffff;"
			    " border-radius: 24px; padding: 28px 48px; }";
} // namespace

NarratorSession::NarratorSession(QWidget *overlayParent_, QObject *parent)
	: QObject(parent),
	  overlayParent(overlayParent_)
{
	overlay = new QLabel(overlayParent);
	overlay->setAlignment(Qt::AlignCenter);
	overlay->setStyleSheet(kOverlayStyle);
	overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
	overlay->hide();
	overlayParent->installEventFilter(this);

	countdownTimer = new QTimer(this);
	countdownTimer->setInterval(1000);
	connect(countdownTimer, &QTimer::timeout, this, &NarratorSession::CountdownTick);

	overlayHideTimer = new QTimer(this);
	overlayHideTimer->setSingleShot(true);
	connect(overlayHideTimer, &QTimer::timeout, this, &NarratorSession::HideCard);

	voiceTimer = new QTimer(this);
	voiceTimer->setInterval(kVoiceIntervalMs);
	connect(voiceTimer, &QTimer::timeout, this, &NarratorSession::DetectVoice);
	voiceTimer->start();
	uptime.start();
}

NarratorSession::~NarratorSession()
{
	DetachMicrophone();
}

QString NarratorSession::SidecarPath(const QString &takePath)
{
	return takePath + QStringLiteral(".narration.json");
}

void NarratorSession::AttachMicrophone(obs_source_t *source)
{
	DetachMicrophone();
	if (!source) {
		return;
	}
	volmeter = obs_volmeter_create(OBS_FADER_LOG);
	obs_volmeter_attach_source(volmeter, source);
	obs_volmeter_add_callback(volmeter, &NarratorSession::VolmeterCallback, this);
}

void NarratorSession::DetachMicrophone()
{
	if (volmeter) {
		obs_volmeter_remove_callback(volmeter, &NarratorSession::VolmeterCallback, this);
		obs_volmeter_destroy(volmeter);
		volmeter = nullptr;
	}
}

/* runs on the audio thread, only stores numbers for the UI timer */
void NarratorSession::VolmeterCallback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
				       const float[MAX_AUDIO_CHANNELS], const float[MAX_AUDIO_CHANNELS])
{
	auto *self = static_cast<NarratorSession *>(data);
	int channels = std::clamp(obs_volmeter_get_nr_channels(self->volmeter), 1, MAX_AUDIO_CHANNELS);
	float loudestLevel = -100.0f;
	for (int c = 0; c < channels; c++) {
		if (std::isfinite(magnitude[c])) {
			loudestLevel = std::max(loudestLevel, magnitude[c]);
		}
	}
	self->levelDb.store(loudestLevel);
}

double NarratorSession::ElapsedSeconds() const
{
	if (!recording) {
		return 0.0;
	}
	qint64 paused = pausedMs + (pauseStartedMs >= 0 ? clock.elapsed() - pauseStartedMs : 0);
	return double(clock.elapsed() - paused) / 1000.0;
}

void NarratorSession::RecordingStarted()
{
	recording = true;
	clock.start();
	pausedMs = 0;
	pauseStartedMs = -1;
	mistakes.clear();
	roomToneEnd = 0.0;

	/* the file is already being written: these seconds become the room tone */
	OBSBasic *main = OBSBasic::Get();
	countdownLeft = main ? std::clamp(int(config_get_int(main->Config(), "SimpleOutput", "RecCountdown")), 0,
					  kMaxCountdownSeconds)
			     : kDefaultCountdownSeconds;
	if (countdownLeft == 0) {
		/* the narrator turned the countdown off: start reading right away */
		emit SpeakNow();
		return;
	}
	ShowCard(QString::number(countdownLeft), QTStr("Narrator.StayQuiet"), QColor("#ffffff"), 96, 0);
	countdownTimer->start();
}

void NarratorSession::CountdownTick()
{
	countdownLeft--;
	if (countdownLeft > 0) {
		ShowCard(QString::number(countdownLeft), QTStr("Narrator.StayQuiet"), QColor("#ffffff"), 96, 0);
		return;
	}

	countdownTimer->stop();
	roomToneEnd = std::max(0.0, ElapsedSeconds() - kRoomToneSafetySec);
	ShowCard(QTStr("Narrator.Speak"), QTStr("Narrator.MistakeHint"), QColor("#7ee2a0"), 56, kSpeakOverlayMs);
	emit SpeakNow();
}

void NarratorSession::RecordingPaused()
{
	if (pauseStartedMs < 0) {
		pauseStartedMs = clock.elapsed();
	}
}

void NarratorSession::RecordingUnpaused()
{
	if (pauseStartedMs >= 0) {
		pausedMs += clock.elapsed() - pauseStartedMs;
		pauseStartedMs = -1;
	}
}

void NarratorSession::MarkMistake()
{
	if (!recording || countdownLeft > 0) {
		return;
	}
	mistakes.append(ElapsedSeconds());
	ShowCard(QTStr("Narrator.MistakeMarked"), QTStr("Narrator.ReadAgain"), QColor("#ff8a8a"), 34, kMarkOverlayMs);
}

void NarratorSession::RecordingStopped(const QString &takePath)
{
	bool wasCounting = countdownLeft > 0;
	countdownTimer->stop();
	countdownLeft = 0;
	if (wasCounting) {
		HideCard();
		/* stopped during the countdown: the whole take is room tone */
		roomToneEnd = std::max(0.0, ElapsedSeconds() - kRoomToneSafetySec);
	}
	recording = false;

	if (takePath.isEmpty()) {
		return;
	}

	QJsonObject root;
	root["version"] = 1;
	if (roomToneEnd > 0.0) {
		root["roomTone"] = QJsonArray{0.0, roomToneEnd};
	}
	QJsonArray marks;
	for (double t : mistakes) {
		marks.append(t);
	}
	root["mistakes"] = marks;

	QFile file(SidecarPath(takePath));
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		blog(LOG_WARNING, "Narrator: could not save marks for '%s'", QT_TO_UTF8(takePath));
		return;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void NarratorSession::ShowCard(const QString &title, const QString &subtitle, const QColor &color, int titlePx,
			       int hideAfterMs)
{
#ifdef __APPLE__
	/* macOS 26+: a real Liquid Glass card floating over the script */
	if (MacGlass::Available() && overlayParent) {
		MacGlass::ShowCard(overlayParent, title, subtitle, color, titlePx);
		overlay->hide();
		if (hideAfterMs > 0) {
			overlayHideTimer->start(hideAfterMs);
		} else {
			overlayHideTimer->stop();
		}
		return;
	}
#endif
	QString html = QStringLiteral("<div style='font-size:%1px; font-weight:700; color:%2;'>%3</div>")
			       .arg(titlePx)
			       .arg(color.name(), title.toHtmlEscaped());
	if (!subtitle.isEmpty()) {
		html += QStringLiteral("<div style='font-size:17px; color:#d0d0d8;'>%1</div>").arg(subtitle.toHtmlEscaped());
	}
	overlay->setText(html);
	overlay->adjustSize();
	PositionOverlay();
	overlay->show();
	overlay->raise();
	if (hideAfterMs > 0) {
		overlayHideTimer->start(hideAfterMs);
	} else {
		overlayHideTimer->stop();
	}
}

void NarratorSession::HideCard()
{
	overlay->hide();
#ifdef __APPLE__
	MacGlass::HideCard();
#endif
}

void NarratorSession::PositionOverlay()
{
	if (!overlayParent) {
		return;
	}
	QSize size = overlay->sizeHint();
	overlay->setGeometry((overlayParent->width() - size.width()) / 2,
			     (overlayParent->height() - size.height()) / 2, size.width(), size.height());
}

bool NarratorSession::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == overlayParent && event->type() == QEvent::Resize && overlay->isVisible()) {
		PositionOverlay();
	}
	return QObject::eventFilter(watched, event);
}

void NarratorSession::DetectVoice()
{
	const qint64 now = uptime.elapsed();
	const float level = levelDb.load();

	/* the room noise is the lowest level we keep seeing: follow it down fast, up slowly */
	if (level > -99.0f) {
		noiseFloorDb = level < noiseFloorDb ? 0.7f * noiseFloorDb + 0.3f * level : noiseFloorDb + 0.02f;
	}

	if (level > std::max(kVoiceMinDb, noiseFloorDb + kVoiceAboveNoiseDb)) {
		lastVoiceMs = now;
	}
	bool nowSpeaking = now - lastVoiceMs < kVoiceHoldMs;
	if (nowSpeaking != speaking) {
		speaking = nowSpeaking;
		emit VoiceActivityChanged(speaking);
	}
}
