#include "lomirismssender.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDebug>
#include <QVariantList>
#include <QVariantMap>

namespace {
// The service and its interface share one name.
const char* SERVICE = "com.lomiri.TelephonyServiceHandler";
const char* PATH = "/com/lomiri/TelephonyServiceHandler";
const char* JOB_IFACE = "com.lomiri.TelephonyServiceHandler.MessageSendingJob";
const char* PROPS_IFACE = "org.freedesktop.DBus.Properties";
const int CHAT_TYPE_CHAT = 1;

// MessageJob::Status from the service. The job deletes itself 60 s after
// it reaches a final state.
enum JobStatus { StatusPending,
    StatusInitialising,
    StatusRunning,
    StatusFinished,
    StatusFailed };
}

// SendMessage() expects an empty a(sss) list; a bare QVariantList would go
// out as av and the service rejects the call. This marshals an empty list of
// (sss) structs instead. Never filled, we do not send attachments.
struct SmsAttachment {
    QString content;
    QString identifier;
    QString contentType;
};

QDBusArgument& operator<<(QDBusArgument& arg, const SmsAttachment& attachment)
{
    arg.beginStructure();
    arg << attachment.content << attachment.identifier << attachment.contentType;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, SmsAttachment& attachment)
{
    arg.beginStructure();
    arg >> attachment.content >> attachment.identifier >> attachment.contentType;
    arg.endStructure();
    return arg;
}

Q_DECLARE_METATYPE(SmsAttachment)
Q_DECLARE_METATYPE(QList<SmsAttachment>)

QSet<LomiriSmsSender*> LomiriSmsSender::s_senders;

QPointer<LomiriSmsSender> LomiriSmsSender::send(const QString& account, const QString& number,
    const QString& text, QObject* target)
{
    // Both the struct and the list must be registered for D-Bus: without the
    // element type Qt has no signature for the list and setArguments() drops
    // the argument silently, so the service sees a 3-argument SendMessage.
    static const int registeredStruct = qDBusRegisterMetaType<SmsAttachment>();
    static const int registeredList = qDBusRegisterMetaType<QList<SmsAttachment>>();
    Q_UNUSED(registeredStruct);
    Q_UNUSED(registeredList);

    LomiriSmsSender* sender = new LomiriSmsSender(account, number, text, target);
    s_senders.insert(sender);

    // One call carries the whole send; the service runs the Mission Control
    // EnsureChannel dance itself. The argument shape was captured live from
    // the messaging app on UT 24.04, and account is the MC account name,
    // such as ofono/ofono/ril_0.
    QVariantMap properties;
    properties.insert(QStringLiteral("chatType"), CHAT_TYPE_CHAT);
    properties.insert(QStringLiteral("participantIds"), QStringList() << number);
    properties.insert(QStringLiteral("threadId"), number);

    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(SERVICE), QLatin1String(PATH), QLatin1String(SERVICE), QStringLiteral("SendMessage"));
    call.setArguments(QList<QVariant>()
        << account << text << QVariant::fromValue(QList<SmsAttachment>()) << properties);

    QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(call);
    QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher(pending, sender);
    connect(watcher, &QDBusPendingCallWatcher::finished, sender, &LomiriSmsSender::callFinished);
    return sender;
}

LomiriSmsSender::LomiriSmsSender(const QString& account, const QString& number, const QString& text, QObject* target)
    : QObject(target)
    , m_account(account)
    , m_number(number)
    , m_text(text)
{
}

void LomiriSmsSender::fail(const QString& reason)
{
    qWarning() << "lomirismssender:" << reason;
    emit failureSignal(m_number, reason);
    finish();
}

void LomiriSmsSender::failWatcher(const QString& stage, QDBusPendingCallWatcher* watcher)
{
    qWarning() << "lomirismssender:" << stage << "failed:"
               << watcher->error().name() << watcher->error().message();
    QString reason = watcher->error().message();
    watcher->deleteLater();
    fail(reason);
}

void LomiriSmsSender::finish()
{
    if (m_done) {
        return;
    }
    m_done = true;
    if (!m_jobPath.isEmpty()) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.disconnect(QLatin1String(SERVICE), m_jobPath, QLatin1String(JOB_IFACE),
            QStringLiteral("statusChanged"), this, SLOT(jobStatusChanged()));
        bus.disconnect(QLatin1String(SERVICE), m_jobPath, QLatin1String(JOB_IFACE),
            QStringLiteral("finished"), this, SLOT(jobStatusChanged()));
    }
    s_senders.remove(this);
    deleteLater();
}

void LomiriSmsSender::callFinished(QDBusPendingCallWatcher* watcher)
{
    if (watcher->isError()) {
        failWatcher("SendMessage", watcher);
        return;
    }
    QDBusPendingReply<QString> reply = *watcher;
    watcher->deleteLater();

    m_jobPath = reply.value();
    qDebug() << Q_FUNC_INFO << "reply to" << m_number << "queued via account" << m_account
             << "as job" << m_jobPath;
    watchJob(m_jobPath);
}

void LomiriSmsSender::watchJob(const QString& jobPath)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.connect(QLatin1String(SERVICE), jobPath, QLatin1String(JOB_IFACE),
            QStringLiteral("statusChanged"), this, SLOT(jobStatusChanged()))
        || !bus.connect(QLatin1String(SERVICE), jobPath, QLatin1String(JOB_IFACE),
            QStringLiteral("finished"), this, SLOT(jobStatusChanged()))) {
        qWarning() << "lomirismssender: could not watch job" << jobPath
                   << ", failures of the send will go unnoticed";
    }
    // A bad account fails almost immediately, possibly before the connect
    // above was in place; poll the status once right away.
    jobStatusChanged();
}

void LomiriSmsSender::jobStatusChanged()
{
    if (m_done) {
        return;
    }
    bool ok = false;
    int status = readJobStatus(ok);
    if (!ok) {
        fail(QStringLiteral("the telephony service dropped the send job"));
        return;
    }
    qDebug() << Q_FUNC_INFO << "job status" << status;
    if (status == StatusFailed) {
        fail(QStringLiteral("the telephony service could not send the message (status %1)").arg(status));
    } else if (status == StatusFinished) {
        qInfo() << "lomirismssender: telephony job finished for" << m_number;
        finish();
    }
}

int LomiriSmsSender::readJobStatus(bool& ok)
{
    QDBusInterface props(QLatin1String(SERVICE), m_jobPath, QLatin1String(PROPS_IFACE),
        QDBusConnection::sessionBus());
    QDBusReply<QVariant> value = props.call(QStringLiteral("Get"), QString(QLatin1String(JOB_IFACE)),
        QStringLiteral("status"));
    if (!value.isValid()) {
        qWarning() << "lomirismssender: cannot read status of job" << m_jobPath << ":"
                   << value.error().message();
        ok = false;
        return -1;
    }
    ok = true;
    return value.value().toInt();
}
