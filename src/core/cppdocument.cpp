#include "cppdocument.h"

#include "logger.h"
#include "project.h"
#include "settings.h"

#include "lsp/client.h"
#include "lsp/types.h"

#include <QFileInfo>
#include <QHash>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextDocument>
#include <QVariantMap>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace Core {

/*!
 * \qmltype CppDocument
 * \brief Document object for a C++ file (source or header)
 * \instantiates Core::CppDocument
 * \inqmlmodule Script
 * \since 4.0
 * \inherits LspDocument
 */

/*!
 * \qmlproperty bool CppDocument::isHeader
 * Return true if the current document is a header.
 */

CppDocument::CppDocument(QObject *parent)
    : LspDocument(Type::Cpp, parent)
{
}

static bool isHeaderSuffix(const QString &suffix)
{
    // Good enough for now, headers starts with h or hpp
    return suffix.startsWith('h');
}

bool CppDocument::isHeader() const
{
    QFileInfo fi(fileName());
    return isHeaderSuffix(fi.suffix());
}

/*!
 * \qmlmethod CppDocument::commentSelection()
 * Comments the selected lines (or current line if there's no selection) in current document.
 *
 * - If there's no selection, current line is commented using `//`.
 * - If there's a valid selection and the start and end position of the selection are before any text of the lines,
 *   all of the selected lines are commented using `//`.
 * - If there's a valid selection and the start and/or end position of the selection are between any text of the
 *   lines, all of the selected lines are commented using multi-line comment.
 * - If selection or position is invalid or out of range, or the position is on an empty line, the document remains
 *   unchanged.
 */
void CppDocument::commentSelection()
{
    LOG("CppDocument::commentSelection");

    QTextCursor cursor = textEdit()->textCursor();
    cursor.beginEditBlock();

    int cursorPos = cursor.position();
    int selectionOffset = 0;

    if (hasSelection()) {
        int selectionStartPos = cursor.selectionStart();
        int selectionEndPos = cursor.selectionEnd();

        // Preparing to check if the start and end positions of the selection are before any text of the lines
        cursor.setPosition(selectionStartPos);
        cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor);
        const QString str1 = cursor.selectedText();
        cursor.setPosition(selectionEndPos);
        cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor);
        const QString str2 = cursor.selectedText();

        if (str1.trimmed().isEmpty() && str2.trimmed().isEmpty()) {
            // Comment all lines in the selected region with "//"
            cursor.setPosition(selectionStartPos);
            cursor.movePosition(QTextCursor::StartOfLine);
            selectionStartPos = cursor.position();

            cursor.setPosition(selectionEndPos);
            // If the end of selection is at the beginning of the line, don't comment out the line the cursor is in.
            if (str2.isEmpty())
                cursor.movePosition(QTextCursor::Left);
            cursor.movePosition(QTextCursor::StartOfLine);

            do {
                cursor.insertText("//");
                selectionOffset += 2;
                cursor.movePosition(QTextCursor::Up);
                cursor.movePosition(QTextCursor::StartOfLine);
            } while (cursor.position() >= selectionStartPos);
        } else {
            // Comment the selected region using "/*" and "*/"
            cursor.setPosition(selectionEndPos);
            cursor.insertText("*/");
            selectionOffset += 2;
            cursor.setPosition(selectionStartPos);
            cursor.insertText("/*");
            selectionOffset += 2;
        }

        // Set the selection after commenting
        if (cursorPos == selectionEndPos) {
            cursor.setPosition(selectionStartPos);
            cursor.setPosition(selectionEndPos + selectionOffset, QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(selectionEndPos + selectionOffset);
            cursor.setPosition(selectionStartPos, QTextCursor::KeepAnchor);
        }
    } else {
        cursor.select(QTextCursor::LineUnderCursor);
        // If the line is not empty, then comment it using "//"
        if (!cursor.selectedText().isEmpty()) {
            cursor.movePosition(QTextCursor::StartOfLine);
            cursor.insertText("//");
            selectionOffset += 2;
        }

        // Set the position after commenting
        cursor.setPosition(cursorPos + selectionOffset);
    }

    cursor.endEditBlock();
    textEdit()->setTextCursor(cursor);
}

