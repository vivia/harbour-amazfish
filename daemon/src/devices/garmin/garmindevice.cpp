#include "garmindevice.h"
#include "amazfishconfig.h"
#include "deviceinfoservice.h"
#include "hrmservice.h"
#include "services/garmin/communicator_v2.h"
#include "services/garmin/garmintypes.h"
#include "services/garmin/garminweathermessage.h"

#include "activitysummary.h"
#include "services/garmin/garminfitgpx.h"
#include "services/garmin/garminfitmonitor.h"
#include "services/garmin/garminfitsleep.h"
#include "services/garmin/garminfitsummary.h"

#include "services/garmin/garminfitmetrics.h"
#include "services/garmin/garminfitspo2.h"
#include "services/garmin/garminfitstress.h"

#include <KDb.h>
#include <QDomDocument>
#include <QStringList>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

const char* UUID_SERVICE_GARMIN_GFDI_V0 = "9b012401-bc30-ce9a-e111-0f67e491abde";
const char* UUID_CHARACTERISTIC_GARMIN_GFDI_V0_SEND = "df334c80-e6a7-d082-274a-78fc66f85e16";
const char* UUID_CHARACTERISTIC_GARMIN_GFDI_V0_RECEIVE = "4acbcd28-7425-868e-f447-915c8f00d0cb";

const char* UUID_SERVICE_GARMIN_GFDI_V1 = "6a4e2401-667b-11e3-949a-0800200c9a66";
const char* UUID_CHARACTERISTIC_GARMIN_GFDI_V1_SEND = "6a4e4c80-667b-11e3-949a-0800200c9a66";
const char* UUID_CHARACTERISTIC_GARMIN_GFDI_V1_RECEIVE = "6a4ecd28-667b-11e3-949a-0800200c9a66";

const char* UUID_SERVICE_GARMIN_ML_GFDI = "6a4e2800-667b-11e3-949a-0800200c9a66";

GarminDevice::GarminDevice(const QString &pairedName, QObject *parent) : AbstractDevice(pairedName, parent)
{
    qDebug() << Q_FUNC_INFO << pairedName;
    connect(this, &QBLEDevice::propertiesChanged, this, &GarminDevice::onPropertiesChanged, Qt::UniqueConnection);
    m_pairing= false;
}

Amazfish::Features GarminDevice::supportedFeatures() const
{

    //HRM and steps should be supported on all devices

    return Amazfish::Feature::FEATURE_NONE
        | Amazfish::Feature::FEATURE_ACTIVITY
        | Amazfish::Feature::FEATURE_HRM
        | Amazfish::Feature::FEATURE_STEPS
        | Amazfish::Feature::FEATURE_ALERT
        | Amazfish::Feature::FEATURE_WEATHER;
}


Amazfish::DataTypes GarminDevice::supportedDataTypes() const
{
    return Amazfish::DataType::TYPE_SPO2
        | Amazfish::DataType::TYPE_STRESS
        | Amazfish::DataType::TYPE_VO2MAX
        | Amazfish::DataType::TYPE_DAILY_CALORIES
        | Amazfish::DataType::TYPE_HRV
        | Amazfish::DataType::TYPE_HEART_RATE
        | Amazfish::DataType::TYPE_ACTIVITY
        | Amazfish::DataType::TYPE_SLEEP;
}

QString GarminDevice::deviceType() const
{
    return "garmin";
}

AbstractFirmwareInfo *GarminDevice::firmwareInfo(const QByteArray &bytes, const QString &path)
{
    Q_UNUSED(bytes);
    Q_UNUSED(path);
    qDebug() << Q_FUNC_INFO;
    return nullptr;
}

void GarminDevice::sendAlert(const Amazfish::WatchNotification &notification)
{
    qDebug() << Q_FUNC_INFO << "notification from " << notification.appName;
    if (mNotificationHandler)
    {
        NotificationSpec note;
        note.body = notification.body;
        note.sourceName = notification.appName;
        note.title = notification.summary;
        // Configure notification type based on Appname (small list so far, needs to be extended);
        if (notification.appName=="Whisperfish")
            note.notificationType=NotificationType::GenericSocial;
        else if (notification.appName=="Signal")
            note.notificationType=NotificationType::GenericSocial;
        // It seems for emails we don't see the app name but the sender address. Bug or feature?
        else if (notification.appName.contains('@'))
            note.notificationType=NotificationType::GenericEmail;
        else if (notification.appName=="Calendar")
            note.notificationType=NotificationType::GenericCalendar;
        else note.notificationType = NotificationType::Generic;
        note.id=notification.id;
        mNotificationHandler->onNotification(note);
    }
    else qDebug() << Q_FUNC_INFO << "Garmin: No notification handler!";
}

void GarminDevice::incomingCall(const QString &caller)
{
    qDebug() << Q_FUNC_INFO << caller;
    if (mNotificationHandler)
    {
        CallSpec call;
        call.name = caller;
        call.command = CallCommand::Incoming;
        mNotificationHandler->onSetCallState(call);
    }
    else qDebug() << Q_FUNC_INFO << "Garmin: No notification handler!";
}

void GarminDevice::incomingCallEnded()
{
    qDebug() << Q_FUNC_INFO;
    if (mNotificationHandler)
    {
        CallSpec call;
        call.command = CallCommand::End;
        mNotificationHandler->onSetCallState(call);
    }
    else qDebug() << Q_FUNC_INFO << "Garmin: No notification handler!";
}

