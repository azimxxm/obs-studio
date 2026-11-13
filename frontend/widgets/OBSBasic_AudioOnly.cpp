/* Audio-only (narrator) mode for the main window: teleprompter, takes,
 * microphone button and shortcuts. Kept out of the upstream OBS files so
 * merging new OBS versions stays easy; OBSBasic only calls in here. */

#include "OBSBasic.hpp"

#include <components/MicrophoneButton.hpp>
#include <components/NarratorSession.hpp>
#include <components/PanelGrip.hpp>
#include <components/RecordButton.hpp>
#include <components/TakesPanel.hpp>
#include <components/Teleprompter.hpp>
#include <utility/AudioFilterOrder.hpp>

#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QTimer>
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
		if (!audioFilterOrder) {
			audioFilterOrder = new AudioFilterOrder(this);
			connect(audioFilterOrder, &AudioFilterOrder::Notice, this, &OBSBasic::ShowStatusBarMessage);
		}
		/* audio mode keeps the voice chain in order; video mode leaves filters alone */
		audioFilterOrder->Attach(obs_sceneitem_get_source(micItem));
	}
}

/* Docks and the mixer orientation are saved before entering audio-only mode
 * and restored when leaving it, so Video mode looks exactly like before.
 * "Active" survives restarts: an app that starts in audio mode must not save its
 * own hidden docks as the video layout, and one that starts in video mode after
 * an audio session must get the video layout back. */
