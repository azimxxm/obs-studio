/* Audio-only (narrator) mode for the main window: teleprompter, takes,
 * microphone button and shortcuts. Kept out of the upstream OBS files so
 * merging new OBS versions stays easy; OBSBasic only calls in here. */

#include "OBSBasic.hpp"

#include <components/MicrophoneButton.hpp>
#include <components/NarratorSession.hpp>
#include <components/TakesPanel.hpp>
#include <components/Teleprompter.hpp>

#include <QShortcut>
#include <QSplitter>
#include <QVBoxLayout>

#include <qt-wrappers.hpp>

bool OBSBasic::IsAudioOnlyMode()
{
	const char *outputMode = config_get_string(activeConfiguration, "Output", "Mode");
	const char *recMode = config_get_string(activeConfiguration, "SimpleOutput", "RecMode");
	return astrcmpi(outputMode, "Simple") == 0 && astrcmpi(recMode, "Audio") == 0;
}

/* Finds the first microphone-like source in the current scene, creating one if
 * there is none, and selects it so the device picker shows up right away. */
void OBSBasic::SelectAudioOnlyMicrophone()
{
	OBSScene scene = GetCurrentScene();
	if (!scene) {
		return;
	}

	obs_sceneitem_t *micItem = nullptr;
	auto findMic = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		uint32_t flags = obs_source_get_output_flags(obs_sceneitem_get_source(item));
		if ((flags & OBS_SOURCE_AUDIO) != 0 && (flags & OBS_SOURCE_VIDEO) == 0) {
			*static_cast<obs_sceneitem_t **>(param) = item;
			return false;
		}
		return true;
	};
	obs_scene_enum_items(scene, findMic, &micItem);

	if (!micItem) {
		OBSSourceAutoRelease mic = obs_source_create(App()->InputAudioSource(),
							     QT_TO_UTF8(QTStr("AudioOnly.Microphone")), nullptr, nullptr);
		if (!mic) {
			blog(LOG_WARNING, "Audio-only mode: could not create a microphone source");
			return;
		}
		micItem = obs_scene_add(scene, mic);
	}

	if (micItem) {
		obs_sceneitem_select(micItem, true);
		if (narrator) {
			narrator->AttachMicrophone(obs_sceneitem_get_source(micItem));
		}
		if (micButton) {
			micButton->SetSource(obs_sceneitem_get_source(micItem));
		}
	}
}

/* Docks and the mixer orientation are saved before entering audio-only mode
 * and restored when leaving it, so Video mode looks exactly like before. */
void OBSBasic::ApplyAudioOnlyDocks(bool audioOnly)
{
	config_t *appConfig = App()->GetUserConfig();
	const char *section = "AudioOnlyLayout";

	if (audioOnly) {
		config_set_bool(appConfig, section, "ScenesDockVisible", ui->scenesDock->isVisible());
		config_set_bool(appConfig, section, "SourcesDockVisible", ui->sourcesDock->isVisible());
		config_set_bool(appConfig, section, "TransitionsDockVisible", ui->transitionsDock->isVisible());
		config_set_bool(appConfig, section, "VerticalVolumeControl",
				config_get_bool(appConfig, "BasicWindow", "VerticalVolumeControl"));
	} else {
		config_set_default_bool(appConfig, section, "ScenesDockVisible", true);
		config_set_default_bool(appConfig, section, "SourcesDockVisible", true);
		config_set_default_bool(appConfig, section, "TransitionsDockVisible", true);
	}

	ui->scenesDock->setVisible(!audioOnly && config_get_bool(appConfig, section, "ScenesDockVisible"));
	ui->sourcesDock->setVisible(!audioOnly && config_get_bool(appConfig, section, "SourcesDockVisible"));
	ui->transitionsDock->setVisible(!audioOnly && config_get_bool(appConfig, section, "TransitionsDockVisible"));

	/* A wide horizontal meter is much easier to read for a narrator */
	bool vertical = audioOnly ? false : config_get_bool(appConfig, section, "VerticalVolumeControl");
	if (vertical != config_get_bool(appConfig, "BasicWindow", "VerticalVolumeControl")) {
		config_set_bool(appConfig, "BasicWindow", "VerticalVolumeControl", vertical);
		emit userSettingChanged("BasicWindow", "VerticalVolumeControl");
	}
}

/* Teleprompter on the left, the list of recorded takes on the right. */
void OBSBasic::CreateAudioOnlyWorkspace(QBoxLayout *centralLayout)
{
	audioOnlySplitter = new QSplitter(Qt::Horizontal, ui->centralwidget);
	audioOnlySplitter->setChildrenCollapsible(false);

	teleprompter = new Teleprompter(audioOnlySplitter);
	takesPanel = new TakesPanel(audioOnlySplitter);
	audioOnlySplitter->addWidget(teleprompter);
	audioOnlySplitter->addWidget(takesPanel);
	audioOnlySplitter->setStretchFactor(0, 1);
	audioOnlySplitter->setStretchFactor(1, 0);
	audioOnlySplitter->setSizes({1000, 300});

	if (centralLayout) {
		centralLayout->addWidget(audioOnlySplitter, 1);
	}

	/* the countdown runs on top of the teleprompter; the text starts moving after it */
	narrator = new NarratorSession(teleprompter, this);
	connect(this, &OBSBasic::RecordingStarted, narrator, &NarratorSession::RecordingStarted);
	connect(this, &OBSBasic::RecordingPaused, narrator, &NarratorSession::RecordingPaused);
	connect(this, &OBSBasic::RecordingUnpaused, narrator, &NarratorSession::RecordingUnpaused);
	connect(this, &OBSBasic::RecordingStopped, narrator,
		[this]() { narrator->RecordingStopped(QString::fromStdString(outputHandler->lastRecordingPath)); });
	connect(narrator, &NarratorSession::SpeakNow, teleprompter, &Teleprompter::StartScrolling);
	connect(narrator, &NarratorSession::VoiceActivityChanged, teleprompter, &Teleprompter::SetVoiceActive);

	connect(this, &OBSBasic::RecordingUnpaused, teleprompter, &Teleprompter::StartScrolling);
	connect(this, &OBSBasic::RecordingPaused, teleprompter, &Teleprompter::PauseScrolling);
	connect(this, &OBSBasic::RecordingStopped, teleprompter, &Teleprompter::StopScrolling);

	/* don't play an old take into the microphone while recording a new one */
	connect(this, &OBSBasic::RecordingStarted, takesPanel, &TakesPanel::StopPlayback);
	connect(this, &OBSBasic::RecordingStopped, takesPanel, &TakesPanel::RefreshAfterRecording);
}

