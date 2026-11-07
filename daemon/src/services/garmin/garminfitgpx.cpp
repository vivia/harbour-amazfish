#include "garminfitgpx.h"

#include <QDateTime>
#include <QTextStream>

#include "activitykind.h"

namespace {

// FIT protocol global message numbers used here.
const quint16 FIT_MESSAGE_SESSION = 18;
const quint16 FIT_MESSAGE_RECORD = 20;

// Session field number for the sport, used only for the metadata name.
const quint8 FIT_SESSION_SPORT = 5;

// Record field numbers. Positions are semicircles, altitude is scaled by 5
// with a 500 m offset.
const quint8 FIT_RECORD_POSITION_LAT = 0;
const quint8 FIT_RECORD_POSITION_LONG = 1;
const quint8 FIT_RECORD_HEART_RATE = 3;
const quint8 FIT_RECORD_ENHANCED_ALTITUDE = 78;
const quint8 FIT_RECORD_TIMESTAMP = 253;
const qint64 FIT_ALTITUDE_SCALE = 5;
const qint64 FIT_ALTITUDE_OFFSET = 500;

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

QString garminFitGpxText(const QVector<GarminFitMessage>& messages)
{
    QString str;
    QTextStream out(&str);
    out.setRealNumberPrecision(10);

    out << "<?xml version=\"1.0\" standalone=\"yes\"?>" << endl;
    out << "<?xml-stylesheet type=\"text/xsl\" href=\"details.xsl\"?>" << endl;
    out << "<gpx" << endl;
    out << "version=\"1.1\"" << endl;
    out << "creator=\"Amazfish for SailfishOS\"" << endl;
    out << "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"" << endl;
    out << "xmlns=\"http://www.topografix.com/GPX/1/1\"" << endl;
    out << "xmlns:topografix=\"http://www.topografix.com/GPX/Private/TopoGrafix/0/1\"" << endl;
    out << "xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd http://www.garmin.com/xmlschemas/GpxExtensions/v3 http://www.garmin.com/xmlschemas/GpxExtensionsv3.xsd http://www.garmin.com/xmlschemas/TrackPointExtension/v1 http://www.garmin.com/xmlschemas/TrackPointExtensionv1.xsd\"" << endl;
    out << "xmlns:gpxtpx=\"http://www.garmin.com/xmlschemas/TrackPointExtension/v1\">" << endl;

    // Laufhelden compatible metadata
    QString kind = ActivityKind::toString(ActivityKind::Unknown);
    const GarminFitMessage* session = firstMessage(messages, FIT_MESSAGE_SESSION);
    if (session) {
        const GarminFitFieldValue* sport = validField(*session, FIT_SESSION_SPORT);
        if (sport) {
            kind = ActivityKind::toString(ActivityKind::fromFitSportType(quint8(sport->raw)));
        }
    }

    out << "<metadata>" << endl;
    out << "<name>" << kind << "</name>" << endl;
    out << "<desc></desc>" << endl;
    out << "<extensions>" << endl;
    out << "<meerun activity=\"" << kind.toLower() << "\" />" << endl;
    out << "</extensions>" << endl;
    out << "</metadata>" << endl;

    out << "<trk>" << endl;
    out << "<trkseg>" << endl;

    for (const GarminFitMessage& record : messages) {
        if (record.globalNumber != FIT_MESSAGE_RECORD) {
            continue;
        }

        const GarminFitFieldValue* lat = validField(record, FIT_RECORD_POSITION_LAT);
        const GarminFitFieldValue* lon = validField(record, FIT_RECORD_POSITION_LONG);
        if (!lat || !lon || lat->raw == FIT_POSITION_NO_VALUE || lon->raw == FIT_POSITION_NO_VALUE) {
            continue;
        }

        const double latitude = lat->raw * 180.0 / FIT_SEMICIRCLES_PER_180_DEGREES;
        const double longitude = lon->raw * 180.0 / FIT_SEMICIRCLES_PER_180_DEGREES;

        out << "<trkpt lat=\"" << latitude << "\" lon=\"" << longitude << "\">" << endl;

        const GarminFitFieldValue* altitude = validField(record, FIT_RECORD_ENHANCED_ALTITUDE);
        if (altitude) {
            out << "<ele>" << double(altitude->raw) / FIT_ALTITUDE_SCALE - double(FIT_ALTITUDE_OFFSET) << "</ele>" << endl;
        }

        const GarminFitFieldValue* timestamp = validField(record, FIT_RECORD_TIMESTAMP);
        if (timestamp) {
            const QDateTime time = QDateTime::fromMSecsSinceEpoch((timestamp->raw + FIT_EPOCH_SECONDS) * 1000, Qt::UTC);
            out << "<time>" << time.toTimeSpec(Qt::OffsetFromUTC).toString(Qt::ISODate) << "</time>" << endl;
        }

        const GarminFitFieldValue* heartRate = validField(record, FIT_RECORD_HEART_RATE);
        if (heartRate && heartRate->raw) {
            out << "<extensions><gpxtpx:TrackPointExtension><gpxtpx:hr>" << heartRate->raw << "</gpxtpx:hr></gpxtpx:TrackPointExtension></extensions>" << endl;
        }

        out << "</trkpt>" << endl;
    }

    out << "</trkseg>" << endl;
    out << "</trk>" << endl;
    out << "</gpx>" << endl;

    return str;
}