void OBSBasic::ApplyAudioOnlyDocks(bool audioOnly)
{
	config_t *appConfig = App()->GetUserConfig();
	const char *section = "AudioOnlyLayout";
	bool audioLayoutWasActive = config_get_bool(appConfig, section, "Active");

	/* builds before "Active" existed could save the hidden audio docks as the
	 * video layout; throw those values away once and show OBS' docks again */
	if (!config_has_user_value(appConfig, section, "Active") &&
	    config_has_user_value(appConfig, section, "ScenesDockVisible")) {
		config_remove_value(appConfig, section, "ScenesDockVisible");
		config_remove_value(appConfig, section, "SourcesDockVisible");
		config_remove_value(appConfig, section, "TransitionsDockVisible");
		config_remove_value(appConfig, section, "VerticalVolumeControl");
		audioLayoutWasActive = true;
	}

	if (audioOnly) {
		if (!audioLayoutWasActive) {
			config_set_bool(appConfig, section, "ScenesDockVisible", ui->scenesDock->isVisible());
			config_set_bool(appConfig, section, "SourcesDockVisible", ui->sourcesDock->isVisible());
			config_set_bool(appConfig, section, "TransitionsDockVisible", ui->transitionsDock->isVisible());
			config_set_bool(appConfig, section, "MixerDockVisible", ui->mixerDock->isVisible());
			config_set_bool(appConfig, section, "ControlsDockVisible",
					!controlsDock || controlsDock->isVisible());
			config_set_bool(appConfig, section, "VerticalVolumeControl",
					config_get_bool(appConfig, "BasicWindow", "VerticalVolumeControl"));
		}
		config_set_bool(appConfig, section, "Active", true);

		ui->scenesDock->setVisible(false);
		ui->sourcesDock->setVisible(false);
		ui->transitionsDock->setVisible(false);

		/* the mixer and the record controls are hidden in audio mode unless the narrator
		 * asked for them (Mixer ⌘J) */
		config_set_default_bool(appConfig, "AudioOnly", "BottomPanelsVisible", false);
		ShowBottomPanels(config_get_bool(appConfig, "AudioOnly", "BottomPanelsVisible"));
	} else {
		/* a video session that never was in audio mode keeps OBS' own dock state */
		if (!audioLayoutWasActive) {
			return;
		}
		config_set_bool(appConfig, section, "Active", false);

		auto restore = [&](QWidget *dock, const char *key) {
			config_set_default_bool(appConfig, section, key, true);
			if (dock) {
				dock->setVisible(config_get_bool(appConfig, section, key));
			}
		};
		restore(ui->scenesDock, "ScenesDockVisible");
		restore(ui->sourcesDock, "SourcesDockVisible");
		restore(ui->transitionsDock, "TransitionsDockVisible");
		restore(ui->mixerDock, "MixerDockVisible");
		restore(controlsDock, "ControlsDockVisible");
	}

	/* A wide horizontal meter is much easier to read for a narrator */
	config_set_default_bool(appConfig, section, "VerticalVolumeControl",
				config_get_bool(appConfig, "BasicWindow", "VerticalVolumeControl"));
	bool vertical = audioOnly ? false : config_get_bool(appConfig, section, "VerticalVolumeControl");
	if (vertical != config_get_bool(appConfig, "BasicWindow", "VerticalVolumeControl")) {
		config_set_bool(appConfig, "BasicWindow", "VerticalVolumeControl", vertical);
		emit userSettingChanged("BasicWindow", "VerticalVolumeControl");
	}
	config_save_safe(appConfig, "tmp", nullptr);
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
	CreatePanelGrip(centralLayout);
	CreateCompactBar();
	CreateTakesToggle();
	CreatePanelsToggle();
	/* last in the row, after Takes and Mixer */
	auto *settingsButton = new QPushButton(QTStr("Settings"), teleprompter);
	teleprompter->AddToolbarButton(settingsButton);
	connect(settingsButton, &QPushButton::clicked, this, &OBSBasic::on_action_Settings_triggered);

	/* the countdown runs on top of the teleprompter; the text starts moving after it */
	narrator = new NarratorSession(teleprompter, this);
	/* the workspace stays alive after switching to Video mode; a video recording
	 * must not get a countdown, mistake marks or a .narration.json next to it */
	connect(this, &OBSBasic::RecordingStarted, narrator, [this]() {
		if (audioOnlyLayoutActive) {
			narrator->RecordingStarted();
		}
	});
	connect(this, &OBSBasic::RecordingPaused, narrator, [this]() {
		if (audioOnlyLayoutActive) {
			narrator->RecordingPaused();
		}
	});
	connect(this, &OBSBasic::RecordingUnpaused, narrator, [this]() {
		if (audioOnlyLayoutActive) {
			narrator->RecordingUnpaused();
		}
	});
	connect(this, &OBSBasic::RecordingStopped, narrator, [this]() {
		if (audioOnlyLayoutActive) {
			narrator->RecordingStopped(QString::fromStdString(outputHandler->lastRecordingPath));
		}
	});
	connect(narrator, &NarratorSession::SpeakNow, teleprompter, &Teleprompter::StartScrolling);
	connect(narrator, &NarratorSession::VoiceActivityChanged, teleprompter, &Teleprompter::SetVoiceActive);

	connect(this, &OBSBasic::RecordingUnpaused, teleprompter, &Teleprompter::StartScrolling);
	connect(this, &OBSBasic::RecordingPaused, teleprompter, &Teleprompter::PauseScrolling);
	connect(this, &OBSBasic::RecordingStopped, teleprompter, &Teleprompter::StopScrolling);

	/* don't play an old take into the microphone while recording a new one */
	connect(this, &OBSBasic::RecordingStarted, takesPanel, &TakesPanel::StopPlayback);
	connect(this, &OBSBasic::RecordingStopped, takesPanel, [this]() {
		if (audioOnlyLayoutActive) {
			takesPanel->RefreshAfterRecording();
		}
	});
}

/* "Takes ⌘T" at the right end of the teleprompter's row shows or hides the
 * takes list, so the script can use the whole width. The choice is remembered. */
void OBSBasic::CreateTakesToggle()
{
	constexpr const char *section = "AudioOnly";
	constexpr const char *key = "TakesPanelVisible";
	config_t *config = App()->GetUserConfig();
	config_set_default_bool(config, section, key, true);

	QKeySequence shortcut(Qt::CTRL | Qt::Key_T);
	takesToggle = new QPushButton(QTStr("AudioOnly.TakesToggle") + QStringLiteral("   ") +
					      shortcut.toString(QKeySequence::NativeText),
				      teleprompter);
	takesToggle->setToolTip(QTStr("AudioOnly.TakesToggleTip"));
	takesToggle->setCheckable(true);
	teleprompter->AddToolbarButton(takesToggle);

	bool visible = config_get_bool(config, section, key);
	takesToggle->setChecked(visible);
	takesPanel->setVisible(visible);

	connect(takesToggle, &QPushButton::toggled, this, [this, config, section, key](bool show) {
		takesPanel->setVisible(show);
		config_set_bool(config, section, key, show);
	});
}

