#include "AudioEditor.hpp"

#include <components/WaveformView.hpp>
#include <utility/VoiceEffects.hpp>

#include <OBSApp.hpp>

#include <cmath>

#include <QApplication>
#include <QAudioFormat>
#include <QAudioSink>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QMenu>
#include <QtConcurrent/QtConcurrentRun>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QSlider>
#include <QLabel>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

#include <qt-wrappers.hpp>

#include "moc_AudioEditor.cpp"

namespace {
constexpr int kPlayheadIntervalMs = 30;
/* same target Audacity uses by default, leaves a little room so nothing clips */
constexpr float kNormalizeTargetDb = -1.0f;
constexpr float kCeilingDb = -1.0f;
constexpr float kRumbleHz = 80.0f;
constexpr double kMinPauseSec = 0.8;
constexpr double kKeepPauseSec = 0.4;
constexpr double kFadeSec = 1.0;
/* gentle enough to keep the voice natural, see the measurements in VoiceEffects */
constexpr float kStudioNoiseReductionDb = 15.0f;
constexpr double kDefaultLoudness = -14.0;
constexpr const char *kConfigSection = "AudioEditor";

struct LoudnessPreset {
	const char *label;
	double lufs;
};
/* what each platform plays back at, so the narration is not turned down or up */
const LoudnessPreset kLoudnessPresets[] = {
	{"AudioEditor.Target.Social", -14.0},
	{"AudioEditor.Target.Podcast", -16.0},
	{"AudioEditor.Target.Audiobook", -20.0},
};

QVector<QPair<qint64, qint64>> ToCuts(const std::vector<VoiceEffects::Range> &ranges)
{
	QVector<QPair<qint64, qint64>> cuts;
	for (const VoiceEffects::Range &range : ranges) {
		cuts.append({qint64(range.start), qint64(range.end)});
	}
	return cuts;
}

QString FormatTime(qint64 frames, bool withTenths)
{
	double seconds = double(frames) / AudioTimeline::kSampleRate;
	int whole = int(seconds);
	QString text = QStringLiteral("%1:%2").arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0'));
	if (withTenths) {
		text += QStringLiteral(".%1").arg(int((seconds - whole) * 10));
	}
	return text;
}
} // namespace

