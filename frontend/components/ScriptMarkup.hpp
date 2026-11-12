#pragma once

#include <QString>

class QTextBlock;
class QTextBlockFormat;
class QTextCharFormat;
class QTextCursor;
class QTextDocument;

/* Reading marks for the teleprompter script: which words to stress or stretch,
 * where to pause, which line is a hook, a tone note or a scene title.
 *
 * The narrator sets them with buttons and never sees the file format. On disk
 * the script stays a plain text file with a few light markers:
 *
 *   # Scene title
 *   [tone note, how to read the next lines]
 *   ! A hook line
 *   Say *this* with force and ~this~ slowly... then go on.
 */
namespace ScriptMarkup {

enum class LineKind { Normal, Scene, Tone, Hook };
enum class Mark { Emphasis, Stretch };

struct WordMarks {
	bool emphasis = false;
	bool stretch = false;
	bool pause = false;
};

LineKind LineKindOf(const QTextBlock &block);
WordMarks WordMarksOf(const QTextCharFormat &format);
/* true for lines that are drawn inside a card (any line with text, except scene titles) */
bool IsCardLine(const QTextBlock &block);

/* the look of a piece of text; baseFontSize is the teleprompter text size in points */
QTextCharFormat CharFormat(LineKind kind, const WordMarks &marks, int baseFontSize);
QTextBlockFormat BlockFormat(LineKind kind, int baseFontSize);

/* replaces the document with the script read from the saved text */
void Load(QTextDocument *document, const QString &markup, int baseFontSize);
QString Save(const QTextDocument *document);

/* these work on the selection of the cursor (or on its line when nothing is selected) */
void SetMark(QTextCursor cursor, Mark mark, bool on, int baseFontSize);
/* selected words inside a line are moved to a line of their own first */
void SetLineKind(QTextCursor cursor, LineKind kind, int baseFontSize);
/* removes the word marks from the selection; a fully selected line (or the
 * cursor's line when nothing is selected) also becomes a normal line again */
void ClearMarks(QTextCursor cursor, int baseFontSize);
/* adds a pause sign after the selection and leaves the cursor behind it */
void InsertPause(QTextCursor &cursor, int baseFontSize);

/* rebuilds the look of every line, needed after the text size changes */
void RestyleAll(QTextDocument *document, int baseFontSize);

} // namespace ScriptMarkup
