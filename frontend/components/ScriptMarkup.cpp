#include "ScriptMarkup.hpp"

#include <algorithm>
#include <vector>

#include <QColor>
#include <QFont>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>

namespace ScriptMarkup {

namespace {
constexpr int kLineKindProperty = QTextFormat::UserProperty + 1;
constexpr int kEmphasisProperty = QTextFormat::UserProperty + 2;
constexpr int kStretchProperty = QTextFormat::UserProperty + 3;
constexpr int kPauseProperty = QTextFormat::UserProperty + 4;

constexpr QChar kEmphasisMarker = u'*';
constexpr QChar kStretchMarker = u'~';
constexpr QChar kEscape = u'\\';
constexpr QChar kEllipsis = u'…';
constexpr QChar kSoftLineBreak = u' ';
constexpr int kDotsInPause = 3;
const QString kScenePrefix = QStringLiteral("# ");
const QString kHookPrefix = QStringLiteral("! ");

const QColor kTextColor(0xf2, 0xf2, 0xf2);
const QColor kHookColor(0xff, 0xff, 0xff);
const QColor kMutedColor(0x9a, 0x9c, 0xb8);
const QColor kEmphasisColor(0xff, 0xa2, 0x3f);
const QColor kStretchLineColor(0x8b, 0x5c, 0xf6);

constexpr double kToneSizeRatio = 0.45;
constexpr double kSceneSizeRatio = 0.5;
constexpr double kHookSizeRatio = 1.15;
constexpr int kMinSmallFontSize = 13;
constexpr double kStretchLetterSpacing = 118.0;
constexpr double kSceneLetterSpacing = 112.0;
constexpr double kSceneMarginRatio = 0.4;

struct Segment {
	QString text;
	WordMarks marks;
};

struct ParsedLine {
	LineKind kind = LineKind::Normal;
	std::vector<Segment> segments;
};

struct FormatRange {
	int position;
	int length;
	WordMarks marks;
};

bool HasWordMarks(LineKind kind)
{
	return kind == LineKind::Normal || kind == LineKind::Hook;
}

int PauseLengthAt(const QString &line, int index)
{
	if (line[index] == kEllipsis) {
		return 1;
	}
	int dots = 0;
	while (index + dots < line.size() && line[index + dots] == u'.') {
		dots++;
	}
	return dots >= kDotsInPause ? dots : 0;
}

/* A marker only counts when it has a partner later in the line,
 * so a lonely "~1 second" or "5 * 3" stays normal text. */
bool HasClosingMarker(const QString &line, int from, QChar marker)
{
	for (int i = from; i < line.size(); i++) {
		if (line[i] == kEscape) {
			i++;
		} else if (line[i] == marker) {
			return true;
		}
	}
	return false;
}

std::vector<Segment> ParseWords(const QString &line)
{
	std::vector<Segment> segments;
	WordMarks marks;
	QString pending;

	auto flush = [&]() {
		if (!pending.isEmpty()) {
			segments.push_back({pending, marks});
			pending.clear();
		}
	};
	auto toggle = [&](bool &flag, QChar marker, int next) {
		if (!flag && !HasClosingMarker(line, next, marker)) {
			return false;
		}
		flush();
		flag = !flag;
		return true;
	};

	for (int i = 0; i < line.size(); i++) {
		QChar c = line[i];
		if (c == kEscape && i + 1 < line.size()) {
			pending += line[++i];
			continue;
		}
		if (c == kEmphasisMarker) {
			/* "**bold**" from a markdown file reads the same as "*bold*" */
			int last = i;
			while (last + 1 < line.size() && line[last + 1] == kEmphasisMarker) {
				last++;
			}
			if (toggle(marks.emphasis, kEmphasisMarker, last + 1)) {
				i = last;
				continue;
			}
		} else if (c == kStretchMarker) {
			if (toggle(marks.stretch, kStretchMarker, i + 1)) {
				continue;
			}
		} else if (int pauseLength = PauseLengthAt(line, i); pauseLength > 0) {
			flush();
			WordMarks pauseMarks;
			pauseMarks.pause = true;
			segments.push_back({line.mid(i, pauseLength), pauseMarks});
			i += pauseLength - 1;
			continue;
		}
		pending += c;
	}
	flush();
	return segments;
}

ParsedLine ParseLine(const QString &line)
{
	ParsedLine parsed;
	if (line.startsWith(u'#')) {
		int start = 0;
		while (start < line.size() && line[start] == u'#') {
			start++;
		}
		parsed.kind = LineKind::Scene;
		parsed.segments.push_back({line.mid(start).trimmed(), {}});
	} else if (line.size() >= 2 && line.startsWith(u'[') && line.endsWith(u']')) {
		parsed.kind = LineKind::Tone;
		parsed.segments.push_back({line.mid(1, line.size() - 2), {}});
	} else if (line.startsWith(kHookPrefix)) {
		parsed.kind = LineKind::Hook;
		parsed.segments = ParseWords(line.mid(kHookPrefix.size()));
	} else {
		parsed.segments = ParseWords(line);
	}
	return parsed;
}

QString EscapeWords(const QString &text)
{
	QString escaped;
	for (QChar c : text) {
		if (c == kEscape || c == kEmphasisMarker || c == kStretchMarker) {
			escaped += kEscape;
		}
		escaped += c == kSoftLineBreak ? QChar(u' ') : c;
	}
	return escaped;
}

QString SaveWords(const QTextBlock &block)
{
	QString line;
	WordMarks open;

	auto switchTo = [&](const WordMarks &wanted) {
		if (open.emphasis != wanted.emphasis) {
			line += kEmphasisMarker;
		}
		if (open.stretch != wanted.stretch) {
			line += kStretchMarker;
		}
		open = wanted;
	};

	for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
		QTextFragment fragment = it.fragment();
		WordMarks marks = WordMarksOf(fragment.charFormat());
		if (marks.pause) {
			switchTo({});
			line += fragment.text();
			continue;
		}
		switchTo(marks);
		line += EscapeWords(fragment.text());
	}
	switchTo({});
	return line;
}

QString SaveLine(const QTextBlock &block)
{
	QString text = block.text();
	if (text.trimmed().isEmpty()) {
		return QString();
	}

	switch (LineKindOf(block)) {
	case LineKind::Scene:
		return kScenePrefix + text;
	case LineKind::Tone:
		return u'[' + text + u']';
	case LineKind::Hook:
		return kHookPrefix + SaveWords(block);
	case LineKind::Normal:
		break;
	}

	QString line = SaveWords(block);
	/* keep a normal line from being read back as a scene, tone or hook line */
	if (line.startsWith(u'#') || line.startsWith(u'[') || line.startsWith(kHookPrefix)) {
		line.prepend(kEscape);
	}
	return line;
}

void RestyleBlock(const QTextBlock &block, int baseFontSize)
{
	LineKind kind = LineKindOf(block);

	/* collect first: changing formats while walking the fragments would move them */
	std::vector<FormatRange> ranges;
	for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
		QTextFragment fragment = it.fragment();
		ranges.push_back({fragment.position(), fragment.length(), WordMarksOf(fragment.charFormat())});
	}

