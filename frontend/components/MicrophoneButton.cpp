#include "MicrophoneButton.hpp"

#include <OBSApp.hpp>

#include <QMenu>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include <qt-wrappers.hpp>

#include "moc_MicrophoneButton.cpp"

namespace {
constexpr const char *kDeviceProperty = "device_id";
constexpr int kLevelIntervalMs = 40;
constexpr float kSilentDb = -60.0f;
constexpr float kLoudDb = -9.0f;
constexpr float kClipDb = -2.0f;
/* how fast the line falls back, so it does not flicker */
constexpr float kFallDbPerTick = 1.5f;
constexpr int kLevelHeight = 3;
constexpr int kLevelInset = 6;
const QColor kLevelTrackColor(255, 255, 255, 28);
const QColor kLevelOkColor(0x34, 0xd3, 0x99);
const QColor kLevelLoudColor(0xf5, 0xc5, 0x42);
const QColor kLevelClipColor(0xef, 0x44, 0x44);
} // namespace

MicrophoneButton::MicrophoneButton(QWidget *parent) : QPushButton(parent)
{
	menu = new QMenu(this);
	setMenu(menu);
	setToolTip(QTStr("AudioOnly.MicrophoneTip"));
	connect(menu, &QMenu::aboutToShow, this, &MicrophoneButton::FillMenu);
	levelTimer = new QTimer(this);
	levelTimer->setInterval(kLevelIntervalMs);
	connect(levelTimer, &QTimer::timeout, this, [this]() {
		float target = levelDb.load();
		shownDb = target > shownDb ? target : std::max(target, shownDb - kFallDbPerTick);
		update();
	});
	UpdateText();
}

MicrophoneButton::~MicrophoneButton()
{
	DetachLevel();
}

void MicrophoneButton::SetSource(obs_source_t *source)
{
	weakSource = OBSGetWeakRef(source);
	AttachLevel(source);
	UpdateText();
}

void MicrophoneButton::AttachLevel(obs_source_t *source)
{
	DetachLevel();
	if (!source) {
		return;
	}
	volmeter = obs_volmeter_create(OBS_FADER_LOG);
	obs_volmeter_attach_source(volmeter, source);
	obs_volmeter_add_callback(volmeter, &MicrophoneButton::LevelCallback, this);
	levelTimer->start();
}

void MicrophoneButton::DetachLevel()
{
	if (!volmeter) {
		return;
	}
	obs_volmeter_remove_callback(volmeter, &MicrophoneButton::LevelCallback, this);
	obs_volmeter_destroy(volmeter);
	volmeter = nullptr;
	levelTimer->stop();
	levelDb = -100.0f;
	shownDb = -100.0f;
}

/* runs on the audio thread, only stores the loudest channel */
void MicrophoneButton::LevelCallback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
				     const float[MAX_AUDIO_CHANNELS], const float[MAX_AUDIO_CHANNELS])
{
	auto *self = static_cast<MicrophoneButton *>(data);
	int channels = std::clamp(obs_volmeter_get_nr_channels(self->volmeter), 1, MAX_AUDIO_CHANNELS);
	float loudest = -100.0f;
	for (int c = 0; c < channels; c++) {
		if (std::isfinite(magnitude[c])) {
			loudest = std::max(loudest, magnitude[c]);
		}
	}
	self->levelDb = loudest;
}

void MicrophoneButton::paintEvent(QPaintEvent *event)
{
	QPushButton::paintEvent(event);
	if (!volmeter) {
		return;
	}

	QRectF track(kLevelInset, height() - kLevelInset - kLevelHeight, width() - kLevelInset * 2, kLevelHeight);
	double share = std::clamp((shownDb - kSilentDb) / -kSilentDb, 0.0f, 1.0f);
	QColor color = shownDb >= kClipDb ? kLevelClipColor : (shownDb >= kLoudDb ? kLevelLoudColor : kLevelOkColor);

	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(kLevelTrackColor);
	painter.drawRoundedRect(track, 1.5, 1.5);
	painter.setBrush(color);
	painter.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * share, track.height()), 1.5, 1.5);
}

void MicrophoneButton::FillMenu()
{
	menu->clear();
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (!source) {
		menu->addAction(QTStr("AudioOnly.NoMicrophone"))->setEnabled(false);
		return;
	}

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	QString current = QString::fromUtf8(obs_data_get_string(settings, kDeviceProperty));

	/* ask the source for its devices now, not when the app started */
	OBSProperties props = obs_source_properties(source);
	obs_property_t *list = obs_properties_get(props, kDeviceProperty);
	size_t count = list ? obs_property_list_item_count(list) : 0;
	for (size_t i = 0; i < count; i++) {
		QString name = QString::fromUtf8(obs_property_list_item_name(list, i));
		QString id = QString::fromUtf8(obs_property_list_item_string(list, i));
		QAction *action = menu->addAction(name);
		action->setCheckable(true);
		action->setChecked(id == current);
		action->setEnabled(!obs_property_list_item_disabled(list, i));
		connect(action, &QAction::triggered, this, [this, id]() { SelectDevice(id); });
	}
	if (count == 0) {
		menu->addAction(QTStr("AudioOnly.NoMicrophone"))->setEnabled(false);
	}
}

void MicrophoneButton::SelectDevice(const QString &deviceId)
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (!source) {
		return;
	}
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, kDeviceProperty, QT_TO_UTF8(deviceId));
	obs_source_update(source, settings);
	UpdateText();
}

/* the button shows the name of the device in use, like "Wireless Mic Rx" */
void MicrophoneButton::UpdateText()
{
	QString name = QTStr("AudioOnly.Microphone");
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (source) {
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		QString current = QString::fromUtf8(obs_data_get_string(settings, kDeviceProperty));
		OBSProperties props = obs_source_properties(source);
		obs_property_t *list = obs_properties_get(props, kDeviceProperty);
		size_t count = list ? obs_property_list_item_count(list) : 0;
		for (size_t i = 0; i < count; i++) {
			if (current == QString::fromUtf8(obs_property_list_item_string(list, i))) {
				name = QString::fromUtf8(obs_property_list_item_name(list, i));
				break;
			}
		}
	}
	setText(QStringLiteral("🎤  ") + name);
}