/* "Mixer ⌘J" shows or hides the Audio Mixer and Controls panels under the script */
void OBSBasic::CreatePanelsToggle()
{
	config_t *config = App()->GetUserConfig();
	QKeySequence shortcut(Qt::CTRL | Qt::Key_J);
	panelsToggle = new QPushButton(QTStr("AudioOnly.PanelsToggle") + QStringLiteral("   ") +
					       shortcut.toString(QKeySequence::NativeText),
				       teleprompter);
	panelsToggle->setToolTip(QTStr("AudioOnly.PanelsToggleTip"));
	panelsToggle->setCheckable(true);
	panelsToggle->setChecked(config_get_bool(config, "AudioOnly", "BottomPanelsVisible"));
	teleprompter->AddToolbarButton(panelsToggle);

	connect(panelsToggle, &QPushButton::toggled, this, [this, config](bool show) {
		ShowBottomPanels(show);
		config_set_bool(config, "AudioOnly", "BottomPanelsVisible", show);
	});
	/* the mixer can also be opened from the Docks menu, keep the button in step */
	connect(ui->mixerDock->toggleViewAction(), &QAction::toggled, panelsToggle, [this](bool visible) {
		if (IsAudioOnlyMode() && panelsToggle->isChecked() != visible) {
			QSignalBlocker blocker(panelsToggle);
			panelsToggle->setChecked(visible);
			config_set_bool(App()->GetUserConfig(), "AudioOnly", "BottomPanelsVisible", visible);
		}
	});
}

namespace {
constexpr const char *kPanelsSection = "AudioOnly";
constexpr const char *kPanelsHeightKey = "BottomPanelsHeight";
constexpr int kDefaultPanelsHeight = 200;
/* dragged less than this the panels close, the mixer is useless any smaller */
constexpr int kMinPanelsHeight = 90;
} // namespace

void OBSBasic::ShowBottomPanels(bool show)
{
	bool wasVisible = ui->mixerDock->isVisible();
	ui->mixerDock->setVisible(show);
	if (controlsDock) {
		controlsDock->setVisible(show);
	}
	/* while the grip is dragged it sets the height itself */
	if (!show || wasVisible || gripDragging || !IsAudioOnlyMode()) {
		return;
	}
	/* open at the height they had last time; the docks need a moment to lay out first */
	config_t *config = App()->GetUserConfig();
	config_set_default_int(config, kPanelsSection, kPanelsHeightKey, kDefaultPanelsHeight);
	int height = (int)config_get_int(config, kPanelsSection, kPanelsHeightKey);
	QTimer::singleShot(0, this, [this, height]() { ResizeBottomPanels(height); });
}

void OBSBasic::ResizeBottomPanels(int height)
{
	QList<QDockWidget *> docks{ui->mixerDock};
	if (controlsDock) {
		docks.append(controlsDock);
	}
	QList<int> heights(docks.size(), height);
	resizeDocks(docks, heights, Qt::Vertical);
}

void OBSBasic::CreatePanelGrip(QBoxLayout *centralLayout)
{
	panelGrip = new PanelGrip(ui->centralwidget);
	panelGrip->setToolTip(QTStr("AudioOnly.PanelGripTip"));
	if (centralLayout) {
		centralLayout->addWidget(panelGrip);
	}

	panelGrip->onPress = [this]() {
		gripDragging = true;
		gripStartHeight = ui->mixerDock->isVisible() ? ui->mixerDock->height() : 0;
	};
	panelGrip->onDrag = [this](int movedUp) {
		int height = gripStartHeight + movedUp;
		bool open = height >= kMinPanelsHeight;
		if (panelsToggle && panelsToggle->isChecked() != open) {
			/* the toggle opens or closes the panels and remembers the choice */
			panelsToggle->setChecked(open);
		}
		if (open) {
			ResizeBottomPanels(height);
		}
	};
	panelGrip->onRelease = [this]() {
		gripDragging = false;
		if (ui->mixerDock->isVisible()) {
			config_set_int(App()->GetUserConfig(), kPanelsSection, kPanelsHeightKey, ui->mixerDock->height());
		}
	};
}