AudioEditor::AudioEditor(const QStringList &paths_, const QString &exportFolder_, QWidget *parent)
	: QDialog(parent),
	  paths(paths_),
	  exportFolder(exportFolder_)
{
	setWindowTitle(QTStr("AudioEditor.Title"));
	resize(1100, 520);

	auto *hint = new QLabel(QTStr("AudioEditor.Hint"), this);
	hint->setWordWrap(true);
	hint->setStyleSheet("color: #b0b0b8;");

	waveform = new WaveformView(this);
	infoLabel = new QLabel(this);
	infoLabel->setStyleSheet("font-size: 14px;");

	playButton = new QPushButton(this);
	playButton->setMinimumSize(120, 38);
	playSelectionButton = new QPushButton(QTStr("AudioEditor.PlaySelection"), this);
	cutButton = new QPushButton(QTStr("AudioEditor.Cut"), this);
	undoButton = new QPushButton(QTStr("AudioEditor.Undo"), this);
	normalizeButton = new QPushButton(QTStr("AudioEditor.Normalize"), this);
	normalizeButton->setToolTip(QTStr("AudioEditor.NormalizeTip"));
	auto *zoomInButton = new QPushButton("+", this);
	auto *zoomOutButton = new QPushButton("−", this);
	auto *fitButton = new QPushButton(QTStr("AudioEditor.Fit"), this);
	selectToolButton = new QPushButton(QTStr("AudioEditor.ToolSelect"), this);
	handToolButton = new QPushButton(QTStr("AudioEditor.ToolHand"), this);
	for (QPushButton *button : {selectToolButton, handToolButton}) {
		button->setCheckable(true);
		button->setAutoExclusive(true);
	}
	selectToolButton->setChecked(true);
	selectToolButton->setToolTip(QTStr("AudioEditor.ToolSelectTip"));
	handToolButton->setToolTip(QTStr("AudioEditor.ToolHandTip"));
	polishButton = new QPushButton(QTStr("AudioEditor.Polish"), this);
	polishButton->setProperty("class", "btn-primary");
	polishButton->setMinimumSize(190, 38);
	polishButton->setToolTip(QTStr("AudioEditor.PolishTip"));
	effectsButton = new QPushButton(QTStr("AudioEditor.Effects"), this);
	effectsButton->setMinimumHeight(38);
	effectsButton->setMenu(CreateEffectsMenu());
	effectsButton->setToolTip(QTStr("AudioEditor.EffectsTip"));
	compareButton = new QPushButton(QTStr("AudioEditor.HearBefore"), this);
	compareButton->setCheckable(true);
	compareButton->setMinimumHeight(38);
	compareButton->setToolTip(QTStr("AudioEditor.HearBeforeTip"));
	exportButton = new QPushButton(QTStr("AudioEditor.Export"), this);
	exportButton->setProperty("class", "btn-primary");
	exportButton->setMinimumSize(170, 38);
	auto *closeButton = new QPushButton(QTStr("Close"), this);

	zoomInButton->setToolTip(QTStr("AudioEditor.ZoomIn"));
	zoomOutButton->setToolTip(QTStr("AudioEditor.ZoomOut"));
	cutButton->setToolTip(QTStr("AudioEditor.CutTip"));

	auto *tools = new QHBoxLayout;
	tools->addWidget(playButton);
	tools->addWidget(playSelectionButton);
	tools->addSpacing(16);
	tools->addWidget(cutButton);
	tools->addWidget(undoButton);
	tools->addWidget(normalizeButton);
	tools->addSpacing(16);
	tools->addWidget(zoomOutButton);
	tools->addWidget(zoomInButton);
	tools->addWidget(fitButton);
	tools->addSpacing(16);
	tools->addWidget(selectToolButton);
	tools->addWidget(handToolButton);
	tools->addStretch();
	tools->addWidget(infoLabel);

	auto *bottom = new QHBoxLayout;
	bottom->addWidget(polishButton);
	bottom->addWidget(effectsButton);
	bottom->addWidget(compareButton);
	bottom->addStretch();
	bottom->addWidget(closeButton);
	bottom->addWidget(exportButton);

	auto *layout = new QVBoxLayout(this);
	layout->setSpacing(10);
	layout->addWidget(hint);
	layout->addWidget(waveform, 1);
	layout->addLayout(tools);
	layout->addLayout(bottom);

	playheadTimer = new QTimer(this);
	playheadTimer->setInterval(kPlayheadIntervalMs);

	connect(playButton, &QPushButton::clicked, this, &AudioEditor::TogglePlayback);
	connect(playSelectionButton, &QPushButton::clicked, this, &AudioEditor::PlaySelection);
	connect(cutButton, &QPushButton::clicked, this, &AudioEditor::CutSelection);
	connect(undoButton, &QPushButton::clicked, this, &AudioEditor::UndoCut);
	connect(normalizeButton, &QPushButton::clicked, this, &AudioEditor::NormalizeVolume);
	connect(zoomInButton, &QPushButton::clicked, waveform, &WaveformView::ZoomIn);
	connect(zoomOutButton, &QPushButton::clicked, waveform, &WaveformView::ZoomOut);
	connect(fitButton, &QPushButton::clicked, waveform, &WaveformView::ZoomToFit);
	connect(selectToolButton, &QPushButton::clicked, this,
		[this]() { waveform->SetTool(WaveformView::Tool::Select); });
	connect(handToolButton, &QPushButton::clicked, this, [this]() { waveform->SetTool(WaveformView::Tool::Hand); });
	connect(waveform, &WaveformView::ToolChanged, this, [this](WaveformView::Tool tool) {
		(tool == WaveformView::Tool::Hand ? handToolButton : selectToolButton)->setChecked(true);
	});
	connect(exportButton, &QPushButton::clicked, this, &AudioEditor::Export);
	connect(polishButton, &QPushButton::clicked, this, &AudioEditor::PolishVoice);
	connect(compareButton, &QPushButton::clicked, this, &AudioEditor::ToggleCompare);
	connect(closeButton, &QPushButton::clicked, this, &AudioEditor::reject);
	connect(playheadTimer, &QTimer::timeout, this, &AudioEditor::UpdatePlayhead);
	connect(waveform, &WaveformView::SelectionChanged, this, &AudioEditor::UpdateInfo);
	connect(waveform, &WaveformView::CursorMoved, this, [this]() {
		if (IsPlaying()) {
			StartPlayback(waveform->Cursor(), timeline.TotalFrames());
		}
		UpdateInfo();
	});

	/* keys an audio editor user expects */
	auto addShortcut = [this](const QKeySequence &key, auto slot) {
		auto *shortcut = new QShortcut(key, this);
		connect(shortcut, &QShortcut::activated, this, slot);
	};
	addShortcut(QKeySequence(Qt::Key_Space), &AudioEditor::TogglePlayback);
	addShortcut(QKeySequence(Qt::Key_Delete), &AudioEditor::CutSelection);
	addShortcut(QKeySequence(Qt::Key_Backspace), &AudioEditor::CutSelection);
	addShortcut(QKeySequence::Undo, &AudioEditor::UndoCut);
	/* H = hand to drag the timeline, A = back to selecting (like design apps) */
	auto *handKey = new QShortcut(QKeySequence(Qt::Key_H), this);
	connect(handKey, &QShortcut::activated, this, [this]() { waveform->SetTool(WaveformView::Tool::Hand); });
	auto *selectKey = new QShortcut(QKeySequence(Qt::Key_A), this);
	connect(selectKey, &QShortcut::activated, this, [this]() { waveform->SetTool(WaveformView::Tool::Select); });
	auto *selectAll = new QShortcut(QKeySequence::SelectAll, this);
	connect(selectAll, &QShortcut::activated, waveform, &WaveformView::SelectAll);
	addShortcut(QKeySequence::Save, &AudioEditor::Export);

	for (QPushButton *button : findChildren<QPushButton *>()) {
		/* keep Space for play/stop instead of clicking the focused button */
		button->setFocusPolicy(Qt::NoFocus);
		button->setAutoDefault(false);
	}

	UpdateInfo();
}

