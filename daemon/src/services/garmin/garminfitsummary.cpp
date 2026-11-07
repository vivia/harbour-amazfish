#include "garminfitsummary.h"

#include <QDateTime>
#include <QDebug>

namespace {

// FIT protocol global message numbers used here.
const quint16 FIT_MESSAGE_SESSION = 18;
const quint16 FIT_MESSAGE_RECORD = 20;

// Session field numbers and their scales from the FIT activity profiles.
const quint8 FIT_SESSION_POSITION_LAT = 3;
const quint8 FIT_SESSION_POSITION_LONG = 4;
const quint8 FIT_SESSION_SPORT = 5;
const quint8 FIT_SESSION_TOTAL_ELAPSED_TIME = 7;
const quint8 FIT_SESSION_TOTAL_TIMER_TIME = 8;
const quint8 FIT_SESSION_TOTAL_DISTANCE = 9;
const quint8 FIT_SESSION_TOTAL_CALORIES = 11;
const quint8 FIT_SESSION_AVG_HEART_RATE = 16;
const quint8 FIT_SESSION_MAX_HEART_RATE = 17;
const quint8 FIT_SESSION_TIMESTAMP = 253;

// Record field number for altitude; scaled by 5 with a 500 m offset.
const quint8 FIT_RECORD_ENHANCED_ALTITUDE = 78;
const qint64 FIT_ALTITUDE_SCALE = 5;
const qint64 FIT_ALTITUDE_OFFSET = 500;

// Time fields carry milliseconds, distance fields centimeters.
const qint64 FIT_MILLISECOND_SCALE = 1000;
const qint64 FIT_DISTANCE_SCALE = 100;

// A FIT date_time field counts seconds since the FIT epoch, 1989-12-31 00:00 UTC.
const qint64 FIT_EPOCH_SECONDS = 631065600;

// FIT positions are semicircles: 2^31 of them make 180 degrees.
const double FIT_SEMICIRCLES_PER_180_DEGREES = 2147483648.0;

// Watches write this for positions when there was no GPS fix. It is not the
// all-0xFF invalid marker, so the reader passes it through.
const qint64 FIT_POSITION_NO_VALUE = 0x7FFFFFFF;

const GarminFitFieldValue* validField(const GarminFitMessage& message, quint8 number)
{
    for (const GarminFitFieldValue& field : message.fields) {
        if (field.number == number && !field.invalid) {
            return &field;
        }
    }
    return nullptr;
}

const GarminFitMessage* firstMessage(const QVector<GarminFitMessage>& messages, quint16 globalNumber)
{
    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber == globalNumber) {
            return &message;
        }
    }
    return nullptr;
}

} // namespace

bool buildGarminFitSummary(const QVector<GarminFitMessage>& messages, ActivitySummary& summary)
{
    const GarminFitMessage* session = firstMessage(messages, FIT_MESSAGE_SESSION);
    if (!session) {
        return false;
    }

    const GarminFitFieldValue* sport = validField(*session, FIT_SESSION_SPORT);
    if (sport) {
        summary.setActivityKind(ActivityKind::fromFitSportType(quint8(sport->raw)));
    }

    // The session timestamp is the end of the activity; elapsed time reaches back to its start.
    const GarminFitFieldValue* timestamp = validField(*session, FIT_SESSION_TIMESTAMP);
    if (timestamp) {
        const QDateTime end = QDateTime::fromMSecsSinceEpoch((timestamp->raw + FIT_EPOCH_SECONDS) * 1000);
        summary.setEndTime(end);
        const GarminFitFieldValue* elapsed = validField(*session, FIT_SESSION_TOTAL_ELAPSED_TIME);
        if (elapsed) {
            summary.setStartTime(end.addMSecs(-elapsed->raw));
        } else {
            summary.setStartTime(end);
        }
    }

    const GarminFitFieldValue* lat = validField(*session, FIT_SESSION_POSITION_LAT);
    const GarminFitFieldValue* lon = validField(*session, FIT_SESSION_POSITION_LONG);
    if (lat && lon && lat->raw != FIT_POSITION_NO_VALUE && lon->raw != FIT_POSITION_NO_VALUE) {
        summary.setBaseLatitude(float(lat->raw * 180.0 / FIT_SEMICIRCLES_PER_180_DEGREES));
        summary.setBaseLongitude(float(lon->raw * 180.0 / FIT_SEMICIRCLES_PER_180_DEGREES));
    }

    // No altitude in the session, so take it from the first record that has one.
    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber != FIT_MESSAGE_RECORD) {
            continue;
        }
        const GarminFitFieldValue* altitude = validField(message, FIT_RECORD_ENHANCED_ALTITUDE);
        if (altitude) {
            summary.setBaseAltitude(int(altitude->raw / FIT_ALTITUDE_SCALE - FIT_ALTITUDE_OFFSET));
            break;
        }
    }

    struct MetaSpec {
        quint8 fieldNumber;
        const char* key;
        const char* unit;
        qint64 scale;
    };
    // Keys and units match what the UI already renders for other platforms.
    const MetaSpec specs[] = {
        { FIT_SESSION_TOTAL_TIMER_TIME, "activeSeconds", "seconds", FIT_MILLISECOND_SCALE },
        { FIT_SESSION_TOTAL_CALORIES, "caloriesBurnt", "calories_unit", 1 },
        { FIT_SESSION_TOTAL_DISTANCE, "distanceMeters", "meters", FIT_DISTANCE_SCALE },
        { FIT_SESSION_AVG_HEART_RATE, "averageHR", "bpm", 1 },
        { FIT_SESSION_MAX_HEART_RATE, "maxHR", "bpm", 1 },
    };
    for (const MetaSpec& spec : specs) {
        const GarminFitFieldValue* value = validField(*session, spec.fieldNumber);
        if (value) {
            const QString number = QString::number(value->raw / spec.scale);
            summary.addMetaData(QLatin1String(spec.key), number, QLatin1String(spec.unit));
        }
    }

    // The name is the identity of the activity in the database and in the GPX
    // file name, so it must be unique. Second precision keeps two activities
    // that start within the same minute apart.
    const QString stamp = summary.startTime().toLocalTime().toString("yyyyMMdd-HHmmss");
    summary.setName((ActivityKind::toString(summary.activityKind())) + "-" + stamp);
    summary.setValid(true);
    return true;
}