	QTextCursor cursor(block);
	cursor.setBlockFormat(BlockFormat(kind, baseFontSize));
	cursor.setBlockCharFormat(CharFormat(kind, {}, baseFontSize));
	for (const FormatRange &range : ranges) {
		cursor.setPosition(range.position);
		cursor.setPosition(range.position + range.length, QTextCursor::KeepAnchor);
		cursor.setCharFormat(CharFormat(kind, range.marks, baseFontSize));
	}
}

template<typename Action> void ForEachSelectedBlock(const QTextCursor &cursor, Action action)
{
	QTextDocument *document = cursor.document();
	if (!document) {
		return;
	}
	int last = document->findBlock(cursor.selectionEnd()).blockNumber();
	for (int number = document->findBlock(cursor.selectionStart()).blockNumber(); number <= last; number++) {
		action(document->findBlockByNumber(number));
	}
}

void SetBlockKind(const QTextBlock &block, LineKind kind, int baseFontSize)
{
	QTextCursor(block).setBlockFormat(BlockFormat(kind, baseFontSize));
	RestyleBlock(block, baseFontSize);
}

bool CoversWholeBlock(const QTextCursor &cursor, const QTextBlock &block)
{
	int lastPosition = block.position() + block.length() - 1;
	return cursor.selectionStart() <= block.position() && cursor.selectionEnd() >= lastPosition;
}