AudioEditor::~AudioEditor()
{
	StopPlayback();
}

void AudioEditor::showEvent(QShowEvent *event)
{
	QDialog::showEvent(event);
	if (!loaded) {
		loaded = true;
		/* decode after the window is on screen so it doesn't look frozen */
		QTimer::singleShot(0, this, &AudioEditor::LoadTakes);
	}
}

void AudioEditor::reject()
{
	StopPlayback();
	QDialog::reject();
}

void AudioEditor::LoadTakes()
{
	infoLabel->setText(QTStr("AudioEditor.Loading"));
	QApplication::setOverrideCursor(Qt::WaitCursor);
	QString error;
	bool ok = timeline.Load(paths, error);
	QApplication::restoreOverrideCursor();

	if (!ok) {
		OBSMessageBox::warning(this, QTStr("AudioEditor.Title"), QTStr("AudioEditor.LoadFailed").arg(error));
		reject();
		return;
	}

	waveform->SetTimeline(&timeline);
	waveform->setFocus();
	UpdateInfo();
}

void AudioEditor::StartPlayback(qint64 from, qint64 to)
{
	StopPlayback();
	if (to <= from) {
		return;
	}

	QAudioFormat format;
	format.setSampleRate(AudioTimeline::kSampleRate);
	format.setChannelCount(timeline.Channels());
	format.setSampleFormat(QAudioFormat::Float);

	device = new AudioTimelineDevice(&timeline, from, to, this);
	device->open(QIODevice::ReadOnly);

	sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), format, this);
	connect(sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
		/* idle means the device ran out of audio: we reached the end */
		if (state == QAudio::IdleState && device && device->atEnd()) {
			QTimer::singleShot(0, this, &AudioEditor::StopPlayback);
		}
	});
	sink->start(device);
	playheadTimer->start();
	UpdateInfo();
}

void AudioEditor::StopPlayback()
{
	playheadTimer->stop();
	if (sink) {
		sink->stop();
		sink->deleteLater();
		sink = nullptr;
	}
	if (device) {
		device->deleteLater();
		device = nullptr;
	}
	waveform->SetPlayhead(-1);
	UpdateInfo();
}

void AudioEditor::TogglePlayback()
{
	if (IsPlaying()) {
		StopPlayback();
		return;
	}
	qint64 from = waveform->HasSelection() ? waveform->SelectionStart() : waveform->Cursor();
	if (from >= timeline.TotalFrames()) {
		from = 0;
	}
	StartPlayback(from, timeline.TotalFrames());
}