static QStringList matchingSuffixes(bool header)
{
    static const auto mimeTypes =
        Settings::instance()->value<std::map<std::string, Document::Type>>(Settings::MimeTypes);

    QStringList suffixes;
    for (const auto &it : mimeTypes) {
        if (it.second == Document::Type::Cpp) {
            const QString suffix = QString::fromStdString(it.first);
            if ((header && !isHeaderSuffix(suffix)) || (!header && isHeaderSuffix(suffix)))
                suffixes.push_back(suffix);
        }
    }
    return suffixes;
}

static QStringList candidateFileNames(const QString &baseName, const QStringList &suffixes)
{
    QStringList result;
    result.reserve(suffixes.size());
    for (const auto &suffix : suffixes)
        result.push_back(baseName + '.' + suffix);
    return result;
}

static int commonFilePathLength(const QString &s1, const QString &s2)
{
    int length = qMin(s1.length(), s2.length());
    for (int i = 0; i < length; ++i) {
        if (s1[i].toLower() != s2[i].toLower())
            return i;
    }
    return length;
}

/*!
 * \qmlmethod string CppDocument::correspondingHeaderSource()
 * Returns the corresponding source or header file path.
 */
QString CppDocument::correspondingHeaderSource() const
{
    LOG("CppDocument::correspondingHeaderSource");
    static QHash<QString, QString> cache;

    QString cacheData = cache.value(fileName());
    if (!cacheData.isEmpty())
        return cacheData;

    const bool header = isHeader();
    const QStringList suffixes = matchingSuffixes(header);

    QFileInfo fi(fileName());
    QStringList candidates = candidateFileNames(fi.completeBaseName(), suffixes);

    // Search in the current directory
    for (const auto &candidate : candidates) {
        const QString testFileName = fi.absolutePath() + '/' + candidate;
        if (QFile::exists(testFileName)) {
            cache[fileName()] = testFileName;
            cache[testFileName] = fileName();
            spdlog::debug("CppDocument::correspondingHeaderSource {} => {}", fileName().toStdString(),
                          testFileName.toStdString());
            LOG_RETURN("path", testFileName);
        }
    }

    // Search in the whole project, and find the possible files
    QStringList fullPathNames = Project::instance()->allFilesWithExtensions(suffixes, Project::FullPath);
    auto checkIfPathNeedToBeRemoved = [&](const auto &path) {
        for (const auto &fileName : candidates) {
            if (path.endsWith(fileName, Qt::CaseInsensitive))
                return false;
        }
        return true;
    };
    fullPathNames.erase(std::remove_if(fullPathNames.begin(), fullPathNames.end(), checkIfPathNeedToBeRemoved),
                        fullPathNames.end());

    // Find the file having the most common path with fileName
    QString bestFileName;
    int compareValue = 0;
    for (const auto &path : fullPathNames) {
        int value = commonFilePathLength(path, fileName());
        if (value > compareValue) {
            compareValue = value;
            bestFileName = path;
        }
    }

    if (!bestFileName.isEmpty()) {
        cache[fileName()] = bestFileName;
        cache[bestFileName] = fileName();
        spdlog::debug("CppDocument::correspondingHeaderSource {} => {}", fileName().toStdString(),
                      bestFileName.toStdString());
        LOG_RETURN("path", bestFileName);
    }

    spdlog::warn("CppDocument::correspondingHeaderSource {} - not found ", fileName().toStdString());
    return {};
}

/*!
 * \qmlmethod CppDocument CppDocument::openHeaderSource()
 * Opens the corresponding source or header files, the current document is the new file.
 * If no files have been found, it's a no-op.
 */