/* When only a part of one line is selected, that part moves to a line of its own,
 * so a hook, tone note or scene title can be made from just the selected words.
 * The cursor ends up in that new line. */
void SplitOutSelection(QTextCursor &cursor)
{
	QTextDocument *document = cursor.document();
	if (!document || !cursor.hasSelection()) {
		return;
	}
	int start = cursor.selectionStart();
	int end = cursor.selectionEnd();
	QTextBlock block = document->findBlock(start);
	if (document->findBlock(end) != block) {
		return;
	}

	QString text = block.text();
	bool textBefore = !text.left(start - block.position()).trimmed().isEmpty();
	bool textAfter = !text.mid(end - block.position()).trimmed().isEmpty();

	/* the end goes first, so the start position stays where it is */
	QTextCursor edit(document);
	if (textAfter) {
		edit.setPosition(end);
		while (!edit.atBlockEnd() && document->characterAt(edit.position()).isSpace()) {
			edit.deleteChar();
		}
		edit.insertBlock();
	}
	if (textBefore) {
		edit.setPosition(start);
		while (!edit.atBlockStart() && document->characterAt(edit.position() - 1).isSpace()) {
			edit.deletePreviousChar();
		}
		edit.insertBlock();
		start = edit.position();
	}
	cursor.setPosition(start);
}
} // namespace

LineKind LineKindOf(const QTextBlock &block)
{
	return static_cast<LineKind>(block.blockFormat().intProperty(kLineKindProperty));
}

WordMarks WordMarksOf(const QTextCharFormat &format)
{
	WordMarks marks;
	marks.emphasis = format.boolProperty(kEmphasisProperty);
	marks.stretch = format.boolProperty(kStretchProperty);
	marks.pause = format.boolProperty(kPauseProperty);
	return marks;
}

bool IsCardLine(const QTextBlock &block)
{
	return LineKindOf(block) != LineKind::Scene && !block.text().trimmed().isEmpty();
}

QTextCharFormat CharFormat(LineKind kind, const WordMarks &wantedMarks, int baseFontSize)
{
	WordMarks marks = HasWordMarks(kind) ? wantedMarks : WordMarks();

	QTextCharFormat format;
	format.setProperty(kEmphasisProperty, marks.emphasis);
	format.setProperty(kStretchProperty, marks.stretch);
	format.setProperty(kPauseProperty, marks.pause);
	format.setForeground(kTextColor);

	switch (kind) {
	case LineKind::Normal:
		break;
	case LineKind::Hook:
		format.setForeground(kHookColor);
		format.setFontWeight(QFont::Bold);
		format.setFontPointSize(baseFontSize * kHookSizeRatio);
		break;
	case LineKind::Tone:
		format.setForeground(kMutedColor);
		format.setFontItalic(true);
		format.setFontPointSize(std::max<double>(kMinSmallFontSize, baseFontSize * kToneSizeRatio));
		break;
	case LineKind::Scene:
		format.setForeground(kMutedColor);
		format.setFontWeight(QFont::Bold);
		format.setFontCapitalization(QFont::AllUppercase);
		format.setFontLetterSpacingType(QFont::PercentageSpacing);
		format.setFontLetterSpacing(kSceneLetterSpacing);
		format.setFontPointSize(std::max<double>(kMinSmallFontSize, baseFontSize * kSceneSizeRatio));
		break;
	}

	if (marks.pause) {
		format.setForeground(kMutedColor);
		format.setFontWeight(QFont::Normal);
		return format;
	}
	if (marks.emphasis) {
		format.setForeground(kEmphasisColor);
		format.setFontWeight(QFont::Bold);
	}
	if (marks.stretch) {
		format.setFontLetterSpacingType(QFont::PercentageSpacing);
		format.setFontLetterSpacing(kStretchLetterSpacing);
		format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
		format.setUnderlineColor(kStretchLineColor);
	}
	return format;
}

