#ifndef LOMIRISMSSENDER_H
#define LOMIRISMSSENDER_H

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

class QDBusPendingCallWatcher;

// Sends an SMS through lomiri-telephony-service (Ubuntu Touch), bypassing the
// messaging app. Lomiri's SMS notification carries no input action, and the
// service reports the result of a send only on the job object it returns.
// Replies still land in the conversation history.
class LomiriSmsSender;

class LomiriSmsSender : public QObject {
    Q_OBJECT
public:
    // target only receives failureSignal; the sender deletes itself when done.
    static QPointer<LomiriSmsSender> send(const QString& account, const QString& number,
        const QString& text, QObject* target = nullptr);

signals:
    void failureSignal(const QString& number, const QString& error);

private:
    LomiriSmsSender(const QString& account, const QString& number, const QString& text, QObject* target);

    void fail(const QString& reason);
    void failWatcher(const QString& stage, QDBusPendingCallWatcher* watcher);
    void watchJob(const QString& jobPath);
    int readJobStatus(bool& ok);
    void finish();

    Q_SLOT void callFinished(QDBusPendingCallWatcher* watcher);
    Q_SLOT void jobStatusChanged();

    QString m_account;
    QString m_number;
    QString m_text;
    QString m_jobPath;
    bool m_done = false;

    // Keeps running senders alive; each sender removes itself when done.
    static QSet<LomiriSmsSender*> s_senders;
};

#endif