namespace {
// One MusicControlEntityUpdate (5049) record: [len][entityId][ordinal][0][utf8 value].
// len counts its own byte plus the three header bytes; a value is at most 252 bytes.
constexpr quint8 MUSIC_ENTITY_PLAYER = 0;
constexpr quint8 MUSIC_ENTITY_TRACK = 2;
constexpr quint8 PLAYER_PLAYBACK_INFO = 1;
constexpr quint8 TRACK_ARTIST = 0;
constexpr quint8 TRACK_ALBUM = 1;
constexpr quint8 TRACK_TITLE = 2;
constexpr quint8 TRACK_DURATION = 3;

// MusicControl command ordinals, same list as sent in the capabilities reply.
constexpr quint8 MUSIC_CMD_PLAYPAUSE = 0;
constexpr quint8 MUSIC_CMD_NEXT = 1;
constexpr quint8 MUSIC_CMD_PREV = 2;
constexpr quint8 MUSIC_CMD_VOLUP = 3;
constexpr quint8 MUSIC_CMD_VOLDOWN = 4;

void appendMusicEntityRecord(QByteArray& payload, quint8 entityId, quint8 ordinal, const QString& value)
{
    QString text = value;
    QByteArray bytes = text.toUtf8();
    while (bytes.size() > 252) {
        text.chop(1);
        bytes = text.toUtf8();
    }
    payload.append(char(bytes.size() + 3));
    payload.append(char(entityId));
    payload.append(char(ordinal));
    payload.append(char(0)); // 4th byte, purpose unknown, Gadgetbridge sends 0
    payload.append(bytes);
}
}

void GarminDevice::setMusicStatus(bool playing, const QString& title, const QString& artist, const QString& album, int duration, int position)
{
    qDebug() << Q_FUNC_INFO << playing << artist << album << title;
    if (!mCommunicator) {
        return;
    }
    QByteArray payload;
    appendMusicEntityRecord(payload, MUSIC_ENTITY_TRACK, TRACK_ARTIST, artist);
    appendMusicEntityRecord(payload, MUSIC_ENTITY_TRACK, TRACK_ALBUM, album);
    appendMusicEntityRecord(payload, MUSIC_ENTITY_TRACK, TRACK_TITLE, title);
    appendMusicEntityRecord(payload, MUSIC_ENTITY_TRACK, TRACK_DURATION, QString::number(duration));
    // PLAYER.PLAYBACK_INFO is "playing,rate,position"; the rate stays 1.0 while playing.
    appendMusicEntityRecord(payload, MUSIC_ENTITY_PLAYER, PLAYER_PLAYBACK_INFO,
        QString(playing ? "1,1.0," : "0,0.0,") + QString::number(position));
    mCommunicator->sendMessage("MUSIC UPDATE", wrapInGfdiEnvelope(MessageId::MusicControlEntityUpdate, payload));
}

void GarminDevice::onPropertiesChanged(QString interface, QVariantMap map, QStringList list)
{
    qDebug() << Q_FUNC_INFO << interface << map << list;

    if (interface == "org.bluez.Device1") {
        // Captured before initialise(): on a first connect that call creates the communicator and
        // its constructor talks to the watch. Only a reconnect has nothing created and needs a re-run.
        CommunicatorV2* svc = mCommunicator.data();
        if (deviceProperty("ServicesResolved").toBool() ) {
            initialise();
            if (svc) {
                qDebug() << Q_FUNC_INFO << "Garmin: Communicator already exists, re-initialising";
                svc->onConnectionStateChange(true);
                // drv_createService() set this state on the first connect and is not reached again.
                setConnectionState("authenticated");
            }
        }
        if (map.contains("Connected")) {
            bool value = map["Connected"].toBool();
            if (!value) {
                // A weather request from this connection must not be answered on the next one.
                mWeatherRequested = false;
                mWeatherRefreshRequested = false;
                setConnectionState("disconnected");
                if (svc)
                {
                    qDebug() << Q_FUNC_INFO << "Garmin: Communicator already exists, sending disconnect";
                    svc->onConnectionStateChange(false);
                }
            } else {
                setConnectionState("connected");
            }
        }
    }
}
void GarminDevice::onAnswerCallEvent() {
    emit deviceEvent(AbstractDevice::EVENT_ANSWER_CALL);
}

void GarminDevice::onRejectCallEvent(){
    emit deviceEvent(AbstractDevice::EVENT_DECLINE_CALL);
}

void GarminDevice::onFindPhoneRequested()
{
    emit deviceEvent(AbstractDevice::EVENT_FIND_PHONE);
}

void GarminDevice::onFindPhoneCancelled()
{
    emit deviceEvent(AbstractDevice::EVENT_CANCEL_FIND_PHONE);
}

void GarminDevice::onMusicControlCommand(quint8 command)
{
    switch (command) {
    case MUSIC_CMD_PLAYPAUSE:
        emit deviceEvent(AbstractDevice::EVENT_MUSIC_PLAYPAUSE);
        break;
    case MUSIC_CMD_NEXT:
        emit deviceEvent(AbstractDevice::EVENT_MUSIC_NEXT);
        break;
    case MUSIC_CMD_PREV:
        emit deviceEvent(AbstractDevice::EVENT_MUSIC_PREV);
        break;
    case MUSIC_CMD_VOLUP:
        emit deviceEvent(AbstractDevice::EVENT_MUSIC_VOLUP);
        break;
    case MUSIC_CMD_VOLDOWN:
        emit deviceEvent(AbstractDevice::EVENT_MUSIC_VOLDOWN);
        break;
    default:
        qDebug() << Q_FUNC_INFO << "Garmin: Unknown music command" << command;
    }
}

