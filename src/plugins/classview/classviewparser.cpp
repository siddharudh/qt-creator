/****************************************************************************
**
** Copyright (C) 2016 Denis Mingulov
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "classviewparser.h"
#include "classviewconstants.h"
#include "classviewutils.h"

// cplusplus shared library. the same folder (cplusplus)
#include <cplusplus/Symbol.h>
#include <cplusplus/Symbols.h>
#include <cplusplus/Scope.h>
#include <cplusplus/Name.h>

// other
#include <cpptools/cppmodelmanager.h>
#include <cplusplus/Overview.h>
#include <cplusplus/Icons.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/session.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectnodes.h>

#include <utils/algorithm.h>
#include <utils/qtcassert.h>

#include <QStandardItem>
#include <QDebug>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QReadWriteLock>
#include <QReadLocker>
#include <QWriteLocker>
#include <QElapsedTimer>

enum { debug = false };

using namespace ProjectExplorer;
using namespace Utils;

namespace ClassView {
namespace Internal {

// ----------------------------- ParserPrivate ---------------------------------

/*!
   \class ParserPrivate
   \brief The ParserPrivate class defines private class data for the Parser
   class.
   \sa Parser
 */

/*!
   \class Parser
   \brief The Parser class parses C++ information. Multithreading is supported.
*/

/*!
    \fn void Parser::treeDataUpdate(QSharedPointer<QStandardItem> result)

    Emits a signal about a tree data update.
*/

class ParserPrivate
{
public:
    // Keep timer as a child of Parser in order to move it together with its parent
    // into another thread.
    ParserPrivate(QObject *parent) : timer(parent) {}

    //! Get document from documentList
    CPlusPlus::Document::Ptr document(const QString &fileName) const;

    CPlusPlus::Overview overview;

    QTimer timer;

    struct DocumentCache {
        unsigned treeRevision = 0;
        ParserTreeItem::Ptr tree;
        CPlusPlus::Document::Ptr document;
    };
    struct ProjectCache {
        unsigned treeRevision = 0;
        ParserTreeItem::Ptr tree;
        QStringList fileList;
    };

    // Project file path to its cached data
    QHash<QString, DocumentCache> m_documentCache;
    // Project file path to its cached data
    QHash<QString, ProjectCache> m_projectCache;

    // other
    //! List for files which has to be parsed
    QSet<QString> fileList;

    //! Root item read write lock
    QReadWriteLock rootItemLocker;

    //! Parsed root item
    ParserTreeItem::ConstPtr rootItem;

    //! Flat mode
    bool flatMode = false;
};

CPlusPlus::Document::Ptr ParserPrivate::document(const QString &fileName) const
{
    return m_documentCache.value(fileName).document;
}

// ----------------------------- Parser ---------------------------------

/*!
    Constructs the parser object.
*/

Parser::Parser(QObject *parent)
    : QObject(parent),
    d(new ParserPrivate(this))
{
    d->timer.setSingleShot(true);

    // timer for emitting changes
    connect(&d->timer, &QTimer::timeout, this, &Parser::requestCurrentState);
}

/*!
    Destructs the parser object.
*/

Parser::~Parser()
{
    delete d;
}

/*!
    Checks \a item for lazy data population of a QStandardItemModel.
*/

bool Parser::canFetchMore(QStandardItem *item, bool skipRoot) const
{
    ParserTreeItem::ConstPtr ptr = findItemByRoot(item, skipRoot);
    if (ptr.isNull())
        return false;
    return ptr->canFetchMore(item);
}

/*!
    Checks \a item for lazy data population of a QStandardItemModel.
    \a skipRoot skips the root item.
*/

void Parser::fetchMore(QStandardItem *item, bool skipRoot) const
{
    ParserTreeItem::ConstPtr ptr = findItemByRoot(item, skipRoot);
    if (ptr.isNull())
        return;
    ptr->fetchMore(item);
}

