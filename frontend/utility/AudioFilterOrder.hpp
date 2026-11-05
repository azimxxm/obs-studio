#pragma once

#include <obs.hpp>

#include <QHash>
#include <QObject>
#include <QString>

class QListWidget;

/* Puts a newly added audio filter where it belongs in a voice chain:
 * rumble cut -> noise removal -> EQ / VST -> de-esser -> compressor -> gain -> limiter.
 * Only the filter that was just added is moved; a later manual reorder is kept.
 * A second noise remover, or a second copy of a filter that must run once,
 * turns the older one off (not deleted) and says why. */
class AudioFilterOrder : public QObject {
	Q_OBJECT

public:
	explicit AudioFilterOrder(QObject *parent = nullptr);
	~AudioFilterOrder() override;

	void Attach(obs_source_t *source);

	/* position in the chain for a filter type, or -1 when we don't know it */
	static int Rank(const char *filterId);

	/* the "Auto" button: puts every audio filter of source in the voice-chain order */
	static void SortAll(obs_source_t *source);

	/* enabled filters that get in each other's way, with a plain explanation */
	static QHash<obs_source_t *, QString> FindConflicts(obs_source_t *source);
	/* yellow ⚠ next to those filters in the Filters window; hover explains it */
	static void ShowConflicts(obs_source_t *source, QListWidget *list);

signals:
	void Notice(const QString &message);

private slots:
	void PlaceFilter(OBSWeakSource filter);

private:
	OBSWeakSource weakSource;
	OBSSignal addedSignal;

	static void FilterAdded(void *data, calldata_t *params);
	void TurnOffDuplicates(obs_source_t *source, obs_source_t *added);
};