QBLEService *GarminDevice::drv_createService(const QString &uuid, const QString &path)
{
    qDebug() << Q_FUNC_INFO << "Creating Communicator service for Garmin";
    // Only the preferred GFDI generation gets a CommunicatorV2.
    if (uuid != mPreferredService) {
        return nullptr;
    }

    QSharedPointer<CommunicatorV2> com = QSharedPointer<CommunicatorV2>::create(path, this);
    if (com) {
        mCommunicator = com;
        connect(com.data(), &CommunicatorV2::informationChanged, this, &GarminDevice::informationChanged, Qt::UniqueConnection);
        // add notification handler
        qDebug() << Q_FUNC_INFO << "Garmin: Adding notification handler";
        if (!mNotificationHandler)
            mNotificationHandler = QSharedPointer<GarminNotificationHandler>::create(mCommunicator);
        connect(com.data(), &CommunicatorV2::NotificationDataRequested, mNotificationHandler.data(), &GarminNotificationHandler::onNotificationDataRequested);
        connect(com.data(), &CommunicatorV2::NotificationPerformAction, mNotificationHandler.data(), &GarminNotificationHandler::onNotificationPerformAction);
        connect(mNotificationHandler.data(), &GarminNotificationHandler::acceptIncomingCall, this, &GarminDevice::onAnswerCallEvent);
        connect(mNotificationHandler.data(), &GarminNotificationHandler::rejectIncomingCall, this, &GarminDevice::onRejectCallEvent);
        connect(com.data(), &CommunicatorV2::weatherRequestReceived, this, &GarminDevice::onWeatherRequestReceived, Qt::UniqueConnection);
        connect(com.data(), &CommunicatorV2::fileParsed, this, &GarminDevice::onFileParsed, Qt::UniqueConnection);
        connect(com.data(), &CommunicatorV2::findPhoneRequested, this, &GarminDevice::onFindPhoneRequested, Qt::UniqueConnection);
        connect(com.data(), &CommunicatorV2::findPhoneCancelled, this, &GarminDevice::onFindPhoneCancelled, Qt::UniqueConnection);
        connect(com.data(), &CommunicatorV2::musicControlCommand, this, &GarminDevice::onMusicControlCommand, Qt::UniqueConnection);
        // A fresh communicator knows nothing about weather yet. Hand it what
        // we already have, so HttpService requests are answered right away.
        if (mWeather) {
            com->setWeatherCache(mWeather);
        }
        setConnectionState("authenticated");
        return com.data();
    }
    // if we are here, no Garmin device was detected or serice already created
    return nullptr;
}

void GarminDevice::initialise()
{
    qDebug() << Q_FUNC_INFO;
    // A watch can advertise more than one GFDI generation. Use the newest one available.
    // The last case is unconditional: if BlueZ reports no UUIDs, the V0 link is still the
    // safest guess.
    const QStringList advertised = deviceProperty("UUIDs").toStringList();
    if (advertised.contains(UUID_SERVICE_GARMIN_ML_GFDI)) {
        mPreferredService = UUID_SERVICE_GARMIN_ML_GFDI;
    } else if (advertised.contains(UUID_SERVICE_GARMIN_GFDI_V1)) {
        mPreferredService = UUID_SERVICE_GARMIN_GFDI_V1;
    } else {
        mPreferredService = UUID_SERVICE_GARMIN_GFDI_V0;
    }
    parseServices();
}

void GarminDevice::sendWeather(CurrentWeather* weather)
{
    mWeather = weather;
    // Keep the communicator's copy current for the HttpService weather
    // endpoints the watch fetches on its own.
    if (mCommunicator) {
        mCommunicator->setWeatherCache(weather);
    }
    // Answer a request the watch sent while we had no data. A Lily 2 fetches
    // weather over HttpService itself, so never push unasked. Keep the request
    // pending when the update carries no usable current values.
    if (mWeatherRequested && weather && weather->hasCurrentData()) {
        mWeatherRequested = false;
        sendWeatherToWatch();
    }
}

void GarminDevice::onWeatherRequestReceived()
{
    if (!mWeather || !mWeather->hasCurrentData()) {
        mWeatherRequested = true;
        if (!mWeatherRefreshRequested) {
            // The next scheduled fetch can be an hour away, so ask for one now. Once per
            // connection: the watch asks again every few seconds while it has nothing.
            mWeatherRefreshRequested = true;
            emit weatherRequested();
        }
        qDebug() << Q_FUNC_INFO << "Garmin: No weather data yet, answering when the next update arrives";
        return;
    }
    sendWeatherToWatch();
}

void GarminDevice::sendWeatherToWatch()
{
    if (!mCommunicator || !mWeather || !mWeather->hasCurrentData()) {
        return;
    }
    if (mCommunicator->httpWeatherSeen()) {
        qWarning() << Q_FUNC_INFO << "Garmin: Watch fetches weather over HTTP, FIT records not sent";
        return;
    }
    const FitWeatherFrames frames = GarminWeatherMessage::encodeWeather(mWeather);
    mCommunicator->sendWeatherFrames(frames.fitDefinition, frames.fitData);
}

void GarminDevice::fetchData(Amazfish::DataTypes dataTypes)
{
    if (!(dataTypes & Amazfish::DataType::TYPE_ACTIVITY) || !mCommunicator) {
        return;
    }
    // The watch answers with a file list; the newest activity is what we want from it.
    if (!mCommunicator->requestDirectoryListing()) {
        emit message(tr("An operation is currently running, please try later"));
    }
}

