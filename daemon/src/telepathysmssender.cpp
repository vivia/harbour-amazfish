#include "telepathysmssender.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDebug>
#include <QVariantList>

// Steps of one send; each slot consumes a watcher reply and issues the next call.
// The recipe was verified live on SFOS 5 against ring's tel connections.
// Ring deviates from the Telepathy spec on purpose and its introspection
// advertises methods that reject calls (RequestHandles, CreateChannel and
// EnsureChannel are all vacant). Only these work:
//   Connection.Interface.Contacts.GetContactByID(s identifier, as types) -> (u handle, a{sv})
//   Connection.ListChannels() -> a(osuu) with channel type, handle type, handle
//   Connection.RequestChannel(s type, u handleType, u handle, b suppress) -> o
//   Channel.Type.Text.Send takes a message type (1), not the spec timestamp.
// Do not "fix" these to the spec.

namespace {
const QString TEXT_TYPE = QStringLiteral("org.freedesktop.Telepathy.Channel.Type.Text");
const QString CONNECTION_IFACE = QStringLiteral("org.freedesktop.Telepathy.Connection");
const QString CONTACTS_IFACE = QStringLiteral("org.freedesktop.Telepathy.Connection.Interface.Contacts");
const QString DESTROYABLE_IFACE = QStringLiteral("org.freedesktop.Telepathy.Channel.Interface.Destroyable");
const quint32 HT_CONTACT = 1;
}

QSet<TelepathySmsSender*> TelepathySmsSender::s_senders;

QPointer<TelepathySmsSender> TelepathySmsSender::send(const QString& number, const QString& text, QObject* target)
{
    TelepathySmsSender* sender = new TelepathySmsSender(number, text, target);
    s_senders.insert(sender);

    // Find ring's connection bus names on the session bus.
    QDBusPendingCall call = QDBusConnection::sessionBus().interface()->asyncCall("ListNames");
    QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher(call, sender);
    connect(watcher, &QDBusPendingCallWatcher::finished, sender, &TelepathySmsSender::findConnection);
    return sender;
}

TelepathySmsSender::TelepathySmsSender(const QString& number, const QString& text, QObject* target)
    : QObject(target)
    , m_number(number)
    , m_text(text)
{
}

void TelepathySmsSender::failLocal(const QString& reason)
{
    qWarning() << "telepathysmssender:" << reason;
    emit failureSignal(m_number, reason);
    finish();
}

void TelepathySmsSender::failWatcher(const QString& stage, QDBusPendingCallWatcher* watcher)
{
    qWarning() << "telepathysmssender:" << stage << "failed:"
               << watcher->error().name() << watcher->error().message();
    emit failureSignal(m_number, watcher->error().message());
    watcher->deleteLater();
    finish();
}

void TelepathySmsSender::finish()
{
    s_senders.remove(this);
    deleteLater();
}

void TelepathySmsSender::findConnection(QDBusPendingCallWatcher* watcher)
{
    if (watcher->isError()) {
        failWatcher("ListNames", watcher);
        return;
    }
    QDBusReply<QStringList> reply = *watcher;
    watcher->deleteLater();

    // Ring registers one bus name per connection, for example
    // org.freedesktop.Telepathy.Connection.ring.tel._0x3bddab20. A phone can
    // hold several tel connections at a time and they do not all behave the
    // same, so keep the candidates and try each until one resolves the number.
    const QString prefix = QStringLiteral("org.freedesktop.Telepathy.Connection.ring.");
    for (const QString& name : reply.value()) {
        if (name.startsWith(prefix) && name.contains(QStringLiteral(".tel."))) {
            m_candidates << name;
            qInfo() << "telepathysmssender: ring connection candidate" << name;
        }
    }

    if (m_candidates.isEmpty()) {
        failLocal(QStringLiteral("no ring tel connection on this session bus"));
        return;
    }
    tryConnection();
}

void TelepathySmsSender::tryConnection()
{
    if (m_candidateIdx >= m_candidates.size()) {
        failLocal(QStringLiteral("no ring connection resolved %1").arg(m_number));
        return;
    }
    m_connectionName = m_candidates.at(m_candidateIdx++);
    // The object path is the full bus name with every dot turned into a slash.
    m_connectionPath = m_connectionName;
    m_connectionPath.replace(QLatin1Char('.'), QLatin1Char('/'));
    m_connectionPath.prepend(QLatin1Char('/'));
    qInfo() << "telepathysmssender: trying" << m_connectionName << m_connectionPath;

    QDBusInterface conn(m_connectionName, m_connectionPath, CONTACTS_IFACE,
        QDBusConnection::sessionBus());
    QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher(
        conn.asyncCall("GetContactByID", m_number, QStringList() << TEXT_TYPE), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &TelepathySmsSender::resolveContact);
}

