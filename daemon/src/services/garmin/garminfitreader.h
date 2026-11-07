#ifndef GARMINFITREADER_H
#define GARMINFITREADER_H

#include <QByteArray>
#include <QString>
#include <QVector>

// Reads a FIT file that a watch sent over GFDI file transfer. This first step
// only validates the container, walks the record stream and decodes field
// values by base type. It does not interpret any field yet, so nothing here
// knows what a heart rate or a distance is.
//
// A FIT file is a 14 byte header, record messages, and a CRC-16 over every
// byte before it. Message fields are positional: each definition record lists
// the field numbers, sizes and base types of the data records that follow it
// under a local message type. A record of a global message we do not know is
// still walked, using its definition, so the stream stays in step.

// One decoded field of a FIT message. Numeric fields carry their value in
// raw, text fields in text. Values are not scaled yet.
struct GarminFitFieldValue {
    quint8 number = 0;
    quint8 size = 0;
    quint8 baseType = 0;
    qint64 raw = 0;
    QByteArray text;
    // True when the value bytes are all 0xFF. FIT uses an all-ones value for
    // "field not present", and the watch leaves whole fields set that way.
    bool invalid = false;

    QString valueText() const;
};

// One FIT data message: its global message number and its decoded fields.
struct GarminFitMessage {
    quint16 globalNumber = 0;
    QVector<GarminFitFieldValue> fields;
};

// Parse a whole FIT file into plain messages. On false, errorText says why.
// Unknown messages are kept, so a caller can count them without knowing
// their field numbers.
bool parseGarminFitFile(const QByteArray& file, QVector<GarminFitMessage>& messages, QString& errorText);

// One log line per message type in the file, plus the fields of the first
// message of each known type.
void logGarminFitSummary(const QVector<GarminFitMessage>& messages, int fileSize);

#endif // GARMINFITREADER_H
