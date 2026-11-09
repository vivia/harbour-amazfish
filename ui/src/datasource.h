#ifndef DATASOURCE_H
#define DATASOURCE_H

#include <QObject>
#include <QVariant>

#include <KDb3/KDbConnection>

class DataSource: public QObject
{
    Q_OBJECT

public:
    enum Type {
        Heartrate = 1,
        Steps = 2,
        Sleep = 3,
        Intensity = 4,
        StepSummary = 5,
        SleepSummary = 6,
        BatteryLog = 7,
        HRV = 8,
        Spo2Normal = 9,
        Spo2Sleep = 10,
        BodyTemperature = 11,
        StressAuto = 12,
        StressManual = 13,
        StressSummary = 14,
        Vo2maxSummary = 15
    };
    Q_ENUM(Type)

    DataSource();
    void setConnection(KDbConnection *conn);

    Q_INVOKABLE QVariant data(const DataSource::Type type, const QDate  &day);

private:
    KDbConnection *m_conn = nullptr;
    
    struct SleepSession {
        QDateTime sleepStart;
        QDateTime sleepEnd;
        long lightSleepDuration;
        long deepSleepDuration;
        long remSleepDuration = 0;
    };
    QList<SleepSession> calculateSleep(const QDate &day);
    bool calculateSleepStages(const QDate& day, SleepSession& session);
    bool isSleep(int kind);
};

#endif // DATASOURCE_H