void AudioEditor::PlaySelection()
{
	if (waveform->HasSelection()) {
		StartPlayback(waveform->SelectionStart(), waveform->SelectionEnd());
	}
}

void AudioEditor::CutSelection()
{
	if (processing || timeline.IsComparing()) {
		return;
	}
	if (!waveform->HasSelection()) {
		return;
	}
	StopPlayback();
	qint64 start = waveform->SelectionStart();
	timeline.Cut(start, waveform->SelectionEnd());
	waveform->TimelineChanged();
	UpdateInfo();
}

void AudioEditor::UndoCut()
{
	if (processing || timeline.IsComparing()) {
		return;
	}
	if (!timeline.CanUndo()) {
		return;
	}
	StopPlayback();
	timeline.Undo();
	waveform->TimelineChanged();
	UpdateInfo();
}

void AudioEditor::NormalizeVolume()
{
	if (processing || timeline.IsComparing()) {
		return;
	}
	StopPlayback();
	if (timeline.Normalize(kNormalizeTargetDb)) {
		waveform->update();
	}
	UpdateInfo();
}

double AudioEditor::LoudnessTarget()
{
	config_t *config = App()->GetUserConfig();
	config_set_default_double(config, kConfigSection, "LoudnessTarget", kDefaultLoudness);
	return config_get_double(config, kConfigSection, "LoudnessTarget");
}

void AudioEditor::SetLoudnessTarget(double lufs)
{
	config_set_double(App()->GetUserConfig(), kConfigSection, "LoudnessTarget", lufs);
}

QMenu *AudioEditor::CreateEffectsMenu()
{
	auto *menu = new QMenu(this);

	menu->addAction(QTStr("AudioEditor.Fx.NoiseAI"), this, [this]() {
		RunEffect(QTStr("AudioEditor.Fx.NoiseAI"),
			  OnSelection([](std::vector<float> &s, int ch, auto &) { return VoiceEffects::RemoveNoise(s, ch); }));
	})->setEnabled(VoiceEffects::CanRemoveNoise());

	menu->addAction(QTStr("AudioEditor.Fx.NoiseStudio"), this, [this]() {
		RunEffect(QTStr("AudioEditor.Fx.NoiseStudio"), OnSelection([](std::vector<float> &s, int ch, auto &) {
				  VoiceEffects::CleanNoiseSpectral(s, ch, kStudioNoiseReductionDb);
				  return true;
			  }));
	});

	menu->addAction(QTStr("AudioEditor.Fx.Rumble"), this, [this]() {
		RunEffect(QTStr("AudioEditor.Fx.Rumble"), OnSelection([](std::vector<float> &s, int ch, auto &) {
				  VoiceEffects::HighPass(s, ch, kRumbleHz);
				  return true;
			  }));
	});

	menu->addAction(QTStr("AudioEditor.Fx.Even"), this, [this]() {
		RunEffect(QTStr("AudioEditor.Fx.Even"), OnSelection([](std::vector<float> &s, int ch, auto &) {
				  /* the compressor works at a known level, then we go back to where we were */
				  double target = VoiceEffects::MeasureLoudness(s, ch);
				  VoiceEffects::NormalizeLoudness(s, ch, -20.0, kCeilingDb);
				  VoiceEffects::Compress(s, ch, -26.0f, 3.0f, 5.0f, 120.0f);
				  VoiceEffects::NormalizeLoudness(s, ch, target, kCeilingDb);
				  return true;
			  }));
	});

	menu->addAction(QTStr("AudioEditor.Fx.Pauses"), this, [this]() {
		StopPlayback();
		std::vector<float> audio = timeline.Render();
		auto cuts = ToCuts(VoiceEffects::FindLongPauses(audio, timeline.Channels(), kMinPauseSec, kKeepPauseSec));
		if (cuts.isEmpty()) {
			infoLabel->setText(QTStr("AudioEditor.NoPauses"));
			return;
		}
		/* pauses are cut like a manual edit, the audio itself is not changed */
		timeline.CutRanges(cuts);
		waveform->TimelineChanged();
		UpdateInfo();
	});

	menu->addSeparator();
	QMenu *loudness = menu->addMenu(QTStr("AudioEditor.Fx.Loudness"));
	for (const LoudnessPreset &preset : kLoudnessPresets) {
		QAction *action = loudness->addAction(QTStr(preset.label));
		action->setCheckable(true);
		action->setChecked(std::fabs(LoudnessTarget() - preset.lufs) < 0.1);
		double lufs = preset.lufs;
		connect(action, &QAction::triggered, this, [this, lufs, name = QTStr(preset.label)]() {
			SetLoudnessTarget(lufs);
			RunEffect(name, [lufs](std::vector<float> &s, int ch, auto &) {
				return VoiceEffects::NormalizeLoudness(s, ch, lufs, kCeilingDb);
			});
		});
	}
	connect(loudness, &QMenu::aboutToShow, this, [loudness]() {
		/* keep the check mark on the target that Polish Voice will use */
		int i = 0;
		for (QAction *action : loudness->actions()) {
			action->setChecked(std::fabs(LoudnessTarget() - kLoudnessPresets[i++].lufs) < 0.1);
		}
	});

	menu->addSeparator();
	menu->addAction(QTStr("AudioEditor.Fx.Volume"), this, &AudioEditor::ChangeVolume);
	menu->addAction(QTStr("AudioEditor.Fx.FadeIn"), this, [this]() { ApplyFade(true); });
	menu->addAction(QTStr("AudioEditor.Fx.FadeOut"), this, [this]() { ApplyFade(false); });
	return menu;
}