void GarminDevice::onFileParsed(quint8 type, quint8 subType, qint32 stamp, const QVector<GarminFitMessage>& messages)
{
    // The communicator only queues files newer than the saved stamp. The stamp
    // is in seconds on the watch's own epoch.
    if (type == DIR_TYPE_FIT && subType == DIR_SUBTYPE_SLEEP) {
        QVector<GarminSleepSample> samples;
        QDateTime assessmentStart;
        int score = 0;
        buildGarminSleepSamples(messages, samples);
        buildGarminSleepAssessment(messages, assessmentStart, score);
        if (samples.isEmpty() && !assessmentStart.isValid()) {
            // A rest detection with no stages and no score: nothing to store,
            // but the file is done with. Advance the stamp so it is not
            // fetched again on every sync.
            qDebug() << Q_FUNC_INFO << "Garmin: sleep file has no stages and no score, skipping";
            AmazfishConfig::instance()->setValue(STAMP_SLEEP, stamp);
            return;
        }
        if (saveSleepData(samples, assessmentStart, score)) {
            AmazfishConfig::instance()->setValue(STAMP_SLEEP, stamp);
        }
        return;
    }
    if (type == DIR_TYPE_FIT && subType == DIR_SUBTYPE_HRV) {
        QVector<GarminHrvSample> samples;
        buildGarminHrvSamples(messages, samples);
        if (samples.isEmpty()) {
            // A file with no 5 minute values: nothing to store, but the file
            // is done with. Advance the stamp so it is not fetched again.
            qDebug() << Q_FUNC_INFO << "Garmin: hrv file has no values, skipping";
            AmazfishConfig::instance()->setValue(STAMP_HRV, stamp);
            return;
        }
        if (saveHrvData(samples)) {
            AmazfishConfig::instance()->setValue(STAMP_HRV, stamp);
        }
        return;
    }
    if (type == DIR_TYPE_FIT && subType == DIR_SUBTYPE_MONITOR) {
        QVector<GarminMonitorSample> samples;
        QVector<GarminSpo2Sample> spo2Samples;
        QVector<GarminStressSample> stressSamples;
        QVector<GarminDailyCaloriesSample> caloriesSamples;
        buildGarminMonitorSamples(messages, samples);
        buildGarminSpo2Samples(messages, spo2Samples);
        buildGarminStressSamples(messages, stressSamples);
        buildGarminDailyCaloriesSamples(messages, caloriesSamples);
        if (samples.isEmpty() && spo2Samples.isEmpty() && stressSamples.isEmpty() && caloriesSamples.isEmpty()) {
            // No heart rate, no step movement, no blood oxygen, no stress and
            // no calories: nothing to store, but the file is done with.
            // Advance the stamp.
            qDebug() << Q_FUNC_INFO << "Garmin: monitor file has no samples, skipping";
            AmazfishConfig::instance()->setValue(STAMP_MONITOR, stamp);
            return;
        }
        bool saved = true;
        if (!samples.isEmpty() && !saveMonitorData(samples)) {
            saved = false;
        }
        if (!spo2Samples.isEmpty() && !saveSpo2Data(spo2Samples)) {
            saved = false;
        }
        if (!stressSamples.isEmpty() && !saveStressData(stressSamples)) {
            saved = false;
        }
        if (!caloriesSamples.isEmpty() && !saveDailyCaloriesData(caloriesSamples)) {
            saved = false;
        }
        // Move the stamp only when every domain the file offered is stored.
        // Otherwise the file is fetched again and the rest can still arrive.
        if (saved) {
            AmazfishConfig::instance()->setValue(STAMP_MONITOR, stamp);
        }
        return;
    }
    if (type == DIR_TYPE_FIT && subType == DIR_SUBTYPE_METRICS) {
        QVector<GarminVo2maxSample> samples;
        buildGarminVo2maxSamples(messages, samples);
        if (samples.isEmpty()) {
            // Most metrics files hold other metrics and no VO2max reading.
            // Nothing to store, but the file is done with. Advance the stamp
            // so it is not fetched again.
            qDebug() << Q_FUNC_INFO << "Garmin: metrics file has no vo2max reading, skipping";
            AmazfishConfig::instance()->setValue(STAMP_METRICS, stamp);
            return;
        }
        if (saveVo2maxData(samples)) {
            AmazfishConfig::instance()->setValue(STAMP_METRICS, stamp);
        }
        return;
    }
    if (saveActivityMessages(messages)) {
        AmazfishConfig::instance()->setValue(STAMP_ACTIVITY, stamp);
    }
}

bool GarminDevice::saveActivityMessages(const QVector<GarminFitMessage>& messages)
{
    if (!m_conn || !m_conn->isDatabaseUsed()) {
        qDebug() << Q_FUNC_INFO << "Garmin: Database not connected";
        return false;
    }

    ActivitySummary summary;
    if (!buildGarminFitSummary(messages, summary)) {
        qDebug() << Q_FUNC_INFO << "Garmin: no session in the activity file, not saving";
        return false;
    }

    auto config = AmazfishConfig::instance();
    summary.setProfileId(qHash(config->profileName()));
    summary.setDeviceId(qHash(config->pairedAddress()));

    // A rescan replaces the row for the same activity, because the new parse
    // may know more than the old one. The name is derived from the file
    // contents, so it identifies the activity across rescans.
    int oldId = 0;
    // escapeString() adds the SQL quotes around the name. arg(QString) does not,
    // so without it SQLite reads the name as an identifier and finds nothing.
    const KDbEscapedString escapedName = m_conn->escapeString(summary.name());
    const auto foundOld = m_conn->querySingleNumber(
        KDbEscapedString("SELECT id FROM sports_data WHERE name = %1").arg(escapedName), &oldId);
    if (foundOld == true) {
        m_conn->executeSql(KDbEscapedString("DELETE FROM sports_meta WHERE sport_id = %1").arg(oldId));
        m_conn->executeSql(KDbEscapedString("DELETE FROM sports_data WHERE id = %1").arg(oldId));
    } else if (foundOld == false) {
        qWarning() << Q_FUNC_INFO << "Garmin: cannot look up the stored row for" << summary.name();
    }

    QDir laufhelden(QDir::homePath() + "/Laufhelden/");
    const QString gpxDir = laufhelden.exists() ? laufhelden.absolutePath()
                                               : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QFile gpxFile(gpxDir + "/" + summary.name() + ".gpx");
    if (gpxFile.open(QIODevice::WriteOnly)) {
        QTextStream stream(&gpxFile);
        stream << garminFitGpxText(messages);
        gpxFile.close();
        summary.setGPX(gpxFile.fileName());
    } else {
        qDebug() << Q_FUNC_INFO << "Garmin: cannot write" << gpxFile.fileName();
    }

    if (summary.saveToDatabase(m_conn)) {
        qDebug() << Q_FUNC_INFO << "Garmin: saved activity" << summary.name();
        return true;
    }
    qDebug() << Q_FUNC_INFO << "Garmin: could not save activity" << summary.name();
    return false;
}

