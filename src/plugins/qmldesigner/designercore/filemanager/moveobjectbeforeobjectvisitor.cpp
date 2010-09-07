/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "moveobjectbeforeobjectvisitor.h"
#include "textmodifier.h"

#include <qmljs/parser/qmljsast_p.h>
#include <qmljs/parser/qmljsengine_p.h>

#include <QtCore/QDebug>

using namespace QmlJS;
using namespace QmlJS::AST;
using namespace QmlDesigner::Internal;
using namespace QmlDesigner;

MoveObjectBeforeObjectVisitor::MoveObjectBeforeObjectVisitor(TextModifier &modifier,
                                                             quint32 movingObjectLocation,
                                                             bool inDefaultProperty):
    QMLRewriter(modifier),
    movingObjectLocation(movingObjectLocation),
    inDefaultProperty(inDefaultProperty),
    toEnd(true),
    beforeObjectLocation(0)
{}

MoveObjectBeforeObjectVisitor::MoveObjectBeforeObjectVisitor(TextModifier &modifier,
                                                             quint32 movingObjectLocation,
                                                             quint32 beforeObjectLocation,
                                                             bool inDefaultProperty):
    QMLRewriter(modifier),
    movingObjectLocation(movingObjectLocation),
    inDefaultProperty(inDefaultProperty),
    toEnd(false),
    beforeObjectLocation(beforeObjectLocation)
{}

bool MoveObjectBeforeObjectVisitor::operator ()(QmlJS::AST::UiProgram *ast)
{
    movingObject = 0;
    beforeObject = 0;
    movingObjectParents.clear();

    QMLRewriter::operator ()(ast);

    if (foundEverything()) {
        doMove();
    }

    return didRewriting();
}

bool MoveObjectBeforeObjectVisitor::preVisit(Node *ast)
{ if (ast) parents.push(ast); return true; }

void MoveObjectBeforeObjectVisitor::postVisit(Node *ast)
{ if (ast) parents.pop(); }

bool MoveObjectBeforeObjectVisitor::visit(UiObjectDefinition *ast)
{
    if (foundEverything())
        return false;

    const quint32 start = ast->firstSourceLocation().offset;
    if (start == movingObjectLocation) {
        movingObject = ast;
        movingObjectParents = parents;
        movingObjectParents.pop();
    } else if (!toEnd && start == beforeObjectLocation) {
        beforeObject = ast;
    }

    if (movingObjectLocation < start)
        return false;
    else if (!toEnd && beforeObjectLocation < start)
        return false;
    else if (foundEverything())
        return false;
    else
        return true;
}

void MoveObjectBeforeObjectVisitor::doMove()
{
    Q_ASSERT(movingObject);
    Q_ASSERT(!movingObjectParents.isEmpty());

    TextModifier::MoveInfo moveInfo;
    Node *parent = movingObjectParent();
    UiArrayMemberList *arrayMember = 0, *otherArrayMember;
    QString separator;

    if (!inDefaultProperty) {
        UiArrayBinding *initializer = cast<UiArrayBinding*>(parent);
        Q_ASSERT(initializer);

        otherArrayMember = 0;
        for (UiArrayMemberList *cur = initializer->members; cur; cur = cur->next) {
            if (cur->member == movingObject) {
                arrayMember = cur;
                if (cur->next)
                    otherArrayMember = cur->next;
                break;
            }
            otherArrayMember = cur;
        }
        Q_ASSERT(arrayMember && otherArrayMember);
        separator = QLatin1String(",");
    }

    moveInfo.objectStart = movingObject->firstSourceLocation().offset;
    moveInfo.objectEnd = movingObject->lastSourceLocation().end();

    int start = moveInfo.objectStart;
    int end = moveInfo.objectEnd;
    if (!inDefaultProperty) {
        if (arrayMember->commaToken.isValid()) {
            start = arrayMember->commaToken.begin();
        }
        else {
            end = otherArrayMember->commaToken.end();
        }
    }

    includeSurroundingWhitespace(start, end);
    moveInfo.leadingCharsToRemove = moveInfo.objectStart - start;
    moveInfo.trailingCharsToRemove = end - moveInfo.objectEnd;

    if (beforeObject) {
        moveInfo.destination = beforeObject->firstSourceLocation().offset;
        int dummy = -1;
        includeSurroundingWhitespace(moveInfo.destination, dummy);

        moveInfo.prefixToInsert = QString(moveInfo.leadingCharsToRemove, QLatin1Char(' '));
        moveInfo.suffixToInsert = separator + QLatin1String("\n\n");
    } else {
        const SourceLocation insertionPoint = lastParentLocation();
        Q_ASSERT(insertionPoint.isValid());
        moveInfo.destination = insertionPoint.offset;
        int dummy = -1;
        includeSurroundingWhitespace(moveInfo.destination, dummy);

        moveInfo.prefixToInsert = separator + QString(moveInfo.leadingCharsToRemove, QLatin1Char(' '));
        moveInfo.suffixToInsert = QLatin1String("\n");
    }

    move(moveInfo);
    setDidRewriting(true);
}

Node *MoveObjectBeforeObjectVisitor::movingObjectParent() const
{
    if (movingObjectParents.size() > 1)
        return movingObjectParents.at(movingObjectParents.size() - 2);
    else
        return 0;
}

SourceLocation MoveObjectBeforeObjectVisitor::lastParentLocation() const
{
    dump(movingObjectParents);

    Node *parent = movingObjectParent();
    if (UiObjectInitializer *initializer = cast<UiObjectInitializer*>(parent))
        return initializer->rbraceToken;
    else if (UiArrayBinding *initializer = cast<UiArrayBinding*>(parent))
        return initializer->rbracketToken;
    else
        return SourceLocation();
}