QTextBlockFormat BlockFormat(LineKind kind, int baseFontSize)
{
	QTextBlockFormat format;
	format.setProperty(kLineKindProperty, static_cast<int>(kind));
	if (kind == LineKind::Scene) {
		format.setTopMargin(baseFontSize * kSceneMarginRatio);
		format.setBottomMargin(baseFontSize * kSceneMarginRatio);
	}
	return format;
}

void Load(QTextDocument *document, const QString &markup, int baseFontSize)
{
	QString text = markup;
	text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	const QStringList lines = text.split(u'\n');

	/* loading is not something the narrator should be able to undo */
	document->setUndoRedoEnabled(false);
	document->clear();

	QTextCursor cursor(document);
	cursor.beginEditBlock();
	for (int i = 0; i < lines.size(); i++) {
		ParsedLine line = ParseLine(lines[i]);
		QTextBlockFormat blockFormat = BlockFormat(line.kind, baseFontSize);
		QTextCharFormat plainFormat = CharFormat(line.kind, {}, baseFontSize);
		if (i == 0) {
			cursor.setBlockFormat(blockFormat);
			cursor.setBlockCharFormat(plainFormat);
		} else {
			cursor.insertBlock(blockFormat, plainFormat);
		}
		for (const Segment &segment : line.segments) {
			cursor.insertText(segment.text, CharFormat(line.kind, segment.marks, baseFontSize));
		}
	}
	cursor.endEditBlock();
	document->setUndoRedoEnabled(true);
}

QString Save(const QTextDocument *document)
{
	QStringList lines;
	for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
		lines.append(SaveLine(block));
	}
	return lines.join(u'\n');
}

void SetMark(QTextCursor cursor, Mark mark, bool on, int baseFontSize)
{
	if (!cursor.hasSelection()) {
		return;
	}
	QTextCharFormat change;
	change.setProperty(mark == Mark::Emphasis ? kEmphasisProperty : kStretchProperty, on);

	cursor.beginEditBlock();
	cursor.mergeCharFormat(change);
	ForEachSelectedBlock(cursor, [&](const QTextBlock &block) { RestyleBlock(block, baseFontSize); });
	cursor.endEditBlock();
}

void SetLineKind(QTextCursor cursor, LineKind kind, int baseFontSize)
{
	cursor.beginEditBlock();
	if (kind != LineKind::Normal) {
		SplitOutSelection(cursor);
	}
	ForEachSelectedBlock(cursor, [&](const QTextBlock &block) { SetBlockKind(block, kind, baseFontSize); });
	cursor.endEditBlock();
}

void ClearMarks(QTextCursor cursor, int baseFontSize)
{
	QTextCharFormat change;
	change.setProperty(kEmphasisProperty, false);
	change.setProperty(kStretchProperty, false);

	cursor.beginEditBlock();
	if (cursor.hasSelection()) {
		cursor.mergeCharFormat(change);
	}
	/* a line only goes back to normal when all of it is selected, or nothing is */
	ForEachSelectedBlock(cursor, [&](const QTextBlock &block) {
		if (!cursor.hasSelection() || CoversWholeBlock(cursor, block)) {
			SetBlockKind(block, LineKind::Normal, baseFontSize);
		} else {
			RestyleBlock(block, baseFontSize);
		}
	});
	cursor.endEditBlock();
}

void InsertPause(QTextCursor &cursor, int baseFontSize)
{
	WordMarks pauseMarks;
	pauseMarks.pause = true;

	cursor.setPosition(cursor.selectionEnd());
	cursor.insertText(QString(kEllipsis), CharFormat(LineKindOf(cursor.block()), pauseMarks, baseFontSize));
}

void RestyleAll(QTextDocument *document, int baseFontSize)
{
	/* a text size change must not show up as an undo step */
	document->setUndoRedoEnabled(false);
	for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
		RestyleBlock(block, baseFontSize);
	}
	document->setUndoRedoEnabled(true);
}

} // namespace ScriptMarkup