void OBSBasic::ToggleRecordingFromShortcut()
{
	if (RecordingActive()) {
		StopRecording();
	} else {
		StartRecording();
	}
}

/* Cmd+R records / stops, Cmd+P listens to the last take, Cmd+Backspace throws it away,
 * E opens the editor, Shift+Cmd+R shows the take in Finder. */
void OBSBasic::CreateAudioOnlyShortcuts()
{
	auto add = [this](const QKeySequence &key, auto slot) {
		auto *shortcut = new QShortcut(key, this);
		shortcut->setEnabled(false);
		connect(shortcut, &QShortcut::activated, this, slot);
		/* Cmd+ and Cmd= can be the same key on some keyboards; either one works */
		connect(shortcut, &QShortcut::activatedAmbiguously, this, slot);
		audioOnlyShortcuts.append(shortcut);
	};
	add(QKeySequence(Qt::CTRL | Qt::Key_R), [this]() { ToggleRecordingFromShortcut(); });
	add(QKeySequence(Qt::CTRL | Qt::Key_P), [this]() {
		if (takesPanel && !RecordingActive()) {
			takesPanel->PlayLatest();
		}
	});
	/* M while reading: "I made a mistake, I'm reading this sentence again" */
	add(QKeySequence(Qt::Key_M), [this]() {
		if (narrator && RecordingActive()) {
			narrator->MarkMistake();
		}
	});
	/* Cmd+ / Cmd- (and Cmd= on keyboards where + needs Shift) size the script text */
	add(QKeySequence::ZoomIn, [this]() {
		if (teleprompter) {
			teleprompter->BiggerText();
		}
	});
	add(QKeySequence(Qt::CTRL | Qt::Key_Equal), [this]() {
		if (teleprompter) {
			teleprompter->BiggerText();
		}
	});
	add(QKeySequence::ZoomOut, [this]() {
		if (teleprompter) {
			teleprompter->SmallerText();
		}
	});
	add(QKeySequence(Qt::Key_E), [this]() {
		if (takesPanel && !RecordingActive()) {
			takesPanel->OpenEditor();
		}
	});
	add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), [this]() {
		if (takesPanel) {
			takesPanel->ShowSelectedInFinder();
		}
	});
	add(QKeySequence(Qt::CTRL | Qt::Key_Backspace), [this]() {
		if (takesPanel && !RecordingActive()) {
			takesPanel->DeleteLatest();
		}
	});
}

void OBSBasic::ApplyAudioOnlyCentralArea(bool audioOnly)
{
	auto *centralLayout = qobject_cast<QBoxLayout *>(ui->centralwidget->layout());

	ui->canvasEditor->setVisible(!audioOnly);
	obs_display_set_enabled(ui->preview->GetDisplay(), !audioOnly && previewEnabled);

	if (audioOnly && !audioOnlySplitter) {
		CreateAudioOnlyWorkspace(centralLayout);
		CreateAudioOnlyShortcuts();
	}
	if (audioOnlySplitter) {
		audioOnlySplitter->setVisible(audioOnly);
	}
	if (takesPanel && !audioOnly) {
		takesPanel->StopPlayback();
	}
	for (QShortcut *shortcut : audioOnlyShortcuts) {
		shortcut->setEnabled(audioOnly);
	}

	MoveMicrophoneBar(audioOnly);
}

/* In audio-only mode the microphone is one button in the teleprompter's row
 * ("🎤 Wireless Mic Rx ▾"); Properties and Filters are in the mixer's menu.
 * Video mode gets OBS's normal context bar back under the preview. */
void OBSBasic::MoveMicrophoneBar(bool intoTeleprompter)
{
	if (intoTeleprompter && teleprompter) {
		if (!micButton) {
			micButton = new MicrophoneButton(teleprompter);
		}
		teleprompter->SetToolbarAccessory(micButton);
		ui->contextContainer->setVisible(false);
		return;
	}

	if (teleprompter) {
		teleprompter->SetToolbarAccessory(nullptr);
	}
	if (micButton) {
		micButton->hide();
	}
	ui->contextContainer->setVisible(config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowContextToolbars"));
}

/* Audio-only mode hides everything a narrator doesn't need: the preview, scenes,
 * sources, transitions and the streaming / virtual camera / studio mode buttons. */
void OBSBasic::UpdateAudioOnlyLayout()
{
	bool audioOnly = IsAudioOnlyMode();
	if (audioOnly == audioOnlyLayoutActive) {
		return;
	}

	ApplyAudioOnlyDocks(audioOnly);
	ApplyAudioOnlyCentralArea(audioOnly);
	if (audioOnly) {
		SelectAudioOnlyMicrophone();
	}

	audioOnlyLayoutActive = audioOnly;
	emit AudioOnlyModeChanged(audioOnly);
}