CppDocument *CppDocument::openHeaderSource()
{
    LOG("CppDocument::openHeaderSource");
    const QString fileName = correspondingHeaderSource();
    if (!fileName.isEmpty())
        LOG_RETURN("document", qobject_cast<CppDocument *>(Project::instance()->open(fileName)));
    return nullptr;
}

/*!
 * \qmlmethod CppDocument::insertForwardDeclaration(string fwddecl)
 * Inserts the forward declaration `fwddecl` into the current file.
 * The method will check if the file is a header file, and also that the forward declaration starts with 'class ' or
 * 'struct '. Fully qualified the forward declaration to add namespaces: `class Foo::Bar::FooBar` will result in:
 *
 * ```cpp
 * namespace Foo {
 * namespace Bar {
 * class FooBar
 * }
 * }
 * ```
 */
bool CppDocument::insertForwardDeclaration(const QString &fwddecl)
{
    LOG("CppDocument::insertForwardDeclaration", LOG_ARG("text", fwddecl));
    if (!isHeader()) {
        spdlog::warn("CppDocument::insertForwardDeclaration: {} - is not a header file. ", fileName().toStdString());
        return false;
    }

    int spacePos = fwddecl.indexOf(' ');
    auto classOrStruct = fwddecl.leftRef(spacePos);
    if (fwddecl.isEmpty() || (classOrStruct != "class" && classOrStruct != "struct")) {
        spdlog::warn("CppDocument::insertForwardDeclaration: {} - should start with 'class ' or 'struct '. ",
                     fwddecl.toStdString());
        return false;
    }

    auto qualifierList = fwddecl.midRef(spacePos + 1).split("::");
    std::ranges::reverse(qualifierList);

    // Get the un-qualified declaration
    QString result = QString("%1 %2;").arg(classOrStruct).arg(qualifierList.first());
    qualifierList.pop_front();

    // Check if the declaration already exists
    QTextDocument *doc = textEdit()->document();
    QTextCursor cursor(doc);
    cursor = doc->find(result, cursor, QTextDocument::FindWholeWords);
    if (!cursor.isNull()) {
        spdlog::warn("CppDocument::insertForwardDeclaration: '{}' - already exists in file.", fwddecl.toStdString());
        return false;
    }

    for (const auto &qualifier : std::as_const(qualifierList))
        result = QString("namespace %1 {\n%2\n}").arg(qualifier, result);

    cursor = QTextCursor(doc);
    cursor.movePosition(QTextCursor::End, QTextCursor::MoveAnchor);
    cursor = doc->find(QRegularExpression(QStringLiteral(R"(^#include\s*)")), cursor, QTextDocument::FindBackward);
    if (!cursor.isNull()) {
        cursor.beginEditBlock();
        cursor.movePosition(QTextCursor::EndOfLine, QTextCursor::MoveAnchor);
        cursor.insertText("\n" + result + "\n");
        cursor.endEditBlock();
        return true;
    }

    return false;
}

/*!
 * \qmlmethod map<string, string> CppDocument::mfcExtractDDX()
 * Extract the DDX information from a MFC class.
 *
 * The DDX information gives the mapping between the ID and the member variables in the class.
 */
QVariantMap CppDocument::mfcExtractDDX(const QString &className)
{
    LOG("CppDocument::mfcExtractDDX", LOG_ARG("text", className));

    QVariantMap map;

    // TODO: use semantic information coming from LSP instead of regexp to find the method

    const QString source = text();
    const QRegularExpression searchFunctionExpression(QString(R"*(void\s*%1\s*::DoDataExchange\s*\()*").arg(className),
                                                      QRegularExpression::MultilineOption);
    QRegularExpressionMatch match = searchFunctionExpression.match(source);

    if (match.hasMatch()) {
        const int capturedStart = match.capturedStart(0);
        const int capturedEnd = match.capturedEnd(0);
        int bracketCount = 0;
        int positionEnd = -1;
        for (int i = capturedEnd; i < source.length(); ++i) {
            if (source.at(i) == QLatin1Char('{')) {
                bracketCount++;
            } else if (source.at(i) == QLatin1Char('}')) {
                bracketCount--;
                if (bracketCount == 0) {
                    positionEnd = i;
                    break;
                }
            }
        }

        if (positionEnd == -1)
            return {};

        const QString ddxText = source.mid(capturedStart, (positionEnd - capturedStart + 1));
        static const QRegularExpression doDataExchangeExpression(R"*(DDX_.*\(.*,\s*(.*)\s*,\s*(.*)\))*");
        QRegularExpressionMatchIterator userIteratorWidgetExpression = doDataExchangeExpression.globalMatch(ddxText);
        while (userIteratorWidgetExpression.hasNext()) {
            const QRegularExpressionMatch match = userIteratorWidgetExpression.next();
            map.insert(match.captured(1), match.captured(2));
        }
    }
    return map;
}