void TelepathySmsSender::resolveContact(QDBusPendingCallWatcher* watcher)
{
    if (watcher->isError()) {
        QString error = watcher->error().message();
        watcher->deleteLater();
        if (m_candidateIdx < m_candidates.size()) {
            qWarning() << "telepathysmssender: connection" << m_connectionName
                       << "unusable, trying next:" << error;
            tryConnection();
            return;
        }
        failLocal(QStringLiteral("GetContactByID failed on all ring connections: ") + error);
        return;
    }
    QDBusReply<quint32> reply = *watcher;
    watcher->deleteLater();
    if (!reply.isValid() || reply.value() == 0) {
        failLocal(QStringLiteral("ring resolved no contact for %1").arg(m_number));
        return;
    }
    m_handle = reply.value();

    QDBusInterface conn(m_connectionName, m_connectionPath, CONNECTION_IFACE,
        QDBusConnection::sessionBus());
    QDBusPendingCallWatcher* listWatcher = new QDBusPendingCallWatcher(
        conn.asyncCall("ListChannels"), this);
    connect(listWatcher, &QDBusPendingCallWatcher::finished, this, &TelepathySmsSender::findChannel);
}

void TelepathySmsSender::findChannel(QDBusPendingCallWatcher* watcher)
{
    if (watcher->isError()) {
        failWatcher("ListChannels", watcher);
        return;
    }
    QDBusReply<QList<QVariant>> reply = *watcher;
    watcher->deleteLater();

    // Reuse the open channel of the conversation when there is one, usually
    // opened by the dispatcher when the SMS arrived. Such a channel belongs
    // to the dispatcher; never Destroy one we did not create.
    for (const QVariant& entry : reply.value()) {
        const QVariantList channel = entry.toList();
        if (channel.size() >= 4 && channel.at(1).toString() == TEXT_TYPE
            && channel.at(2).toUInt() == HT_CONTACT && channel.at(3).toUInt() == m_handle) {
            m_channelPath = channel.at(0).toString();
            sendOnChannel();
            return;
        }
    }

    // No open channel: create one with the handler suppressed, so the
    // dispatcher does not open the Messages UI while we send.
    QDBusInterface conn(m_connectionName, m_connectionPath, CONNECTION_IFACE,
        QDBusConnection::sessionBus());
    QDBusPendingCallWatcher* reqWatcher = new QDBusPendingCallWatcher(
        conn.asyncCall("RequestChannel", TEXT_TYPE, HT_CONTACT, m_handle, true), this);
    connect(reqWatcher, &QDBusPendingCallWatcher::finished, this, &TelepathySmsSender::channelRequested);
}

void TelepathySmsSender::channelRequested(QDBusPendingCallWatcher* watcher)
{
    if (watcher->isError()) {
        failWatcher("RequestChannel", watcher);
        return;
    }
    QDBusReply<QDBusObjectPath> reply = *watcher;
    watcher->deleteLater();
    if (!reply.isValid()) {
        failLocal(QStringLiteral("ring returned no text channel for %1").arg(m_number));
        return;
    }
    m_channelPath = reply.value().path();
    m_ourChannel = true;
    sendOnChannel();
}

void TelepathySmsSender::sendOnChannel()
{
    QDBusInterface channel(m_connectionName, m_channelPath, TEXT_TYPE,
        QDBusConnection::sessionBus());
    QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher(
        channel.asyncCall("Send", static_cast<quint32>(1), m_text), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &TelepathySmsSender::textSent);
}

void TelepathySmsSender::textSent(QDBusPendingCallWatcher* watcher)
{
    if (watcher->isError()) {
        failWatcher("Send", watcher);
        return;
    }
    watcher->deleteLater();
    qDebug() << "telepathysmssender: sent SMS to" << m_number;
    if (m_ourChannel) {
        closeChannel();
    } else {
        finish();
    }
}

void TelepathySmsSender::closeChannel()
{
    QDBusInterface channel(m_connectionName, m_channelPath, DESTROYABLE_IFACE,
        QDBusConnection::sessionBus());
    QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher(
        channel.asyncCall("Destroy"), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &TelepathySmsSender::channelClosed);
}

void TelepathySmsSender::channelClosed(QDBusPendingCallWatcher* watcher)
{
    if (watcher->isError()) {
        qWarning() << "telepathysmssender: Destroy failed:"
                   << watcher->error().name() << watcher->error().message();
    }
    watcher->deleteLater();
    finish();
}