bool GarminDevice::saveSleepMessages(const QVector<GarminFitMessage>& messages)
{
    QVector<GarminSleepSample> samples;
    QDateTime assessmentStart;
    int score = 0;
    buildGarminSleepSamples(messages, samples);
    buildGarminSleepAssessment(messages, assessmentStart, score);
    return saveSleepData(samples, assessmentStart, score);
}

bool GarminDevice::saveSleepData(const QVector<GarminSleepSample>& samples, const QDateTime& assessmentStart, int score)
{
    if (!m_conn || !m_conn->isDatabaseUsed()) {
        qDebug() << Q_FUNC_INFO << "Garmin: Database not connected";
        return false;
    }

    // The builders leave samples empty when there was no real sleep, and an
    // invalid start when there was no score.
    const bool hasSamples = !samples.isEmpty();
    const bool hasAssessment = assessmentStart.isValid();

    if (!hasSamples && !hasAssessment) {
        return false;
    }

    auto sleepTable = m_conn->tableSchema("sleep");
    auto assessmentTable = m_conn->tableSchema("sleep_assessment");
    if ((hasSamples && !sleepTable) || (hasAssessment && !assessmentTable)) {
        qDebug() << Q_FUNC_INFO << "Garmin: sleep tables missing";
        return false;
    }

    KDbTransaction transaction = m_conn->beginTransaction();
    KDbTransactionGuard tg(transaction);

    if (hasSamples) {
        KDbFieldList fields;
        fields.addField(sleepTable->field("sleep_timestamp"));
        fields.addField(sleepTable->field("sleep_timestamp_dt"));
        fields.addField(sleepTable->field("sleep_stage"));

        for (const GarminSleepSample& sample : samples) {
            const qint64 stamp = sample.timestamp.toMSecsSinceEpoch() / 1000;
            // A rescan parses the file again. The stage belongs to its
            // timestamp, so the old row for it is replaced, not duplicated.
            if (!m_conn->executeSql(KDbEscapedString("DELETE FROM sleep WHERE sleep_timestamp = %1").arg(stamp))) {
                qDebug() << Q_FUNC_INFO << "Garmin: could not replace stage at" << stamp;
                return false;
            }
            QList<QVariant> values;
            values << stamp;
            values << sample.timestamp;
            values << sample.stage;
            if (!m_conn->insertRecord(&fields, values)) {
                qDebug() << Q_FUNC_INFO << "Garmin: error inserting sleep stage";
                return false;
            }
        }
    }

    if (hasAssessment) {
        const qint64 stamp = assessmentStart.toMSecsSinceEpoch() / 1000;
        if (!m_conn->executeSql(KDbEscapedString("DELETE FROM sleep_assessment WHERE sleep_assessment_timestamp = %1").arg(stamp))) {
            qDebug() << Q_FUNC_INFO << "Garmin: could not replace assessment at" << stamp;
            return false;
        }
        KDbFieldList fields;
        fields.addField(assessmentTable->field("sleep_assessment_timestamp"));
        fields.addField(assessmentTable->field("sleep_assessment_timestamp_dt"));
        fields.addField(assessmentTable->field("sleep_assessment_score"));

        QList<QVariant> values;
        values << stamp;
        values << assessmentStart;
        values << score;
        if (!m_conn->insertRecord(&fields, values)) {
            qDebug() << Q_FUNC_INFO << "Garmin: error inserting sleep assessment";
            return false;
        }
    }

    tg.commit();
    qDebug() << Q_FUNC_INFO << "Garmin: saved" << samples.size() << "sleep stages"
             << (hasAssessment ? QStringLiteral("and score %1").arg(score) : QString());
    return true;
}

bool GarminDevice::saveHrvMessages(const QVector<GarminFitMessage>& messages)
{
    QVector<GarminHrvSample> samples;
    if (!buildGarminHrvSamples(messages, samples)) {
        return false;
    }
    return saveHrvData(samples);
}

bool GarminDevice::saveHrvData(const QVector<GarminHrvSample>& samples)
{
    if (!m_conn || !m_conn->isDatabaseUsed()) {
        qDebug() << Q_FUNC_INFO << "Garmin: Database not connected";
        return false;
    }
    if (samples.isEmpty()) {
        return false;
    }

    auto hrvTable = m_conn->tableSchema("hrv");
    if (!hrvTable) {
        qDebug() << Q_FUNC_INFO << "Garmin: hrv table missing";
        return false;
    }

    KDbTransaction transaction = m_conn->beginTransaction();
    KDbTransactionGuard tg(transaction);

    KDbFieldList fields;
    fields.addField(hrvTable->field("hrv_timestamp"));
    fields.addField(hrvTable->field("hrv_timestamp_dt"));
    fields.addField(hrvTable->field("hrv_value"));

    for (const GarminHrvSample& sample : samples) {
        const qint64 stamp = sample.timestamp.toMSecsSinceEpoch() / 1000;
        // A rescan parses the file again. The value belongs to its timestamp,
        // so the old row for it is replaced, not duplicated.
        if (!m_conn->executeSql(KDbEscapedString("DELETE FROM hrv WHERE hrv_timestamp = %1").arg(stamp))) {
            qDebug() << Q_FUNC_INFO << "Garmin: could not replace hrv value at" << stamp;
            return false;
        }
        QList<QVariant> values;
        values << stamp;
        values << sample.timestamp;
        values << sample.valueMs;
        if (!m_conn->insertRecord(&fields, values)) {
            qDebug() << Q_FUNC_INFO << "Garmin: error inserting hrv value";
            return false;
        }
    }

    tg.commit();
    qDebug() << Q_FUNC_INFO << "Garmin: saved" << samples.size() << "hrv values";
    return true;
}