/*!
 * \qmlmethod int CppDocument::gotoBlockStart(int count)
 * Move the cursor to the start of the block it's in, and returns the new cursor position.
 * A block is definied by {} or () or [].
 * Do it `count` times.
 */
int CppDocument::gotoBlockStart(int count)
{
    LOG_AND_MERGE("CppDocument::gotoBlockStart", count);

    QTextCursor cursor = textEdit()->textCursor();
    while (count != 0) {
        cursor.setPosition(moveBlock(cursor.position(), QTextCursor::PreviousCharacter));
        --count;
    }
    textEdit()->setTextCursor(cursor);
    return cursor.position();
}

/*!
 * \qmlmethod int CppDocument::gotoBlockEnd(int count)
 * Move the cursor to the end of the block it's in, and returns the new cursor position.
 * A block is definied by {} or () or [].
 * Do it `count` times.
 */
int CppDocument::gotoBlockEnd(int count)
{
    LOG_AND_MERGE("CppDocument::gotoBlockEnd", count);

    QTextCursor cursor = textEdit()->textCursor();
    while (count != 0) {
        cursor.setPosition(moveBlock(cursor.position(), QTextCursor::NextCharacter));
        --count;
    }
    textEdit()->setTextCursor(cursor);
    return cursor.position();
}

/**
 * \brief Internal method to move to the start or end of a block
 * \param startPos current cursor position
 * \param direction the iteration
 * \return position of the start or end of the block
 */
int CppDocument::moveBlock(int startPos, QTextCursor::MoveOperation direction)
{
    Q_ASSERT(direction == QTextCursor::NextCharacter || direction == QTextCursor::PreviousCharacter);

    QTextDocument *doc = textEdit()->document();
    Q_ASSERT(doc);

    const int inc = direction == QTextCursor::NextCharacter ? 1 : -1;
    const int lastPos = direction == QTextCursor::NextCharacter ? textEdit()->document()->characterCount() - 1 : 0;
    if (startPos == lastPos)
        return startPos;
    int pos = startPos + inc;
    QChar currentChar = doc->characterAt(pos);

    // Set teh characters delimiter that increment or decrement the counter when iterating
    auto incCounterChar =
        direction == QTextCursor::NextCharacter ? QVector<QChar> {'(', '{', '['} : QVector<QChar> {')', '}', ']'};
    auto decCounterChar =
        direction == QTextCursor::PreviousCharacter ? QVector<QChar> {'(', '{', '['} : QVector<QChar> {')', '}', ']'};

    // If the character next is a sepcial one, go inside the block
    if (incCounterChar.contains(currentChar))
        pos += inc;

    // Iterate to find the other side of the block
    int counter = 0;
    pos += inc;
    while (pos != lastPos) {
        currentChar = doc->characterAt(pos);

        if (incCounterChar.contains(currentChar)) {
            counter++;

        } else if (decCounterChar.contains(currentChar)) {
            counter--;

            // When counter is negative, we have found the other side of the block
            if (counter < 0)
                return pos + std::max(inc, 0);
        }
        pos += inc;
    }
    return startPos;
}

} // namespace Core