/* With the mixer and the controls hidden, the teleprompter's row holds what is
 * left of them: "● Record ⌘R" with the time, the microphone with its level
 * line, and a Settings button at the right end. */
void OBSBasic::CreateCompactBar()
{
	recordButton = new RecordButton(teleprompter);
	teleprompter->AddToolbarButton(recordButton, Teleprompter::ToolbarSide::Start);
	connect(recordButton, &QPushButton::clicked, this, &OBSBasic::ToggleRecordingFromShortcut);
	connect(this, &OBSBasic::RecordingStarted, recordButton, [this]() {
		recordButton->RecordingStarted();
		teleprompter->SetRecording(true);
	});
	connect(this, &OBSBasic::RecordingPaused, recordButton, [this]() { recordButton->RecordingPaused(); });
	connect(this, &OBSBasic::RecordingUnpaused, recordButton, [this]() { recordButton->RecordingUnpaused(); });
	connect(this, &OBSBasic::RecordingStopped, recordButton, [this]() {
		recordButton->RecordingStopped();
		teleprompter->SetRecording(false);
	});

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
	add(QKeySequence(Qt::CTRL | Qt::Key_J), [this]() {
		if (panelsToggle) {
			panelsToggle->toggle();
		}
	});
	add(QKeySequence(Qt::CTRL | Qt::Key_T), [this]() {
		if (takesToggle) {
			takesToggle->toggle();
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
	if (panelGrip) {
		panelGrip->setVisible(audioOnly);
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
	if (audioFilterOrder) {
		audioFilterOrder->Attach(nullptr);
	}
	if (micButton) {
		micButton->hide();
	}
	ui->contextContainer->setVisible(config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowContextToolbars"));
}

/* Menu entries that only make sense with a video canvas are hidden in audio
 * mode; Video mode shows the full OBS menus again. */
void OBSBasic::ApplyAudioOnlyMenus(bool audioOnly)
{
	QList<QAction *> videoActions{ui->sceneCollectionMenu->menuAction(),
				      ui->transformMenu->menuAction(),
				      ui->orderMenu->menuAction(),
				      ui->scalingMenu->menuAction(),
				      ui->actionLockPreview,
				      ui->actionCopySource,
				      ui->actionPasteRef,
				      ui->actionPasteDup,
				      ui->sceneListModeMenu->menuAction(),
				      ui->multiviewProjectorMenu->menuAction(),
				      ui->toggleListboxToolbars,
				      ui->toggleContextBar,
				      ui->toggleSourceIcons,
				      ui->autoConfigure,
				      ui->scenesDock->toggleViewAction(),
				      ui->sourcesDock->toggleViewAction(),
				      ui->transitionsDock->toggleViewAction()};
	for (QAction *action : videoActions) {
		if (action) {
			action->setVisible(!audioOnly);
		}
	}
}

/* Audio-only mode hides everything a narrator doesn't need: the preview, scenes,
 * sources, transitions and the streaming / virtual camera / studio mode buttons. */
void OBSBasic::UpdateAudioOnlyLayout()
{
	bool audioOnly = IsAudioOnlyMode();
	/* the first call (at startup) always runs, the window may still carry the
	 * other mode's layout from the last session */
	if (audioOnlyLayoutApplied && audioOnly == audioOnlyLayoutActive) {
		return;
	}
	audioOnlyLayoutApplied = true;

	ApplyAudioOnlyDocks(audioOnly);
	ApplyAudioOnlyCentralArea(audioOnly);
	ApplyAudioOnlyMenus(audioOnly);
	if (audioOnly) {
		SelectAudioOnlyMicrophone();
	}

	audioOnlyLayoutActive = audioOnly;
	emit AudioOnlyModeChanged(audioOnly);
}
