#include "MicrophoneButton.hpp"

#include <OBSApp.hpp>

#include <QMenu>

#include <qt-wrappers.hpp>

#include "moc_MicrophoneButton.cpp"

namespace {
constexpr const char *kDeviceProperty = "device_id";
}

MicrophoneButton::MicrophoneButton(QWidget *parent) : QPushButton(parent)
{
	menu = new QMenu(this);
	setMenu(menu);
	setToolTip(QTStr("AudioOnly.MicrophoneTip"));
	connect(menu, &QMenu::aboutToShow, this, &MicrophoneButton::FillMenu);
	UpdateText();
}

void MicrophoneButton::SetSource(obs_source_t *source)
{
	weakSource = OBSGetWeakRef(source);
	UpdateText();
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
