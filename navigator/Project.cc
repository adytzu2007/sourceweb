#include "Project.h"

#include <QTime>
#include <QDebug>

#include <QFileInfo>
#include <QtConcurrentRun>

#include "FileManager.h"
#include "File.h"
#include "Misc.h"
#include "Ref.h"
#include "../libindexdb/IndexDb.h"

namespace Nav {

// loc table
const int kLocColumns       = 4;
const int kLocColumnFile    = 0;
const int kLocColumnLine    = 1;
const int kLocColumnColumn  = 2;
const int kLocColumnSymbol  = 3;

// ref table
const int kRefColumns       = 5;
const int kRefColumnSymbol  = 0;
const int kRefColumnFile    = 1;
const int kRefColumnLine    = 2;
const int kRefColumnColumn  = 3;
const int kRefColumnKind    = 4;

Project *theProject;

Project::Project(const QString &path)
{
    m_index = new indexdb::Index(path.toStdString());
    m_symbolTable = m_index->stringTable("usr");
    m_pathTable = m_index->stringTable("path");
    m_kindTable = m_index->stringTable("kind");
    assert(m_symbolTable != NULL);
    assert(m_pathTable != NULL);
    assert(m_kindTable != NULL);

    m_sortedSymbolsInited =
            QtConcurrent::run(this, &Project::initSortedSymbols);

    // Query all the paths, then use that to initialize the FileManager.
    QList<QString> allPaths;
    indexdb::StringTable *pathTable = m_index->stringTable("path");
    for (uint32_t i = 0; i < pathTable->size(); ++i) {
        const char *path = pathTable->item(i);
        if (path[0] != '\0') {
            allPaths.append(path);
        }
    }
    m_fileManager = new FileManager(
                QFileInfo(path).absolutePath(),
                allPaths);
}

Project::~Project()
{
    m_sortedSymbolsInited.waitForFinished();
    delete m_fileManager;
    delete m_index;
}

QList<Ref> Project::queryReferencesOfSymbol(const QString &symbol)
{
    QList<Ref> result;

    indexdb::ID symbolID = m_symbolTable->id(symbol.toStdString().c_str());
    if (symbolID == indexdb::kInvalidID)
        return result;

    indexdb::Row rowLookup(1);
    assert(kRefColumnSymbol == 0);
    rowLookup[kRefColumnSymbol] = symbolID;

    indexdb::Row rowItem(kRefColumns);
    indexdb::TableIterator itEnd = m_index->table("ref")->end();
    indexdb::TableIterator it = m_index->table("ref")->lowerBound(rowLookup);
    for (; it != itEnd; ++it) {
        it.value(rowItem);
        if (rowLookup[kRefColumnSymbol] != rowItem[kRefColumnSymbol])
            break;

        indexdb::ID fileID = rowItem[kRefColumnFile];
        int line = rowItem[kRefColumnLine];
        int column = rowItem[kRefColumnColumn];
        indexdb::ID kindID = rowItem[kRefColumnKind];

        result << Ref(*this,
                      symbolID,
                      fileID,
                      line,
                      column,
                      kindID);
    }

    return result;
}

QStringList Project::querySymbolsAtLocation(File *file, int line, int column)
{
    QStringList result;

    indexdb::ID fileID = m_index->stringTable("path")->id(file->path().toStdString().c_str());
    if (fileID == indexdb::kInvalidID)
        return result;

    indexdb::Row rowLookup(3);
    assert(kLocColumnFile < 3);
    assert(kLocColumnLine < 3);
    assert(kLocColumnColumn < 3);
    rowLookup[kLocColumnFile] = fileID;
    rowLookup[kLocColumnLine] = line;
    rowLookup[kLocColumnColumn] = column;

    indexdb::TableIterator itEnd = m_index->table("loc")->end();
    indexdb::TableIterator it = m_index->table("loc")->lowerBound(rowLookup);
    for (; it != itEnd; ++it) {
        indexdb::Row rowItem(kLocColumns);
        it.value(rowItem);
        if (rowLookup[kLocColumnFile] != rowItem[kLocColumnFile] ||
                rowLookup[kLocColumnLine] != rowItem[kLocColumnLine] ||
                rowLookup[kLocColumnColumn] != rowItem[kLocColumnColumn])
            break;
        result << m_symbolTable->item(rowItem[kLocColumnSymbol]);
    }

    return result;
}

// Sorting the symbols is slow (e.g. 800ms for LLVM+Clang), so do it just once,
// when the navigator starts up, and start doing it in the background when the
// application starts.
//
// XXX: Consider sorting the symbols in the index instead.
// XXX: It's probably possible to write a parallel_sort that would be a
// drop-in replacement for std::sort.
void Project::initSortedSymbols()
{
    indexdb::StringTable *symbolTable = m_index->stringTable("usr");
    m_sortedSymbols.resize(symbolTable->size());
    for (uint32_t i = 0, iEnd = symbolTable->size(); i < iEnd; ++i) {
        const char *symbol = symbolTable->item(i);
        m_sortedSymbols[i] = symbol;
    }
    struct ConstCharCompare {
        bool operator()(const char *x, const char *y) {
            return strcmp(x, y) < 0;
        }
    };
    std::sort(m_sortedSymbols.begin(),
              m_sortedSymbols.end(),
              ConstCharCompare());
}

// XXX: This function could return a reference rather than copying the symbol
// list.
void Project::queryAllSymbolsSorted(std::vector<const char*> &output)
{
    m_sortedSymbolsInited.waitForFinished();
    output = m_sortedSymbols;
}

QList<File*> Project::queryAllFiles()
{
    QList<File*> result;
    indexdb::StringTable *pathTable = m_index->stringTable("path");
    for (uint32_t i = 0; i < pathTable->size(); ++i) {
        const char *path = pathTable->item(i);
        if (path[0] != '\0') {
            File *file = &m_fileManager->file(path);
            result << file;
        }
    }
    return result;
}

// Finds the only definition ref (or declaration ref) of the symbol.  If there
// isn't a single such ref, return NULL.
Ref Project::findSingleDefinitionOfSymbol(const QString &symbol)
{
    int declCount = 0;
    int defnCount = 0;
    Ref decl;
    Ref defn;
    QList<Ref> refs = theProject->queryReferencesOfSymbol(symbol);
    for (const Ref &ref : refs) {
        if (declCount < 2 && ref.kind() == "Declaration") {
            declCount++;
            decl = ref;
        }
        if (defnCount < 2 && ref.kind() == "Definition") {
            defnCount++;
            defn = ref;
        }
    }
    if (defnCount == 1) {
        return defn;
    } else if (defnCount == 0 && declCount == 1) {
        return decl;
    } else {
        return Ref();
    }
}

QList<Ref> Project::queryAllSymbolDefinitions()
{
    QList<Ref> result;

    indexdb::ID defnKindID = m_kindTable->id("Definition");
    indexdb::TableIterator itEnd = m_index->table("ref")->end();
    indexdb::TableIterator it = m_index->table("ref")->begin();

    for (; it != itEnd; ++it) {
        indexdb::Row rowItem(kRefColumns);
        it.value(rowItem);
        if (rowItem[kRefColumnKind] != defnKindID)
            continue;
        indexdb::ID symbolID = rowItem[kRefColumnSymbol];
        indexdb::ID fileID = rowItem[kRefColumnFile];
        int line = rowItem[kRefColumnLine];
        int column = rowItem[kRefColumnColumn];
        indexdb::ID kindID = rowItem[kRefColumnKind];

        result << Ref(*this, symbolID, fileID, line, column, kindID);
    }

    return result;
}

} // namespace Nav