void AudioEditor::ApplyFade(bool fadeIn)
{
	/* the selection if there is one, otherwise the first or last second */
	const qint64 total = timeline.TotalFrames();
	const qint64 length = std::min<qint64>(total, qint64(kFadeSec * AudioTimeline::kSampleRate));
	qint64 from = fadeIn ? 0 : total - length;
	qint64 to = fadeIn ? length : total;
	if (waveform->HasSelection()) {
		from = waveform->SelectionStart();
		to = waveform->SelectionEnd();
	}

	QString name = QTStr(fadeIn ? "AudioEditor.Fx.FadeIn" : "AudioEditor.Fx.FadeOut");
	RunEffect(name, [from, to, fadeIn](std::vector<float> &s, int ch, auto &) {
		VoiceEffects::Fade(s, ch, from, to, fadeIn);
		return true;
	});
}

void AudioEditor::ChangeVolume()
{
	if (processing || timeline.IsComparing() || timeline.TotalFrames() == 0) {
		return;
	}
	/* slider works in tenths of a dB */
	constexpr int kMinTenths = -240;
	constexpr int kMaxTenths = 120;
	constexpr int kStartTenths = -30;

	const qint64 from = waveform->HasSelection() ? waveform->SelectionStart() : 0;
	const qint64 to = waveform->HasSelection() ? waveform->SelectionEnd() : timeline.TotalFrames();

	QDialog dialog(this);
	dialog.setWindowTitle(QTStr("AudioEditor.Fx.Volume"));
	auto *prompt = new QLabel(QTStr(waveform->HasSelection() ? "AudioEditor.VolumePromptSelection"
								 : "AudioEditor.VolumePromptAll"),
				  &dialog);
	prompt->setWordWrap(true);
	auto *slider = new QSlider(Qt::Horizontal, &dialog);
	slider->setRange(kMinTenths, kMaxTenths);
	slider->setValue(kStartTenths);
	slider->setMinimumWidth(360);
	auto *valueLabel = new QLabel(&dialog);
	valueLabel->setMinimumWidth(70);
	valueLabel->setStyleSheet("font-size: 15px; font-weight: 600;");
	auto *listen = new QPushButton(QTStr("AudioEditor.Play"), &dialog);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);

	auto *row = new QHBoxLayout;
	row->addWidget(slider, 1);
	row->addWidget(valueLabel);
	auto *layout = new QVBoxLayout(&dialog);
	layout->addWidget(prompt);
	layout->addLayout(row);
	auto *bottom = new QHBoxLayout;
	bottom->addWidget(listen);
	bottom->addStretch();
	bottom->addWidget(buttons);
	layout->addLayout(bottom);

	/* every slider move is drawn and heard right away */
	auto preview = [&]() {
		double db = slider->value() / 10.0;
		valueLabel->setText(QStringLiteral("%1%2 dB").arg(db > 0 ? "+" : "").arg(db, 0, 'f', 1));
		timeline.SetPreviewGain(from, to, float(db));
		waveform->update();
	};
	connect(slider, &QSlider::valueChanged, &dialog, preview);
	connect(listen, &QPushButton::clicked, &dialog, [&]() {
		if (IsPlaying()) {
			StopPlayback();
		} else {
			StartPlayback(from, to);
		}
		listen->setText(IsPlaying() ? QTStr("AudioEditor.Stop") : QTStr("AudioEditor.Play"));
	});
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	preview();

	bool accepted = dialog.exec() == QDialog::Accepted;
	double gainDb = slider->value() / 10.0;
	StopPlayback();
	timeline.ClearPreview();
	waveform->update();
	if (!accepted || std::fabs(gainDb) < 0.05) {
		return;
	}

	RunEffect(QTStr("AudioEditor.Fx.Volume"), [from, to, gainDb](std::vector<float> &s, int ch, auto &) {
		VoiceEffects::ChangeVolume(s, ch, from, to, float(gainDb));
		/* louder can go over the top, the limiter keeps it clean */
		if (gainDb > 0) {
			VoiceEffects::Limit(s, ch, kCeilingDb);
		}
		return true;
	});
}

