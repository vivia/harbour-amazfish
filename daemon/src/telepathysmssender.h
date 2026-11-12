#ifndef TELEPATHYSMSSENDER_H
#define TELEPATHYSMSSENDER_H

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

class QDBusPendingCallWatcher;

// Sends an SMS through telepathy-ring, bypassing the Messages UI.
// The Messages remote action (sendMessage) is refused for untrusted callers,
// while the ring connection accepts sends from this daemon's D-Bus context.
// Replies still show up in the Messages conversation history.
class TelepathySmsSender;

class TelepathySmsSender : public QObject {
    Q_OBJECT
public:
    // target only receives failureSignal; the sender deletes itself when done.
    static QPointer<TelepathySmsSender> send(const QString& number, const QString& text, QObject* target = nullptr);

signals:
    void failureSignal(const QString& number, const QString& error);

private:
    TelepathySmsSender(const QString& number, const QString& text, QObject* target);

    void failLocal(const QString& reason);
    void failWatcher(const QString& stage, QDBusPendingCallWatcher* watcher);
    void tryConnection();
    void sendOnChannel();
    void closeChannel();
    void finish();

    Q_SLOT void findConnection(QDBusPendingCallWatcher* watcher);
    Q_SLOT void resolveContact(QDBusPendingCallWatcher* watcher);
    Q_SLOT void findChannel(QDBusPendingCallWatcher* watcher);
    Q_SLOT void channelRequested(QDBusPendingCallWatcher* watcher);
    Q_SLOT void textSent(QDBusPendingCallWatcher* watcher);
    Q_SLOT void channelClosed(QDBusPendingCallWatcher* watcher);

    QString m_number;
    QString m_text;
    QString m_connectionName;
    QString m_connectionPath;
    QString m_channelPath;
    QStringList m_candidates;
    int m_candidateIdx = 0;
    quint32 m_handle = 0;
    bool m_ourChannel = false;

    // Keeps running senders alive; each sender removes itself when done.
    static QSet<TelepathySmsSender*> s_senders;
};

#endif