bool Parser::hasChildren(QStandardItem *item) const
{
    ParserTreeItem::ConstPtr ptr = findItemByRoot(item);
    if (ptr.isNull())
        return false;
    return ptr->childCount() != 0;
}

/*!
    Switches to flat mode (without subprojects) if \a flat returns \c true.
*/

void Parser::setFlatMode(bool flatMode)
{
    if (flatMode == d->flatMode)
        return;

    // change internal
    d->flatMode = flatMode;

    // regenerate and resend current tree
    requestCurrentState();
}

/*!
    Returns the internal tree item for \a item. \a skipRoot skips the root
    item.
*/

ParserTreeItem::ConstPtr Parser::findItemByRoot(const QStandardItem *item, bool skipRoot) const
{
    if (!item)
        return ParserTreeItem::ConstPtr();

    // go item by item to the root
    QList<const QStandardItem *> uiList;
    const QStandardItem *cur = item;
    while (cur) {
        uiList.append(cur);
        cur = cur->parent();
    }

    if (skipRoot && uiList.count() > 0)
        uiList.removeLast();

    ParserTreeItem::ConstPtr internal;
    {
        QReadLocker locker(&d->rootItemLocker);
        internal = d->rootItem;
    }

    while (uiList.count() > 0) {
        cur = uiList.last();
        uiList.removeLast();
        const SymbolInformation &inf = Internal::symbolInformationFromItem(cur);
        internal = internal->child(inf);
        if (internal.isNull())
            break;
    }

    return internal;
}

/*!
    Parses the class and produces a new tree.

    \sa addProject
*/

ParserTreeItem::ConstPtr Parser::parse()
{
    QElapsedTimer time;
    if (debug)
        time.start();

    ParserTreeItem::Ptr rootItem(new ParserTreeItem());

    // check all projects
    for (const Project *prj : SessionManager::projects()) {
        ParserTreeItem::Ptr item;
        QString prjName(prj->displayName());
        QString prjType = prj->projectFilePath().toString();
        SymbolInformation inf(prjName, prjType);
        item = ParserTreeItem::Ptr(new ParserTreeItem());

        addFlatTree(item, prj);

        item->setIcon(prj->containerNode()->icon());

        rootItem->appendChild(item, inf);
    }

    if (debug)
        qDebug() << "Class View:" << QDateTime::currentDateTime().toString()
            << "Parsed in " << time.elapsed() << "msecs.";

    return rootItem;
}

/*!
    Parses the project with the \a projectId and adds the documents
    from the \a fileList to the tree item \a item.
*/

void Parser::addProject(const ParserTreeItem::Ptr &item, const QStringList &fileList,
                        const QString &projectId)
{
    // recalculate cache tree if needed
    ParserTreeItem::Ptr prj(getCachedOrParseProjectTree(fileList, projectId));
    if (item.isNull())
        return;

    // if there is an item - copy project tree to that item
    item->copy(prj);
}

/*!
    Parses \a symbol and adds the results to \a item (as a parent).
*/

