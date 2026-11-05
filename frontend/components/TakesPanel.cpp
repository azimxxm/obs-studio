#include "TakesPanel.hpp"
#include "AudioTimeline.hpp"

#include <dialogs/AudioEditor.hpp>
#include <widgets/OBSBasic.hpp>
#include <OBSApp.hpp>

#include <algorithm>

#include <QAudioOutput>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QProcess>
#include <QShortcut>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <qt-wrappers.hpp>

#include "moc_TakesPanel.cpp"

namespace {
constexpr int kMaxTakes = 200;
/* the muxer needs a moment to finish the file header after stop */
constexpr int kRefreshDelayMs = 600;
constexpr int kPathRole = Qt::UserRole;

QString FormatDuration(double seconds)
{
	if (seconds < 0) {
		return QString();
	}
	int total = int(seconds + 0.5);
	return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString FormatWhen(const QDateTime &time)
{
	QDate today = QDate::currentDate();
	QString clock = time.toString("HH:mm:ss");
	if (time.date() == today) {
		return clock;
	}
	return time.toString("d MMM ") + clock;
}
} // namespace

TakesPanel::TakesPanel(QWidget *parent) : QWidget(parent)
{
	setObjectName("takesPanel");
	setMinimumWidth(260);

	auto *title = new QLabel(QTStr("Takes.Title"), this);
	title->setStyleSheet("font-size: 15px; font-weight: 600;");
	countLabel = new QLabel(this);
	countLabel->setStyleSheet("color: #9a9aa4;");

	list = new QListWidget(this);
	list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	list->setAlternatingRowColors(false);
	list->setUniformItemSizes(true);
	list->setStyleSheet("QListWidget::item { padding: 6px 4px; }");
	auto *deleteKey = new QShortcut(QKeySequence::Delete, list, nullptr, nullptr, Qt::WidgetShortcut);
	connect(deleteKey, &QShortcut::activated, this, &TakesPanel::DeleteSelected);
	auto *backspaceKey = new QShortcut(QKeySequence(Qt::Key_Backspace), list, nullptr, nullptr, Qt::WidgetShortcut);
	connect(backspaceKey, &QShortcut::activated, this, &TakesPanel::DeleteSelected);

	emptyLabel = new QLabel(QTStr("Takes.Empty"), this);
	emptyLabel->setWordWrap(true);
	emptyLabel->setAlignment(Qt::AlignCenter);
	emptyLabel->setStyleSheet("color: #9a9aa4; padding: 16px;");

	playButton = new QPushButton(this);
	deleteButton = new QPushButton(QTStr("Takes.Delete"), this);
	finderButton = new QPushButton(QTStr("Takes.ShowInFinder"), this);
	editButton = new QPushButton(QTStr("Takes.EditJoin"), this);
	editButton->setProperty("class", "btn-primary");
	for (QPushButton *button : {playButton, deleteButton, finderButton}) {
		button->setMinimumHeight(32);
	}
	editButton->setMinimumHeight(40);
	playButton->setToolTip(QTStr("Takes.PlayTip"));
	deleteButton->setToolTip(QTStr("Takes.DeleteTip"));

	auto *shortcutHint = new QLabel(QTStr("Takes.Shortcuts"), this);
	shortcutHint->setWordWrap(true);
	shortcutHint->setStyleSheet("color: #8a8a94; font-size: 11px;");

	auto *header = new QHBoxLayout;
	header->addWidget(title);
	header->addStretch();
	header->addWidget(countLabel);

	auto *rowButtons = new QHBoxLayout;
	rowButtons->addWidget(playButton, 1);
	rowButtons->addWidget(deleteButton, 1);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 8, 10, 8);
	layout->setSpacing(8);
	layout->addLayout(header);
	layout->addWidget(list, 1);
	layout->addWidget(emptyLabel, 1);
	layout->addLayout(rowButtons);
	layout->addWidget(finderButton);
	layout->addWidget(editButton);
	layout->addWidget(shortcutHint);

	player = new QMediaPlayer(this);
	audioOutput = new QAudioOutput(this);
	player->setAudioOutput(audioOutput);

	connect(playButton, &QPushButton::clicked, this, &TakesPanel::TogglePlaySelected);
	connect(deleteButton, &QPushButton::clicked, this, &TakesPanel::DeleteSelected);
	connect(finderButton, &QPushButton::clicked, this, &TakesPanel::ShowSelectedInFinder);
	connect(editButton, &QPushButton::clicked, this, &TakesPanel::OpenEditor);
	connect(list, &QListWidget::itemSelectionChanged, this, &TakesPanel::UpdateButtons);
	connect(list, &QListWidget::itemDoubleClicked, this,
		[this](QListWidgetItem *item) { Play(item->data(kPathRole).toString()); });
	connect(player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
		if (state == QMediaPlayer::StoppedState) {
			playingPath.clear();
		}
		UpdateButtons();
	});

	Refresh();
}

QString TakesPanel::RecordingFolder()
{
	OBSBasic *main = OBSBasic::Get();
	const char *path = main ? config_get_string(main->Config(), "SimpleOutput", "FilePath") : nullptr;
	return path ? QString::fromUtf8(path) : QString();
}

