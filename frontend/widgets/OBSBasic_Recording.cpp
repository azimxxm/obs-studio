/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <components/TakesPanel.hpp>
#include <components/Teleprompter.hpp>

#include <QShortcut>
#include <QSplitter>

#include <components/UIValidation.hpp>
#include <dialogs/OBSRemux.hpp>

#include <qt-wrappers.hpp>

#include <QDesktopServices>
#include <QFileInfo>

void OBSBasic::on_actionShow_Recordings_triggered()
{
	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	const char *type = config_get_string(activeConfiguration, "AdvOut", "RecType");
	const char *adv_path = strcmp(type, "Standard")
				       ? config_get_string(activeConfiguration, "AdvOut", "FFFilePath")
				       : config_get_string(activeConfiguration, "AdvOut", "RecFilePath");
	const char *path = strcmp(mode, "Advanced") ? config_get_string(activeConfiguration, "SimpleOutput", "FilePath")
						    : adv_path;
	QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

#define RECORDING_START "==== Recording Start ==============================================="
#define RECORDING_STOP "==== Recording Stop ================================================"

extern volatile bool replaybuf_active;

void OBSBasic::AutoRemux(QString input, bool no_show)
{
	auto config = Config();

	bool autoRemux = config_get_bool(config, "Video", "AutoRemux");

	if (!autoRemux) {
		return;
	}

	bool isSimpleMode = false;

	const char *mode = config_get_string(config, "Output", "Mode");
	if (!mode) {
		isSimpleMode = true;
	} else {
		isSimpleMode = strcmp(mode, "Simple") == 0;
	}

	if (!isSimpleMode) {
		const char *recType = config_get_string(config, "AdvOut", "RecType");

		bool ffmpegOutput = astrcmpi(recType, "FFmpeg") == 0;

		if (ffmpegOutput) {
			return;
		}
	}

	if (input.isEmpty()) {
		return;
	}

	QFileInfo fi(input);
	QString suffix = fi.suffix();

	/* do not remux if lossless */
	if (suffix.compare("avi", Qt::CaseInsensitive) == 0) {
		return;
	}

	QString path = fi.path();

	QString output = input;
	output.resize(output.size() - suffix.size());

	const obs_encoder_t *videoEncoder = obs_output_get_video_encoder(outputHandler->fileOutput);
	const char *vCodecName = obs_encoder_get_codec(videoEncoder);
	const char *format = config_get_string(config, isSimpleMode ? "SimpleOutput" : "AdvOut", "RecFormat2");

	/* Retain original container for fMP4/fMOV */
	if (strncmp(format, "fragmented", 10) == 0) {
		output += "remuxed." + suffix;
	} else if (strcmp(vCodecName, "prores") == 0) {
		output += "mov";
	} else {
		output += "mp4";
	}

	OBSRemux *remux = new OBSRemux(QT_TO_UTF8(path), this, true);
	if (!no_show) {
		remux->show();
	}
	remux->AutoRemux(input, output);
}

void OBSBasic::StartRecording()
{
	if (outputHandler->RecordingActive()) {
		return;
	}
	if (disableOutputsRef) {
		return;
	}

	if (!OutputPathValid()) {
		OutputPathInvalidMessage();
		return;
	}

	if (!IsFFmpegOutputToURL() && LowDiskSpace()) {
		DiskSpaceMessage();
		return;
	}

	OnEvent(OBS_FRONTEND_EVENT_RECORDING_STARTING);

	SaveProject();

	outputHandler->StartRecording();
}

void OBSBasic::RecordStopping()
{
	emit RecordingStopping();

	if (sysTrayRecord) {
		sysTrayRecord->setText(QTStr("Basic.Main.StoppingRecording"));
	}

	recordingStopping = true;
	OnEvent(OBS_FRONTEND_EVENT_RECORDING_STOPPING);
}

void OBSBasic::StopRecording()
{
	SaveProject();

	if (outputHandler->RecordingActive()) {
		outputHandler->StopRecording(recordingStopping);
	}

	OnDeactivate();
}

void OBSBasic::RecordingStart()
{
	ui->statusbar->RecordingStarted(outputHandler->fileOutput);
	emit RecordingStarted(isRecordingPausable);

	if (sysTrayRecord) {
		sysTrayRecord->setText(QTStr("Basic.Main.StopRecording"));
	}

	recordingStopping = false;
	OnEvent(OBS_FRONTEND_EVENT_RECORDING_STARTED);

	if (!diskFullTimer->isActive()) {
		diskFullTimer->start(1000);
	}

	OnActivate();

	blog(LOG_INFO, RECORDING_START);
}

void OBSBasic::RecordingStop(int code, QString last_error)
{
	ui->statusbar->RecordingStopped();
	emit RecordingStopped();

	if (sysTrayRecord) {
		sysTrayRecord->setText(QTStr("Basic.Main.StartRecording"));
	}

	blog(LOG_INFO, RECORDING_STOP);

	if (code == OBS_OUTPUT_UNSUPPORTED && isVisible()) {
		OBSMessageBox::critical(this, QTStr("Output.RecordFail.Title"), QTStr("Output.RecordFail.Unsupported"));

	} else if (code == OBS_OUTPUT_ENCODE_ERROR && isVisible()) {
		QString msg = last_error.isEmpty()
				      ? QTStr("Output.RecordError.EncodeErrorMsg")
				      : QTStr("Output.RecordError.EncodeErrorMsg.LastError").arg(last_error);
		OBSMessageBox::warning(this, QTStr("Output.RecordError.Title"), msg);

	} else if (code == OBS_OUTPUT_NO_SPACE && isVisible()) {
		OBSMessageBox::warning(this, QTStr("Output.RecordNoSpace.Title"), QTStr("Output.RecordNoSpace.Msg"));

	} else if (code != OBS_OUTPUT_SUCCESS && isVisible()) {

		const char *errorDescription;
		DStr errorMessage;
		bool use_last_error = true;

		errorDescription = Str("Output.RecordError.Msg");

		if (use_last_error && !last_error.isEmpty()) {
			dstr_printf(errorMessage, "%s<br><br>%s", errorDescription, QT_TO_UTF8(last_error));
		} else {
			dstr_copy(errorMessage, errorDescription);
		}

		OBSMessageBox::critical(this, QTStr("Output.RecordError.Title"), QT_UTF8(errorMessage));

	} else if (code == OBS_OUTPUT_UNSUPPORTED && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordFail.Unsupported"), QSystemTrayIcon::Warning);

	} else if (code == OBS_OUTPUT_NO_SPACE && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordNoSpace.Msg"), QSystemTrayIcon::Warning);

	} else if (code != OBS_OUTPUT_SUCCESS && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordError.Msg"), QSystemTrayIcon::Warning);
	} else if (code == OBS_OUTPUT_SUCCESS) {
		if (outputHandler) {
			std::string path = outputHandler->lastRecordingPath;
			QString str = QTStr("Basic.StatusBar.RecordingSavedTo");
			ShowStatusBarMessage(str.arg(QT_UTF8(path.c_str())));
		}
	}

	OnEvent(OBS_FRONTEND_EVENT_RECORDING_STOPPED);

	if (diskFullTimer->isActive()) {
		diskFullTimer->stop();
	}

	AutoRemux(outputHandler->lastRecordingPath.c_str());

	OnDeactivate();
}

void OBSBasic::RecordingFileChanged(QString lastRecordingPath)
{
	QString str = QTStr("Basic.StatusBar.RecordingSavedTo");
	ShowStatusBarMessage(str.arg(lastRecordingPath));

	AutoRemux(lastRecordingPath, true);
}

void OBSBasic::RecordActionTriggered()
{
	if (outputHandler->RecordingActive()) {
		bool confirm = config_get_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStoppingRecord");

		if (confirm && isVisible()) {
			QMessageBox::StandardButton button = OBSMessageBox::question(
				this, QTStr("ConfirmStopRecord.Title"), QTStr("ConfirmStopRecord.Text"),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

			if (button == QMessageBox::No) {
				return;
			}
		}
		StopRecording();
	} else {
		/* Audio-only recordings don't need any video source, so skip the warning */
		if (!IsAudioOnlyMode() && !UIValidation::NoSourcesConfirmation(this)) {
			return;
		}

		StartRecording();
	}
}

bool OBSBasic::RecordingActive()
{
	if (!outputHandler) {
		return false;
	}
	return outputHandler->RecordingActive();
}

void OBSBasic::PauseRecording()
{
	if (!isRecordingPausable || !outputHandler || !outputHandler->fileOutput ||
	    os_atomic_load_bool(&recording_paused)) {
		return;
	}

	obs_output_t *output = outputHandler->fileOutput;

	if (obs_output_pause(output, true)) {
		os_atomic_set_bool(&recording_paused, true);

		emit RecordingPaused();

		ui->statusbar->RecordingPaused();

		TaskbarOverlaySetStatus(TaskbarOverlayStatusPaused);
		if (trayIcon && trayIcon->isVisible()) {
#ifdef __APPLE__
			QIcon trayIconFile = QIcon(":/res/images/obs_paused_macos.svg");
			trayIconFile.setIsMask(true);
#else
			QIcon trayIconFile = QIcon(":/res/images/obs_paused.png");
#endif
			trayIcon->setIcon(QIcon::fromTheme("obs-tray-paused", trayIconFile));
		}

		OnEvent(OBS_FRONTEND_EVENT_RECORDING_PAUSED);

		if (os_atomic_load_bool(&replaybuf_active)) {
			ShowReplayBufferPauseWarning();
		}
	}
}

void OBSBasic::UnpauseRecording()
{
	if (!isRecordingPausable || !outputHandler || !outputHandler->fileOutput ||
	    !os_atomic_load_bool(&recording_paused)) {
		return;
	}

	obs_output_t *output = outputHandler->fileOutput;

	if (obs_output_pause(output, false)) {
		os_atomic_set_bool(&recording_paused, false);

		emit RecordingUnpaused();

		ui->statusbar->RecordingUnpaused();

		TaskbarOverlaySetStatus(TaskbarOverlayStatusActive);
		if (trayIcon && trayIcon->isVisible()) {
#ifdef __APPLE__
			QIcon trayIconFile = QIcon(":/res/images/tray_active_macos.svg");
			trayIconFile.setIsMask(true);
#else
			QIcon trayIconFile = QIcon(":/res/images/tray_active.png");
#endif
			trayIcon->setIcon(QIcon::fromTheme("obs-tray-active", trayIconFile));
		}

		OnEvent(OBS_FRONTEND_EVENT_RECORDING_UNPAUSED);
	}
}

void OBSBasic::RecordPauseToggled()
{
	if (!isRecordingPausable || !outputHandler || !outputHandler->fileOutput) {
		return;
	}

	obs_output_t *output = outputHandler->fileOutput;
	bool enable = !obs_output_paused(output);

	if (enable) {
		PauseRecording();
	} else {
		UnpauseRecording();
	}
}

void OBSBasic::UpdateIsRecordingPausable()
{
	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	bool adv = astrcmpi(mode, "Advanced") == 0;
	bool shared = true;

	if (adv) {
		const char *recType = config_get_string(activeConfiguration, "AdvOut", "RecType");

		if (astrcmpi(recType, "FFmpeg") == 0) {
			shared = config_get_bool(activeConfiguration, "AdvOut", "FFOutputToFile");
		} else {
			const char *recordEncoder = config_get_string(activeConfiguration, "AdvOut", "RecEncoder");
			shared = astrcmpi(recordEncoder, "none") == 0;
		}
	} else {
		const char *quality = config_get_string(activeConfiguration, "SimpleOutput", "RecQuality");
		shared = strcmp(quality, "Stream") == 0;
	}

	isRecordingPausable = !shared;
}

#define MBYTE (1024ULL * 1024ULL)
#define MBYTES_LEFT_STOP_REC 50ULL
#define MAX_BYTES_LEFT (MBYTES_LEFT_STOP_REC * MBYTE)

void OBSBasic::DiskSpaceMessage()
{
	blog(LOG_ERROR, "Recording stopped because of low disk space");

	OBSMessageBox::critical(this, QTStr("Output.RecordNoSpace.Title"), QTStr("Output.RecordNoSpace.Msg"));
}

bool OBSBasic::LowDiskSpace()
{
	const char *path;

	path = GetCurrentOutputPath();
	if (!path) {
		return false;
	}

	uint64_t num_bytes = os_get_free_disk_space(path);

	if (num_bytes < (MAX_BYTES_LEFT)) {
		return true;
	} else {
		return false;
	}
}

void OBSBasic::CheckDiskSpaceRemaining()
{
	if (LowDiskSpace()) {
		StopRecording();
		StopReplayBuffer();

		DiskSpaceMessage();
	}
}

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

/* The teleprompter replaces the preview and the device bar moves to the top */
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

	connect(this, &OBSBasic::RecordingStarted, teleprompter, &Teleprompter::StartScrolling);
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
		audioOnlyShortcuts.append(shortcut);
	};
	add(QKeySequence(Qt::CTRL | Qt::Key_R), [this]() { ToggleRecordingFromShortcut(); });
	add(QKeySequence(Qt::CTRL | Qt::Key_P), [this]() {
		if (takesPanel && !RecordingActive()) {
			takesPanel->PlayLatest();
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

	if (centralLayout) {
		centralLayout->removeWidget(ui->contextContainer);
		int index = audioOnly ? 0 : centralLayout->indexOf(ui->canvasEditor) + 1;
		centralLayout->insertWidget(index, ui->contextContainer);
	}
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
