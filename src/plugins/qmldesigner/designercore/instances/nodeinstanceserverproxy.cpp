#include "nodeinstanceserverproxy.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QCoreApplication>
#include <QUuid>

#include "propertyabstractcontainer.h"
#include "propertyvaluecontainer.h"
#include "propertybindingcontainer.h"
#include "instancecontainer.h"
#include "createinstancescommand.h"
#include "createscenecommand.h"
#include "changevaluescommand.h"
#include "changebindingscommand.h"
#include "changefileurlcommand.h"
#include "removeinstancescommand.h"
#include "clearscenecommand.h"
#include "removepropertiescommand.h"
#include "reparentinstancescommand.h"
#include "changeidscommand.h"
#include "changestatecommand.h"
#include "addimportcommand.h"
#include "completecomponentcommand.h"

#include "informationchangedcommand.h"
#include "pixmapchangedcommand.h"
#include "valueschangedcommand.h"
#include "childrenchangedcommand.h"
#include "imagecontainer.h"
#include "statepreviewimagechangedcommand.h"
#include "componentcompletedcommand.h"

#include "nodeinstanceview.h"
#include "nodeinstanceclientproxy.h"

namespace QmlDesigner {

NodeInstanceServerProxy::NodeInstanceServerProxy(NodeInstanceView *nodeInstanceView)
    : NodeInstanceServerInterface(nodeInstanceView),
      m_localServer(new QLocalServer(this)),
      m_nodeInstanceView(nodeInstanceView),
      m_slowBlockSize(0),
      m_fastBlockSize(0)
{
   QString socketToken(QUuid::createUuid().toString());

   m_localServer->listen(socketToken);
   m_localServer->setMaxPendingConnections(2);

   m_qmlPuppetProcess = new QProcess(QCoreApplication::instance());
   connect(m_qmlPuppetProcess.data(), SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(processFinished(int,QProcess::ExitStatus)));
   m_qmlPuppetProcess->setProcessChannelMode(QProcess::ForwardedChannels);
   m_qmlPuppetProcess->start(QString("%1/%2").arg(QCoreApplication::applicationDirPath()).arg("qmlpuppet"), QStringList() << socketToken);
   connect(QCoreApplication::instance(), SIGNAL(aboutToQuit()), this, SLOT(deleteLater()));
   m_qmlPuppetProcess->waitForStarted();
   Q_ASSERT(m_qmlPuppetProcess->state() == QProcess::Running);

   if (!m_localServer->hasPendingConnections())
       m_localServer->waitForNewConnection(-1);

   m_slowSocket = m_localServer->nextPendingConnection();
   Q_ASSERT(m_slowSocket);
   connect(m_slowSocket.data(), SIGNAL(readyRead()), this, SLOT(readSlowDataStream()));

   if (!m_localServer->hasPendingConnections())
       m_localServer->waitForNewConnection(-1);

   m_fastSocket = m_localServer->nextPendingConnection();
   Q_ASSERT(m_fastSocket);
   connect(m_fastSocket.data(), SIGNAL(readyRead()), this, SLOT(readFastDataStream()));
   m_localServer->close();
}

NodeInstanceServerProxy::~NodeInstanceServerProxy()
{
    if (m_qmlPuppetProcess) {
        m_qmlPuppetProcess->blockSignals(true);
        m_qmlPuppetProcess->terminate();
    }
}

void NodeInstanceServerProxy::dispatchCommand(const QVariant &command)
{
    static const int informationChangedCommandType = QMetaType::type("InformationChangedCommand");
    static const int valuesChangedCommandType = QMetaType::type("ValuesChangedCommand");
    static const int pixmapChangedCommandType = QMetaType::type("PixmapChangedCommand");
    static const int childrenChangedCommandType = QMetaType::type("ChildrenChangedCommand");
    static const int statePreviewImageChangedCommandType = QMetaType::type("StatePreviewImageChangedCommand");
    static const int componentCompletedCommandType = QMetaType::type("ComponentCompletedCommand");

    if (command.userType() ==  informationChangedCommandType)
        nodeInstanceClient()->informationChanged(command.value<InformationChangedCommand>());
    else if (command.userType() ==  valuesChangedCommandType)
        nodeInstanceClient()->valuesChanged(command.value<ValuesChangedCommand>());
    else if (command.userType() ==  pixmapChangedCommandType)
        nodeInstanceClient()->pixmapChanged(command.value<PixmapChangedCommand>());
    else if (command.userType() == childrenChangedCommandType)
        nodeInstanceClient()->childrenChanged(command.value<ChildrenChangedCommand>());
    else if (command.userType() == statePreviewImageChangedCommandType)
        nodeInstanceClient()->statePreviewImagesChanged(command.value<StatePreviewImageChangedCommand>());
    else if (command.userType() == componentCompletedCommandType)
        nodeInstanceClient()->componentCompleted(command.value<ComponentCompletedCommand>());
    else
        Q_ASSERT(false);
}

NodeInstanceClientInterface *NodeInstanceServerProxy::nodeInstanceClient() const
{
    return m_nodeInstanceView.data();
}

void NodeInstanceServerProxy::setBlockUpdates(bool block)
{
    m_slowSocket->blockSignals(block);
}

void NodeInstanceServerProxy::writeCommand(const QVariant &command)
{
    Q_ASSERT(m_fastSocket.data());

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out << quint32(0);
    out << command;
    out.device()->seek(0);
    out << quint32(block.size() - sizeof(quint32));

    m_fastSocket->write(block);
}

void NodeInstanceServerProxy::processFinished(int /*exitCode*/, QProcess::ExitStatus /* exitStatus */)
{
    m_slowSocket->close();
    emit processCrashed();
}


void NodeInstanceServerProxy::readFastDataStream()
{
    QList<QVariant> commandList;

    while (!m_fastSocket->atEnd()) {
        if (m_fastSocket->bytesAvailable() < int(sizeof(quint32)))
            break;

        QDataStream in(m_fastSocket.data());

        if (m_fastBlockSize == 0) {
            in >> m_fastBlockSize;
        }

        if (m_fastSocket->bytesAvailable() < m_fastBlockSize)
            break;

        QVariant command;
        in >> command;
        m_fastBlockSize = 0;

        Q_ASSERT(in.status() == QDataStream::Ok);

        commandList.append(command);
    }

    foreach (const QVariant &command, commandList) {
        dispatchCommand(command);
    }
}

void NodeInstanceServerProxy::readSlowDataStream()
{
    QList<QVariant> commandList;

    while (!m_slowSocket->atEnd()) {
        if (m_slowSocket->bytesAvailable() < int(sizeof(quint32)))
            break;

        QDataStream in(m_slowSocket.data());

        if (m_slowBlockSize == 0) {
            in >> m_slowBlockSize;
        }

        if (m_slowSocket->bytesAvailable() < m_slowBlockSize)
            break;

        QVariant command;
        in >> command;
        m_slowBlockSize = 0;

        Q_ASSERT(in.status() == QDataStream::Ok);

        commandList.append(command);
    }

    foreach (const QVariant &command, commandList) {
        dispatchCommand(command);
    }
}

void NodeInstanceServerProxy::createInstances(const CreateInstancesCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::changeFileUrl(const ChangeFileUrlCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::createScene(const CreateSceneCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::clearScene(const ClearSceneCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::removeInstances(const RemoveInstancesCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::removeProperties(const RemovePropertiesCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::changePropertyBindings(const ChangeBindingsCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::changePropertyValues(const ChangeValuesCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::reparentInstances(const ReparentInstancesCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::changeIds(const ChangeIdsCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::changeState(const ChangeStateCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::addImport(const AddImportCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}

void NodeInstanceServerProxy::completeComponent(const CompleteComponentCommand &command)
{
    writeCommand(QVariant::fromValue(command));
}
} // namespace QmlDesigner