void TakesPanel::Refresh()
{
	QStringList selected = SelectedPaths();

	QDir dir(RecordingFolder());
	QFileInfoList files = dir.entryInfoList({"*.wav", "*.flac", "*.mp3", "*.m4a"}, QDir::Files, QDir::Time);
	if (files.size() > kMaxTakes) {
		files = files.mid(0, kMaxTakes);
	}

	list->clear();
	/* newest on top, but numbered in the order they were recorded */
	for (int i = 0; i < files.size(); i++) {
		const QFileInfo &file = files[i];
		QString duration = FormatDuration(ReadWavDurationSeconds(file.filePath()));
		QString details = FormatWhen(file.lastModified());
		if (!duration.isEmpty()) {
			details += QStringLiteral("  ·  ") + duration;
		}
		if (file.suffix().compare("wav", Qt::CaseInsensitive) != 0) {
			details += QStringLiteral("  ·  ") + file.suffix().toUpper();
		}

		auto *item = new QListWidgetItem(
			QStringLiteral("%1 %2\n%3").arg(QTStr("Takes.Take")).arg(files.size() - i).arg(details), list);
		item->setData(kPathRole, file.filePath());
		item->setToolTip(file.fileName());
		if (selected.contains(file.filePath())) {
			item->setSelected(true);
		}
	}

	countLabel->setText(files.isEmpty() ? QString() : QString::number(files.size()));
	emptyLabel->setVisible(files.isEmpty());
	list->setVisible(!files.isEmpty());
	UpdateButtons();
}

void TakesPanel::RefreshAfterRecording()
{
	QTimer::singleShot(kRefreshDelayMs, this, [this]() {
		Refresh();
		if (list->count() > 0) {
			list->clearSelection();
			list->item(0)->setSelected(true);
			list->setCurrentRow(0);
		}
	});
}

QStringList TakesPanel::SelectedPaths() const
{
	QStringList paths;
	for (QListWidgetItem *item : list->selectedItems()) {
		paths << item->data(kPathRole).toString();
	}
	return paths;
}

QString TakesPanel::LatestPath() const
{
	return list->count() > 0 ? list->item(0)->data(kPathRole).toString() : QString();
}

void TakesPanel::Play(const QString &path)
{
	if (path.isEmpty()) {
		return;
	}
	player->stop();
	playingPath = path;
	player->setSource(QUrl::fromLocalFile(path));
	player->play();
	UpdateButtons();
}

void TakesPanel::StopPlayback()
{
	player->stop();
	player->setSource(QUrl());
	playingPath.clear();
	UpdateButtons();
}

void TakesPanel::TogglePlaySelected()
{
	if (!playingPath.isEmpty()) {
		StopPlayback();
		return;
	}
	QStringList paths = SelectedPaths();
	Play(paths.isEmpty() ? LatestPath() : paths.first());
}

void TakesPanel::PlayLatest()
{
	if (!playingPath.isEmpty()) {
		StopPlayback();
		return;
	}
	Refresh();
	Play(LatestPath());
}

void TakesPanel::MoveToTrash(const QStringList &paths)
{
	StopPlayback();
	for (const QString &path : paths) {
		/* Trash, not delete: a take removed by mistake can still be put back */
		if (!QFile::moveToTrash(path)) {
			blog(LOG_WARNING, "Takes: could not move '%s' to the Trash", QT_TO_UTF8(path));
		}
	}
	Refresh();
}

void TakesPanel::DeleteSelected()
{
	QStringList paths = SelectedPaths();
	if (!paths.isEmpty()) {
		MoveToTrash(paths);
	}
}

void TakesPanel::DeleteLatest()
{
	Refresh();
	QString latest = LatestPath();
	if (!latest.isEmpty()) {
		MoveToTrash({latest});
	}
}

void TakesPanel::ShowSelectedInFinder()
{
	QStringList paths = SelectedPaths();
	QString target = paths.isEmpty() ? RecordingFolder() : paths.first();
#ifdef __APPLE__
	QStringList args = paths.isEmpty() ? QStringList{target} : QStringList{"-R", target};
	QProcess::startDetached("open", args);
#else
	QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(target).absolutePath()));
#endif
}

void TakesPanel::OpenEditor()
{
	QStringList paths = SelectedPaths();
	if (paths.isEmpty()) {
		/* nothing picked: use every take, but not files the editor exported before */
		for (int i = 0; i < list->count(); i++) {
			QString path = list->item(i)->data(kPathRole).toString();
			if (!QFileInfo(path).fileName().startsWith(AudioEditor::kExportPrefix)) {
				paths << path;
			}
		}
	}
	if (paths.isEmpty()) {
		return;
	}

	/* join in the order the takes were recorded */
	std::sort(paths.begin(), paths.end(), [](const QString &a, const QString &b) {
		return QFileInfo(a).lastModified() < QFileInfo(b).lastModified();
	});

	StopPlayback();
	auto *editor = new AudioEditor(paths, RecordingFolder(), window());
	editor->setAttribute(Qt::WA_DeleteOnClose);
	connect(editor, &AudioEditor::Exported, this, &TakesPanel::Refresh);
	editor->open();
}

void TakesPanel::UpdateButtons()
{
	int selected = int(list->selectedItems().size());
	bool hasTakes = list->count() > 0;

	playButton->setText(playingPath.isEmpty() ? QTStr("Takes.Play") : QTStr("Takes.Stop"));
	playButton->setEnabled(hasTakes);
	deleteButton->setEnabled(selected > 0);
	editButton->setEnabled(hasTakes);
	editButton->setText(selected > 1 ? QTStr("Takes.EditJoinCount").arg(selected) : QTStr("Takes.EditJoin"));
}
