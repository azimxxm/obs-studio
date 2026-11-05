#pragma once

#include <QWidget>

class QAudioOutput;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMediaPlayer;
class QPushButton;

/* Lists the audio files in the recording folder so a narrator can listen to a
 * take right away, throw away a bad one, or join several in the editor. */
class TakesPanel : public QWidget {
	Q_OBJECT

public:
	explicit TakesPanel(QWidget *parent = nullptr);

public slots:
	void Refresh();
	void RefreshAfterRecording();
	void PlayLatest();
	void DeleteLatest();
	void StopPlayback();

private slots:
	void TogglePlaySelected();
	void DeleteSelected();
	void ShowSelectedInFinder();
	void OpenEditor();
	void UpdateButtons();

private:
	QListWidget *list = nullptr;
	QLabel *countLabel = nullptr;
	QLabel *emptyLabel = nullptr;
	QPushButton *playButton = nullptr;
	QPushButton *deleteButton = nullptr;
	QPushButton *finderButton = nullptr;
	QPushButton *editButton = nullptr;
	QMediaPlayer *player = nullptr;
	QAudioOutput *audioOutput = nullptr;
	QString playingPath;

	void Play(const QString &path);
	void MoveToTrash(const QStringList &paths);
	QStringList SelectedPaths() const;
	QString LatestPath() const;
	static QString RecordingFolder();
};