bool GarminDevice::saveMonitorMessages(const QVector<GarminFitMessage>& messages)
{
    QVector<GarminMonitorSample> samples;
    if (!buildGarminMonitorSamples(messages, samples)) {
        return false;
    }
    bool saved = !samples.isEmpty() && saveMonitorData(samples);

    QVector<GarminSpo2Sample> spo2Samples;
    if (buildGarminSpo2Samples(messages, spo2Samples)) {
        saved = saveSpo2Data(spo2Samples) || saved;
    }

    QVector<GarminStressSample> stressSamples;
    if (buildGarminStressSamples(messages, stressSamples)) {
        saved = saveStressData(stressSamples) || saved;
    }

    QVector<GarminDailyCaloriesSample> caloriesSamples;
    if (buildGarminDailyCaloriesSamples(messages, caloriesSamples)) {
        saved = saveDailyCaloriesData(caloriesSamples) || saved;
    }
    return saved;
}

bool GarminDevice::saveMonitorData(const QVector<GarminMonitorSample>& samples)
{
    if (!m_conn || !m_conn->isDatabaseUsed()) {
        qDebug() << Q_FUNC_INFO << "Garmin: Database not connected";
        return false;
    }
    if (samples.isEmpty()) {
        return false;
    }

    auto activityTable = m_conn->tableSchema("mi_band_activity");
    if (!activityTable) {
        qDebug() << Q_FUNC_INFO << "Garmin: mi_band_activity table missing";
        return false;
    }

    auto config = AmazfishConfig::instance();
    const uint deviceId = qHash(config->pairedAddress());
    const uint userId = qHash(config->profileName());

    KDbTransaction transaction = m_conn->beginTransaction();
    KDbTransactionGuard tg(transaction);

    for (const GarminMonitorSample& sample : samples) {
        const qint64 stamp = sample.timestamp.toMSecsSinceEpoch() / 1000;
        // A rescan parses the file again. The sample belongs to its timestamp,
        // so the old row is replaced, not duplicated. Only the rows of this
        // device: another watch can have a sample at the same second.
        if (!m_conn->executeSql(KDbEscapedString("DELETE FROM mi_band_activity WHERE timestamp = %1 AND device_id = %2")
                                    .arg(stamp)
                                    .arg(deviceId))) {
            qDebug() << Q_FUNC_INFO << "Garmin: could not replace monitor sample at" << stamp;
            return false;
        }
        // A sample carries heart rate, steps, or both. Fields left out of the
        // list stay NULL in the row.
        KDbFieldList fields;
        fields.addField(activityTable->field("timestamp"));
        fields.addField(activityTable->field("timestamp_dt"));
        fields.addField(activityTable->field("device_id"));
        fields.addField(activityTable->field("user_id"));
        if (sample.heartrate >= 0) {
            fields.addField(activityTable->field("heartrate"));
        }
        if (sample.steps >= 0) {
            fields.addField(activityTable->field("steps"));
        }
        QList<QVariant> values;
        values << stamp;
        values << sample.timestamp;
        values << deviceId;
        values << userId;
        if (sample.heartrate >= 0) {
            values << sample.heartrate;
        }
        if (sample.steps >= 0) {
            values << sample.steps;
        }
        if (!m_conn->insertRecord(&fields, values)) {
            qDebug() << Q_FUNC_INFO << "Garmin: error inserting monitor sample";
            return false;
        }
    }

    tg.commit();
    qDebug() << Q_FUNC_INFO << "Garmin: saved" << samples.size() << "monitor samples";
    return true;
}

bool GarminDevice::saveSpo2Data(const QVector<GarminSpo2Sample>& samples)
{
    if (!m_conn || !m_conn->isDatabaseUsed()) {
        qDebug() << Q_FUNC_INFO << "Garmin: Database not connected";
        return false;
    }
    if (samples.isEmpty()) {
        return false;
    }

    auto spo2Table = m_conn->tableSchema("spo2");
    if (!spo2Table) {
        qDebug() << Q_FUNC_INFO << "Garmin: spo2 table missing";
        return false;
    }

    KDbTransaction transaction = m_conn->beginTransaction();
    KDbTransactionGuard tg(transaction);

    for (const GarminSpo2Sample& sample : samples) {
        const qint64 stamp = sample.timestamp.toMSecsSinceEpoch() / 1000;
        // A rescan parses the file again. The reading belongs to its
        // timestamp, so the old row for it is replaced, not duplicated.
        if (!m_conn->executeSql(KDbEscapedString("DELETE FROM spo2 WHERE spo2_timestamp = %1").arg(stamp))) {
            qDebug() << Q_FUNC_INFO << "Garmin: could not replace spo2 reading at" << stamp;
            return false;
        }
        KDbFieldList fields;
        fields.addField(spo2Table->field("spo2_timestamp"));
        fields.addField(spo2Table->field("spo2_timestamp_dt"));
        fields.addField(spo2Table->field("spo2_automatic"));
        fields.addField(spo2Table->field("spo2_value"));

        QList<QVariant> values;
        values << stamp;
        values << sample.timestamp;
        values << sample.automatic;
        values << sample.value;
        if (!m_conn->insertRecord(&fields, values)) {
            qDebug() << Q_FUNC_INFO << "Garmin: error inserting spo2 reading";
            return false;
        }
    }

    tg.commit();
    qDebug() << Q_FUNC_INFO << "Garmin: saved" << samples.size() << "spo2 readings";
    return true;
}