void AudioEditor::PolishVoice()
{
	double target = LoudnessTarget();
	RunEffect(QTStr("AudioEditor.Polish"), [target](std::vector<float> &s, int ch, auto &cuts) {
		VoiceEffects::PolishVoice(s, ch, target);
		cuts = ToCuts(VoiceEffects::FindLongPauses(s, ch, kMinPauseSec, kKeepPauseSec));
		return true;
	});
}

AudioEditor::EffectWork AudioEditor::OnSelection(EffectWork work) const
{
	if (!waveform->HasSelection()) {
		return work;
	}

	const qint64 from = waveform->SelectionStart();
	const qint64 to = waveform->SelectionEnd();
	return [work, from, to](std::vector<float> &samples, int channels, QVector<QPair<qint64, qint64>> &) {
		/* half a second around the selection lets filters and the noise
		 * remover settle before the part that is kept */
		constexpr qint64 kContext = AudioTimeline::kSampleRate / 2;
		constexpr qint64 kBlend = AudioTimeline::kSampleRate / 100;
		const qint64 total = qint64(samples.size()) / channels;
		const qint64 first = std::max<qint64>(0, from - kContext);
		const qint64 last = std::min(total, to + kContext);

		std::vector<float> part(samples.begin() + first * channels, samples.begin() + last * channels);
		QVector<QPair<qint64, qint64>> unused;
		if (!work(part, channels, unused)) {
			return false;
		}

		/* blend in and out over 10 ms so the edges of the selection don't click */
		for (qint64 f = from; f < to; f++) {
			double mix = 1.0;
			if (from > 0) {
				mix = std::min(mix, double(f - from) / kBlend);
			}
			if (to < total) {
				mix = std::min(mix, double(to - f) / kBlend);
			}
			mix = std::clamp(mix, 0.0, 1.0);
			for (int c = 0; c < channels; c++) {
				float &out = samples[size_t(f * channels + c)];
				out = float((1.0 - mix) * out + mix * part[size_t((f - first) * channels + c)]);
			}
		}
		return true;
	};
}

void AudioEditor::RunEffect(const QString &name, EffectWork work)
{
	if (processing || timeline.TotalFrames() == 0) {
		return;
	}
	StopPlayback();
	if (timeline.IsComparing()) {
		ToggleCompare();
	}

	processing = true;
	UpdateInfo();
	infoLabel->setText(QTStr("AudioEditor.Working").arg(name));
	QApplication::setOverrideCursor(Qt::BusyCursor);

	struct Result {
		bool ok = false;
		std::vector<float> samples;
		QVector<QPair<qint64, qint64>> cuts;
	};

	/* the worker gets its own copy, so the timeline is never touched off the UI thread */
	auto future = QtConcurrent::run([audio = timeline.Render(), channels = timeline.Channels(),
					 work]() mutable {
		Result result;
		result.ok = work(audio, channels, result.cuts);
		result.samples = std::move(audio);
		return result;
	});

	auto *watcher = new QFutureWatcher<Result>(this);
	connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, name]() {
		Result result = watcher->result();
		watcher->deleteLater();
		QApplication::restoreOverrideCursor();
		processing = false;

		if (!result.ok) {
			UpdateInfo();
			infoLabel->setText(QTStr("AudioEditor.EffectFailed").arg(name));
			return;
		}
		StopPlayback();
		timeline.ReplaceWithRendered(std::move(result.samples), result.cuts);
		waveform->TimelineChanged();
		UpdateInfo();
	});
	watcher->setFuture(future);
}