void Parser::addSymbol(const ParserTreeItem::Ptr &item, const CPlusPlus::Symbol *symbol)
{
    if (item.isNull() || !symbol)
        return;

    // easy solution - lets add any scoped symbol and
    // any symbol which does not contain :: in the name

    //! \todo collect statistics and reorder to optimize
    if (symbol->isForwardClassDeclaration()
        || symbol->isExtern()
        || symbol->isFriend()
        || symbol->isGenerated()
        || symbol->isUsingNamespaceDirective()
        || symbol->isUsingDeclaration()
        )
        return;

    const CPlusPlus::Name *symbolName = symbol->name();
    if (symbolName && symbolName->isQualifiedNameId())
        return;

    QString name = d->overview.prettyName(symbolName).trimmed();
    QString type = d->overview.prettyType(symbol->type()).trimmed();
    int iconType = CPlusPlus::Icons::iconTypeForSymbol(symbol);

    SymbolInformation information(name, type, iconType);

    ParserTreeItem::Ptr itemAdd;

    // If next line will be removed, 5% speed up for the initial parsing.
    // But there might be a problem for some files ???
    // Better to improve qHash timing
    itemAdd = item->child(information);

    if (itemAdd.isNull())
        itemAdd = ParserTreeItem::Ptr(new ParserTreeItem());

    // locations have 1-based column in Symbol, use the same here.
    SymbolLocation location(QString::fromUtf8(symbol->fileName() , symbol->fileNameLength()),
                            symbol->line(), symbol->column());
    itemAdd->addSymbolLocation(location);

    // prevent showing a content of the functions
    if (!symbol->isFunction()) {
        if (const CPlusPlus::Scope *scope = symbol->asScope()) {
            CPlusPlus::Scope::iterator cur = scope->memberBegin();
            CPlusPlus::Scope::iterator last = scope->memberEnd();
            while (cur != last) {
                const CPlusPlus::Symbol *curSymbol = *cur;
                ++cur;
                if (!curSymbol)
                    continue;

                addSymbol(itemAdd, curSymbol);
            }
        }
    }

    // if item is empty and has not to be added
    if (!(symbol->isNamespace() && itemAdd->childCount() == 0))
        item->appendChild(itemAdd, information);
}

/*!
    Parses the project with the \a projectId and adds the documents from the
    \a fileList to the project. Updates the internal cached tree for this
    project.
*/

ParserTreeItem::Ptr Parser::getParseProjectTree(const QStringList &fileList,
                                                const QString &projectId)
{
    //! \todo Way to optimize - for documentUpdate - use old cached project and subtract
    //! changed files only (old edition), and add curent editions
    ParserTreeItem::Ptr item(new ParserTreeItem());
    unsigned revision = 0;
    foreach (const QString &file, fileList) {
        const CPlusPlus::Document::Ptr &doc = d->document(file);
        if (doc.isNull())
            continue;

        revision += doc->revision();

        ParserTreeItem::ConstPtr list = getCachedOrParseDocumentTree(doc);
        if (list.isNull())
            continue;

        // add list to out document
        item->add(list);
    }

    // update the cache
    if (!projectId.isEmpty()) {
        ParserPrivate::ProjectCache &projectCache = d->m_projectCache[projectId];
        projectCache.tree = item;
        projectCache.treeRevision = revision;
    }
    return item;
}

/*!
    Gets the project with \a projectId from the cache if it is valid or parses
    the project and adds the documents from the \a fileList to the project.
    Updates the internal cached tree for this project.
*/

ParserTreeItem::Ptr Parser::getCachedOrParseProjectTree(const QStringList &fileList,
                                                const QString &projectId)
{
    const auto it = d->m_projectCache.constFind(projectId);
    if (it != d->m_projectCache.constEnd() && !it.value().tree.isNull()) {
        // calculate project's revision
        unsigned revision = 0;
        for (const QString &file : fileList) {
            const CPlusPlus::Document::Ptr &doc = d->document(file);
            if (doc.isNull())
                continue;
            revision += doc->revision();
        }

        // if even revision is the same, return cached project
        if (revision == it.value().treeRevision)
            return it.value().tree;
    }

    return getParseProjectTree(fileList, projectId);
}

/*!
    Parses the document \a doc if it is in the project files and adds a tree to
    the internal storage. Updates the internal cached tree for this document.

    \sa parseDocument
*/

ParserTreeItem::ConstPtr Parser::getParseDocumentTree(const CPlusPlus::Document::Ptr &doc)
{
    if (doc.isNull())
        return ParserTreeItem::ConstPtr();

    const QString &fileName = doc->fileName();
    if (!d->fileList.contains(fileName))
        return ParserTreeItem::ConstPtr();

    ParserTreeItem::Ptr itemPtr(new ParserTreeItem());

    const unsigned total = doc->globalSymbolCount();
    for (unsigned i = 0; i < total; ++i)
        addSymbol(itemPtr, doc->globalSymbolAt(i));

    d->m_documentCache.insert(fileName, { doc->revision(), itemPtr, doc } );
    return itemPtr;
}