bool GarminDevice::saveStressData(const QVector<GarminStressSample>& samples)
{
    if (!m_conn || !m_conn->isDatabaseUsed()) {
        qDebug() << Q_FUNC_INFO << "Garmin: Database not connected";
        return false;
    }
    if (samples.isEmpty()) {
        return false;
    }

    auto stressTable = m_conn->tableSchema("stress");
    if (!stressTable) {
        qDebug() << Q_FUNC_INFO << "Garmin: stress table missing";
        return false;
    }

    KDbTransaction transaction = m_conn->beginTransaction();
    KDbTransactionGuard tg(transaction);

    for (const GarminStressSample& sample : samples) {
        const qint64 stamp = sample.timestamp.toMSecsSinceEpoch() / 1000;
        // A rescan parses the file again. Replace the old automatic reading
        // for this timestamp. Manual readings (type 2) are never touched.
        if (!m_conn->executeSql(KDbEscapedString("DELETE FROM stress WHERE stress_timestamp = %1 AND stress_type = 1").arg(stamp))) {
            qDebug() << Q_FUNC_INFO << "Garmin: could not replace stress reading at" << stamp;
            return false;
        }
        KDbFieldList fields;
        fields.addField(stressTable->field("stress_timestamp"));
        fields.addField(stressTable->field("stress_timestamp_dt"));
        fields.addField(stressTable->field("stress_value"));
        fields.addField(stressTable->field("stress_type"));

        QList<QVariant> values;
        values << stamp;
        values << sample.timestamp;
        values << sample.value;
        values << 1; // stress_type 1: automatic, what StressAuto filters for
        if (!m_conn->insertRecord(&fields, values)) {
            qDebug() << Q_FUNC_INFO << "Garmin: error inserting stress reading";
            return false;
        }
    }

    tg.commit();
    qDebug() << Q_FUNC_INFO << "Garmin: saved" << samples.size() << "stress readings";
    return true;
}

bool GarminDevice::saveDailyCaloriesData(const QVector<GarminDailyCaloriesSample>& samples)
{
    if (!m_conn || !m_conn->isDatabaseUsed()) {
        qDebug() << Q_FUNC_INFO << "Garmin: Database not connected";
        return false;
    }
    if (samples.isEmpty()) {
        return false;
    }

    auto caloriesTable = m_conn->tableSchema("daily_calories");
    if (!caloriesTable) {
        qDebug() << Q_FUNC_INFO << "Garmin: daily_calories table missing";
        return false;
    }

    KDbTransaction transaction = m_conn->beginTransaction();
    KDbTransactionGuard tg(transaction);

    for (const GarminDailyCaloriesSample& sample : samples) {
        if (sample.activeCalories < 0 && sample.restingMetabolicRate < 0) {
            continue;
        }
        const KDbEscapedString day = m_conn->escapeString(sample.day.toString(Qt::ISODate));
        int rowId = 0;
        const auto foundRow = m_conn->querySingleNumber(
            KDbEscapedString("SELECT daily_calories_id FROM daily_calories WHERE daily_calories_date = %1").arg(day),
            &rowId);
        if (foundRow != true) {
            // No row for this day yet: create it with the parts this file
            // carried. The unique date column keeps a retry from adding a
            // second row.
            KDbFieldList fields;
            QList<QVariant> values;
            fields.addField(caloriesTable->field("daily_calories_date"));
            values << sample.day.toString(Qt::ISODate);
            if (sample.activeCalories >= 0) {
                fields.addField(caloriesTable->field("daily_calories_active"));
                values << sample.activeCalories;
            }
            if (sample.restingMetabolicRate >= 0) {
                fields.addField(caloriesTable->field("daily_calories_rmr"));
                values << sample.restingMetabolicRate;
            }
            if (!m_conn->insertRecord(&fields, values)) {
                qDebug() << Q_FUNC_INFO << "Garmin: error inserting daily calories row";
                return false;
            }
            continue;
        }
        // Files are cut at sync times, so later files know more of the same
        // day. The counters only grow: keep the higher value. The IS NULL arm
        // covers a row created from the basal value alone.
        if (sample.activeCalories >= 0) {
            if (!m_conn->executeSql(KDbEscapedString("UPDATE daily_calories SET daily_calories_active = %2 "
                                                     "WHERE daily_calories_date = %1 AND (daily_calories_active IS NULL "
                                                     "OR daily_calories_active < %2)")
                                        .arg(day)
                                        .arg(sample.activeCalories))) {
                qDebug() << Q_FUNC_INFO << "Garmin: could not update active calories for" << sample.day;
                return false;
            }
        }
        // The basal value only changes with the user profile: the newest file
        // wins.
        if (sample.restingMetabolicRate >= 0) {
            if (!m_conn->executeSql(KDbEscapedString("UPDATE daily_calories SET daily_calories_rmr = %2 "
                                                     "WHERE daily_calories_date = %1")
                                        .arg(day)
                                        .arg(sample.restingMetabolicRate))) {
                qDebug() << Q_FUNC_INFO << "Garmin: could not update resting metabolic rate for" << sample.day;
                return false;
            }
        }
    }

    tg.commit();
    qDebug() << Q_FUNC_INFO << "Garmin: saved" << samples.size() << "daily calories rows";
    return true;
}

bool GarminDevice::saveMetricsMessages(const QVector<GarminFitMessage>& messages)
{
    QVector<GarminVo2maxSample> samples;
    if (!buildGarminVo2maxSamples(messages, samples)) {
        return false;
    }
    return !samples.isEmpty() && saveVo2maxData(samples);
}

