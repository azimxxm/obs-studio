#include "AudioEditor.hpp"

#include <components/WaveformView.hpp>

#include <OBSApp.hpp>

#include <QApplication>
#include <QAudioFormat>
#include <QAudioSink>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
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
	auto *zoomInButton = new QPushButton("+", this);
	auto *zoomOutButton = new QPushButton("−", this);
	auto *fitButton = new QPushButton(QTStr("AudioEditor.Fit"), this);
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
	tools->addSpacing(16);
	tools->addWidget(zoomOutButton);
	tools->addWidget(zoomInButton);
	tools->addWidget(fitButton);
	tools->addStretch();
	tools->addWidget(infoLabel);

	auto *bottom = new QHBoxLayout;
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
	connect(zoomInButton, &QPushButton::clicked, waveform, &WaveformView::ZoomIn);
	connect(zoomOutButton, &QPushButton::clicked, waveform, &WaveformView::ZoomOut);
	connect(fitButton, &QPushButton::clicked, waveform, &WaveformView::ZoomToFit);
	connect(exportButton, &QPushButton::clicked, this, &AudioEditor::Export);
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
	if (!timeline.CanUndo()) {
		return;
	}
	StopPlayback();
	timeline.Undo();
	waveform->TimelineChanged();
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
	playButton->setEnabled(hasAudio);
	playSelectionButton->setEnabled(hasSelection);
	cutButton->setEnabled(hasSelection);
	undoButton->setEnabled(timeline.CanUndo());
	exportButton->setEnabled(hasAudio);

	if (!hasAudio) {
		return;
	}
	QString text = QTStr("AudioEditor.Total").arg(FormatTime(timeline.TotalFrames(), false));
	text += QStringLiteral("   ·   ") + QTStr("AudioEditor.Takes").arg(timeline.Sources().size());
	if (hasSelection) {
		text += QStringLiteral("   ·   ") +
			QTStr("AudioEditor.Selected")
				.arg(FormatTime(waveform->SelectionEnd() - waveform->SelectionStart(), true));
	}
	infoLabel->setText(text);
}

void AudioEditor::Export()
{
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