/*!
    Gets the document \a doc from the cache or parses it if it is in the project
    files and adds a tree to the internal storage.

    \sa parseDocument
*/

ParserTreeItem::ConstPtr Parser::getCachedOrParseDocumentTree(const CPlusPlus::Document::Ptr &doc)
{
    if (doc.isNull())
        return ParserTreeItem::ConstPtr();

    const QString &fileName = doc->fileName();
    const auto it = d->m_documentCache.constFind(fileName);
    if (it != d->m_documentCache.constEnd() && !it.value().tree.isNull()
            && it.value().treeRevision == doc->revision()) {
        return it.value().tree;
    }
    return getParseDocumentTree(doc);
}

/*!
    Parses the document \a doc if it is in the project files and adds a tree to
    the internal storage.
*/

void Parser::parseDocument(const CPlusPlus::Document::Ptr &doc)
{
    if (doc.isNull())
        return;

    const QString &name = doc->fileName();

    // if it is external file (not in any of our projects)
    if (!d->fileList.contains(name))
        return;

    getParseDocumentTree(doc);

    if (!d->timer.isActive())
        d->timer.start(400); //! Delay in msecs before an update
    return;
}

/*!
    Specifies the files that must be allowed for the parsing as a \a fileList.
    Files outside of this list will not be in any tree.
*/

void Parser::setFileList(const QStringList &fileList)
{
    d->fileList = Utils::toSet(fileList);
}

/*!
    Removes the files defined in the \a fileList from the parsing.
*/

void Parser::removeFiles(const QStringList &fileList)
{
    if (fileList.isEmpty())
        return;

    for (const QString &name : fileList) {
        d->fileList.remove(name);
        d->m_documentCache.remove(name);
        d->m_projectCache.remove(name);
        for (auto it = d->m_projectCache.begin(); it != d->m_projectCache.end(); ++it)
            it.value().fileList.removeOne(name);
    }
}

/*!
    Fully resets the internal state of the code parser to \a snapshot.
*/
void Parser::resetData(const CPlusPlus::Snapshot &snapshot)
{
    d->m_projectCache.clear();
    d->m_documentCache.clear();
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it)
        d->m_documentCache[it.key().toString()].document = it.value();

    // recalculate file list
    FilePaths fileList;

    // check all projects
    for (const Project *prj : SessionManager::projects())
        fileList += prj->files(Project::SourceFiles);
    setFileList(Utils::transform(fileList, &FilePath::toString));

    requestCurrentState();
}

/*!
    Fully resets the internal state of the code parser to the current state.

    \sa resetData
*/

void Parser::resetDataToCurrentState()
{
    // get latest data
    resetData(CppTools::CppModelManager::instance()->snapshot());
}

/*!
    Requests to emit a signal with the current tree state.
*/

void Parser::requestCurrentState()
{
    d->timer.stop();

    const ParserTreeItem::ConstPtr newRoot = parse();
    {
        QWriteLocker locker(&d->rootItemLocker);
        d->rootItem = newRoot;
    }

    QSharedPointer<QStandardItem> std(new QStandardItem());
    d->rootItem->convertTo(std.data());

    emit treeDataUpdate(std);
}

QStringList Parser::getAllFiles(const Project *project)
{
    if (!project)
        return {};

    const QString projectPath = project->projectFilePath().toString();
    const auto it = d->m_projectCache.constFind(projectPath);
    if (it != d->m_projectCache.constEnd())
        return it.value().fileList;

    const QStringList fileList = Utils::transform(project->files(Project::SourceFiles),
                                                  &FilePath::toString);
    d->m_projectCache[projectPath].fileList = fileList;
    return fileList;
}

void Parser::addFlatTree(const ParserTreeItem::Ptr &item, const Project *project)
{
    if (!project)
        return;

    QStringList fileList = getAllFiles(project);

    if (fileList.count() > 0)
        addProject(item, fileList, project->projectFilePath().toString());
}

} // namespace Internal
} // namespace ClassView