void AudioEditor::ToggleCompare()
{
	bool wasPlaying = IsPlaying();
	qint64 cursor = waveform->Cursor();
	StopPlayback();
	timeline.ToggleCompare();
	waveform->TimelineChanged();
	if (wasPlaying) {
		StartPlayback(std::min(cursor, timeline.TotalFrames()), timeline.TotalFrames());
	}
	UpdateInfo();
}

void AudioEditor::UpdatePlayhead()
{
	if (!sink || !device) {
		return;
	}
	/* the sink still holds some audio it hasn't played yet */
	const qint64 frameBytes = timeline.Channels() * qint64(sizeof(float));
	qint64 buffered = (sink->bufferSize() - sink->bytesFree()) / frameBytes;
	waveform->SetPlayhead(std::max<qint64>(0, device->CurrentFrame() - buffered));
}

void AudioEditor::UpdateInfo()
{
	bool hasAudio = timeline.TotalFrames() > 0;
	bool hasSelection = waveform->HasSelection();

	playButton->setText(IsPlaying() ? QTStr("AudioEditor.Stop") : QTStr("AudioEditor.Play"));
	/* while hearing the "before" version or processing, nothing may change the audio */
	bool canEdit = hasAudio && !processing && !timeline.IsComparing();
	playButton->setEnabled(hasAudio && !processing);
	playSelectionButton->setEnabled(hasSelection && !processing);
	cutButton->setEnabled(hasSelection && canEdit);
	undoButton->setEnabled(timeline.CanUndo() && canEdit);
	normalizeButton->setEnabled(canEdit);
	polishButton->setEnabled(canEdit);
	effectsButton->setEnabled(canEdit);
	exportButton->setEnabled(canEdit);
	compareButton->setEnabled(timeline.CanCompare() && !processing);
	compareButton->setChecked(timeline.IsComparing());

	if (!hasAudio) {
		return;
	}
	QString text = QTStr("AudioEditor.Total").arg(FormatTime(timeline.TotalFrames(), false));
	text += QStringLiteral("   ·   ") + QTStr(timeline.TakeCount() == 1 ? "AudioEditor.OneTake" : "AudioEditor.Takes").arg(timeline.TakeCount());
	float gainDb = timeline.GainDb();
	if (std::fabs(gainDb) >= 0.1f) {
		text += QStringLiteral("   ·   ") + QTStr("AudioEditor.Volume")
					.arg((gainDb > 0 ? QStringLiteral("+") : QString()) + QString::number(gainDb, 'f', 1));
	}
	if (hasSelection) {
		text += QStringLiteral("   ·   ") +
			QTStr("AudioEditor.Selected")
				.arg(FormatTime(waveform->SelectionEnd() - waveform->SelectionStart(), true));
	}
	infoLabel->setText(text);
}

void AudioEditor::Export()
{
	if (processing || timeline.IsComparing()) {
		return;
	}
	if (timeline.TotalFrames() == 0) {
		return;
	}
	StopPlayback();

	QString name = QString::fromUtf8(kExportPrefix) + QStringLiteral("%1.wav").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm-ss"));
	QString path = QFileDialog::getSaveFileName(this, QTStr("AudioEditor.Export"), QDir(exportFolder).filePath(name),
						    QStringLiteral("WAV (*.wav)"));
	if (path.isEmpty()) {
		return;
	}
	if (!path.endsWith(".wav", Qt::CaseInsensitive)) {
		path += ".wav";
	}

	QApplication::setOverrideCursor(Qt::WaitCursor);
	QString error;
	bool ok = timeline.ExportWav(path, error);
	QApplication::restoreOverrideCursor();

	if (!ok) {
		OBSMessageBox::warning(this, QTStr("AudioEditor.Title"), QTStr("AudioEditor.ExportFailed").arg(error));
		return;
	}

	emit Exported(path);
	OBSMessageBox::information(this, QTStr("AudioEditor.Title"), QTStr("AudioEditor.ExportDone").arg(path));
}