bool GarminDevice::saveVo2maxData(const QVector<GarminVo2maxSample>& samples)
{
    if (!m_conn || !m_conn->isDatabaseUsed()) {
        qDebug() << Q_FUNC_INFO << "Garmin: Database not connected";
        return false;
    }
    if (samples.isEmpty()) {
        return false;
    }

    auto vo2maxTable = m_conn->tableSchema("vo2max");
    if (!vo2maxTable) {
        qDebug() << Q_FUNC_INFO << "Garmin: vo2max table missing";
        return false;
    }

    KDbTransaction transaction = m_conn->beginTransaction();
    KDbTransactionGuard tg(transaction);

    for (const GarminVo2maxSample& sample : samples) {
        const qint64 stamp = sample.timestamp.toMSecsSinceEpoch() / 1000;
        // A rescan parses the file again. The reading belongs to its
        // timestamp, so the old row for it is replaced, not duplicated.
        if (!m_conn->executeSql(KDbEscapedString("DELETE FROM vo2max WHERE vo2max_timestamp = %1").arg(stamp))) {
            qDebug() << Q_FUNC_INFO << "Garmin: could not replace vo2max reading at" << stamp;
            return false;
        }
        KDbFieldList fields;
        fields.addField(vo2maxTable->field("vo2max_timestamp"));
        fields.addField(vo2maxTable->field("vo2max_timestamp_dt"));
        fields.addField(vo2maxTable->field("vo2max_value"));

        QList<QVariant> values;
        values << stamp;
        values << sample.timestamp;
        values << sample.value;
        if (!m_conn->insertRecord(&fields, values)) {
            qDebug() << Q_FUNC_INFO << "Garmin: error inserting vo2max reading";
            return false;
        }
    }

    tg.commit();
    qDebug() << Q_FUNC_INFO << "Garmin: saved" << samples.size() << "vo2max readings";
    return true;
}

void GarminDevice::rescanActivityFiles()
{
    if (!mCommunicator) {
        qDebug() << Q_FUNC_INFO << "Garmin: no communicator, nothing to rescan";
        return;
    }

    const QString dir = mCommunicator->garminFileDir();
    if (dir.isEmpty()) {
        return;
    }
    if (!mRescanFiles.isEmpty()) {
        qDebug() << Q_FUNC_INFO << "Garmin: rescan already running";
        return;
    }

    QDirIterator it(dir, QStringList() << QStringLiteral("*.fit"), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        mRescanFiles.append(it.next());
    }
    mRescanTotal = mRescanFiles.size();
    mRescanScanned = 0;
    mRescanSaved = 0;
    qDebug() << Q_FUNC_INFO << "Garmin: rescanning" << mRescanTotal << "files from" << dir;

    // One file per event loop turn. Importing a few hundred files in one go
    // kept the D-Bus call unanswered long enough for the UI to be killed as
    // not responding.
    rescanNextFile();
}

void GarminDevice::rescanNextFile()
{
    if (!mRescanFiles.isEmpty()) {
        const QString path = mRescanFiles.takeFirst();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            qDebug() << Q_FUNC_INFO << "Garmin: cannot read" << path;
        } else {
            const QByteArray bytes = file.readAll();
            file.close();

            QString error;
            QVector<GarminFitMessage> messages;
            if (!parseGarminFitFile(bytes, messages, error)) {
                qDebug() << Q_FUNC_INFO << "Garmin: cannot parse" << path << error;
            } else {
                ++mRescanScanned;
                // Sleep and HRV files hold no session, so the activity save refuses them.
                if (saveActivityMessages(messages)) {
                    ++mRescanSaved;
                } else if (saveSleepMessages(messages)) {
                    ++mRescanSaved;
                } else if (saveHrvMessages(messages)) {
                    ++mRescanSaved;
                } else if (saveMonitorMessages(messages)) {
                    ++mRescanSaved;
                } else if (saveMetricsMessages(messages)) {
                    ++mRescanSaved;
                }
            }
        }
        const int done = mRescanTotal - mRescanFiles.size();
        emit downloadProgress(done * 100 / mRescanTotal);
        QTimer::singleShot(0, this, &GarminDevice::rescanNextFile);
        return;
    }

    qDebug() << Q_FUNC_INFO << "Garmin: rescanned" << mRescanTotal << "files:" << mRescanSaved
             << "saved," << mRescanScanned << "parsed";
    emit message(tr("Rescanned activity files: %1 of %2 saved").arg(mRescanSaved).arg(mRescanScanned));
}

void GarminDevice::refreshInformation()
{

}

QString GarminDevice::information(Amazfish::Info i) const
{

    if (!mCommunicator.data()) {
        qDebug() << Q_FUNC_INFO << "No communicator found!";
        return QString();
    }

    struct deviceInfo info=mCommunicator.data()->deviceInfo();
    switch(i) {
    case Amazfish::Info::INFO_SWVER:
        return info.softwareRevision;
        break;
    case Amazfish::Info::INFO_SERIAL:
        return info.serialNumber;
        break;
    case Amazfish::Info::INFO_BATTERY:
        return QString::number(mCommunicator.data()->batteryLevel());
        break;
    case Amazfish::Info::INFO_MODEL:
        return info.deviceModel;
        break;
    case Amazfish::Info::INFO_MANUFACTURER:
        return info.deviceManufacturer;
    case Amazfish::Info::INFO_STEPS:
        qDebug() << Q_FUNC_INFO << "Steps: " << mCommunicator.data()->steps();
        return QString::number(mCommunicator.data()->steps());
        break;
    case Amazfish::Info::INFO_HEARTRATE:
        qDebug() << Q_FUNC_INFO << "Heart rate: " << mCommunicator.data()->heartRate();
        return QString::number(mCommunicator.data()->heartRate());
        break;
    default:
        return QString("");
    }
    return QString();
}

void GarminDevice::authenticated(bool ready)
{
    qDebug() << Q_FUNC_INFO << ready;

    if (ready) {
        setConnectionState("authenticated");
    } else {
        setConnectionState("authfailed");
    }
}
