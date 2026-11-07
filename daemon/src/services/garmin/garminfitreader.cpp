#include "garminfitreader.h"
#include "garmintypes.h"

#include <QDebug>
#include <QHash>
#include <QStringList>
#include <QtGlobal>
#include <cstring>

namespace {

// FIT container layout.
const int FIT_HEADER_MIN_SIZE = 12;
const int FIT_HDR_PROTOCOL_OFFSET = 1;
const int FIT_HDR_DATASIZE_OFFSET = 4;
const int FIT_HDR_MAGIC_OFFSET = 8;
const int FIT_FILE_CRC_SIZE = 2;

// Record header byte. The high bit separates normal records (0) from
// compressed timestamp records (1). The corpus has only normal ones.
const quint8 FIT_HEADER_TYPE_BIT = 0x80;
const quint8 FIT_HEADER_DEFINITION_BIT = 0x40;
const quint8 FIT_HEADER_DEV_DATA_BIT = 0x20;
// Four bits, not three: activity files allocate local types up to 13.
const quint8 FIT_HEADER_LOCAL_TYPE_MASK = 0x0F;

// Definition record layout after the header byte.
const int FIT_DEF_RESERVED_OFFSET = 1;
const int FIT_DEF_ENDIAN_OFFSET = 2;
const int FIT_DEF_GLOBAL_OFFSET = 3;
const int FIT_DEF_NUMFIELDS_OFFSET = 5;
const int FIT_DEF_FIELD_ENTRY_SIZE = 3;

// FIT base type ids, from the FIT protocol base type table. A Lily 2 Active
// sends twelve of them; a vivoactive 5 adds float32 and big endian data.
const quint8 FIT_BASE_ENUM = 0x00;
const quint8 FIT_BASE_SINT8 = 0x01;
const quint8 FIT_BASE_UINT8 = 0x02;
const quint8 FIT_BASE_STRING = 0x07;
const quint8 FIT_BASE_UINT8Z = 0x0A;
const quint8 FIT_BASE_BYTE = 0x0D;
const quint8 FIT_BASE_SINT16 = 0x83;
const quint8 FIT_BASE_UINT16 = 0x84;
const quint8 FIT_BASE_SINT32 = 0x85;
const quint8 FIT_BASE_UINT32 = 0x86;
const quint8 FIT_BASE_FLOAT32 = 0x88;
const quint8 FIT_BASE_FLOAT64 = 0x89;
const quint8 FIT_BASE_UINT16Z = 0x8B;
const quint8 FIT_BASE_UINT32Z = 0x8C;

// Width in bytes of a FIT base type, or 0 for a type we do not decode. A
// field of base type x and size n * width(x) is an array of n values. Every
// field of the corpus follows this rule except one: session field 138 with
// size 2 and base uint8, an array of two single byte values.
int baseTypeWidth(quint8 baseType)
{
    switch (baseType) {
    case FIT_BASE_ENUM:
    case FIT_BASE_SINT8:
    case FIT_BASE_UINT8:
    case FIT_BASE_UINT8Z:
    case FIT_BASE_BYTE:
    case FIT_BASE_STRING:
        return 1;
    case FIT_BASE_SINT16:
    case FIT_BASE_UINT16:
    case FIT_BASE_UINT16Z:
        return 2;
    case FIT_BASE_SINT32:
    case FIT_BASE_UINT32:
    case FIT_BASE_UINT32Z:
    case FIT_BASE_FLOAT32:
        return 4;
    case FIT_BASE_FLOAT64:
        return 8;
    }
    return 0;
}

// A local message type as registered by the definition record that precedes
// the data records using it.
struct FitFieldDefinition {
    quint8 number;
    quint8 size;
    quint8 baseType;
};

struct FitLocalDefinition {
    quint16 globalNumber = 0;
    bool littleEndian = true;
    // True when the definition used the 2-byte form, so the data records
    // under this local type also have a 2-byte header.
    bool wideHeader = false;
    QVector<FitFieldDefinition> fields;
};

// Named field of a global message from the FIT profile. Field numbers and
// names only exist to make the log readable, the decoder takes what the
// definition record says. FIT global numbers: file_id 0, session 18, lap 19,
// record 20, activity 34.
struct FitNamedField {
    quint16 globalNumber;
    quint8 number;
    const char* name;
};

const FitNamedField NAMED_FIELDS[] = {
    { 0, 0, "type" }, // file_id
    { 0, 1, "manufacturer" },
    { 0, 2, "product" },
    { 0, 3, "serial_number" },
    { 0, 4, "time_created" },
    { 0, 5, "number" },
    { 0, 8, "product_name" },
    { 18, 3, "start_position_lat" }, // session
    { 18, 4, "start_position_long" },
    { 18, 5, "sport" },
    { 18, 7, "total_elapsed_time" },
    { 18, 8, "total_timer_time" },
    { 18, 9, "total_distance" },
    { 18, 10, "total_cycles" },
    { 18, 11, "total_calories" },
    { 18, 14, "avg_speed" },
    { 18, 15, "max_speed" },
    { 18, 16, "avg_heart_rate" },
    { 18, 17, "max_heart_rate" },
    { 18, 38, "end_position_lat" },
    { 18, 39, "end_position_long" },
    { 18, 110, "sport_profile_name" },
    { 18, 124, "enhanced_avg_speed" },
    { 18, 125, "enhanced_max_speed" },
    { 18, 253, "timestamp" },
    { 19, 2, "start_time" }, // lap
    { 19, 3, "start_position_lat" },
    { 19, 4, "start_position_long" },
    { 19, 5, "end_position_lat" },
    { 19, 6, "end_position_long" },
    { 19, 7, "total_elapsed_time" },
    { 19, 8, "total_timer_time" },
    { 19, 9, "total_distance" },
    { 19, 10, "total_cycles" },
    { 19, 11, "total_calories" },
    { 19, 13, "avg_speed" },
    { 19, 15, "avg_heart_rate" },
    { 19, 16, "max_heart_rate" },
    { 19, 23, "intensity" },
    { 19, 24, "lap_trigger" },
    { 19, 25, "sport" },
    { 19, 253, "timestamp" },
    { 20, 0, "position_lat" }, // record
    { 20, 1, "position_long" },
    { 20, 3, "heart_rate" },
    { 20, 5, "distance" },
    { 20, 73, "enhanced_speed" },
    { 20, 78, "enhanced_altitude" },
    { 20, 253, "timestamp" },
    { 34, 0, "total_timer_time" }, // activity
    { 34, 1, "num_sessions" },
};

const char* namedFieldName(quint16 globalNumber, quint8 fieldNumber)
{
    for (const FitNamedField& named : NAMED_FIELDS) {
        if (named.globalNumber == globalNumber && named.number == fieldNumber) {
            return named.name;
        }
    }
    return 0;
}

const char* namedMessageName(quint16 globalNumber)
{
    switch (globalNumber) {
    case 0:
        return "file_id";
    case 18:
        return "session";
    case 19:
        return "lap";
    case 20:
        return "record";
    case 34:
        return "activity";
    }
    return 0;
}

// FIT marks a field not present with every bit of its value set, so an
// all-0xFF field of any width reads as invalid here. The z types use 0 as
// their invalid value instead; those few fields simply decode as a value.
// Floats use a NaN, and all 0xFF bytes are one, so the check holds there too.
bool fieldIsInvalid(const QByteArray& bytes)
{
    if (bytes.isEmpty()) {
        return true;
    }
    for (int i = 0; i < bytes.size(); ++i) {
        if (quint8(bytes.at(i)) != 0xFF) {
            return false;
        }
    }
    return true;
}

qint64 decodeNumber(const QByteArray& bytes, bool littleEndian, quint8 baseType)
{
    auto byteAt = [&](int i) { return quint8(bytes.at(i)); };
    auto take = [&](int off, int width) {
        quint64 v = 0;
        if (littleEndian) {
            for (int i = width - 1; i >= 0; --i) {
                v = (v << 8) | byteAt(off + i);
            }
        } else {
            for (int i = 0; i < width; ++i) {
                v = (v << 8) | byteAt(off + i);
            }
        }
        return v;
    };

    switch (baseType) {
    case FIT_BASE_SINT8:
        return qint64(qint8(take(0, 1)));
    case FIT_BASE_SINT16:
        return qint64(qint16(take(0, 2)));
    case FIT_BASE_SINT32:
        return qint64(qint32(take(0, 4)));
    case FIT_BASE_UINT16:
    case FIT_BASE_UINT16Z:
        return qint64(take(0, 2));
    case FIT_BASE_UINT32:
    case FIT_BASE_UINT32Z:
    case FIT_BASE_FLOAT32:
    case FIT_BASE_FLOAT64:
        // Keep the bit pattern; valueText reinterprets it as a float.
        return qint64(take(0, int(baseTypeWidth(baseType))));
    }
    // enum, uint8, uint8z, byte: one unsigned byte.
    return qint64(take(0, 1));
}

bool parseDefinition(const QByteArray& file, int offset, QHash<int, FitLocalDefinition>& definitions, QString& errorText)
{
    const quint8 headerByte = quint8(file.at(offset));
    const int numFields = quint8(file.at(offset + FIT_DEF_NUMFIELDS_OFFSET));
    const int entriesEnd = offset + FIT_DEF_NUMFIELDS_OFFSET + 1 + numFields * FIT_DEF_FIELD_ENTRY_SIZE;
    if (entriesEnd > file.size()) {
        errorText = QStringLiteral("definition record at %1 claims %2 fields, file ends early").arg(offset).arg(numFields);
        return false;
    }
    FitLocalDefinition def;
    // The 0x20 bit on a definition header says developer fields follow the
    // normal fields. Every definition in the corpus is the plain 0x40 form.
    def.wideHeader = (headerByte & FIT_HEADER_DEV_DATA_BIT) != 0;
    def.littleEndian = quint8(file.at(offset + FIT_DEF_ENDIAN_OFFSET)) == 0;
    if (quint8(file.at(offset + FIT_DEF_RESERVED_OFFSET)) != 0) {
        errorText = QStringLiteral("definition record at %1 has a non-zero reserved byte").arg(offset);
        return false;
    }
    const quint8 globalLow = quint8(file.at(offset + FIT_DEF_GLOBAL_OFFSET));
    const quint8 globalHigh = quint8(file.at(offset + FIT_DEF_GLOBAL_OFFSET + 1));
    def.globalNumber = def.littleEndian ? quint16(globalLow | (quint16(globalHigh) << 8))
                                        : quint16(globalHigh | (quint16(globalLow) << 8));
    for (int i = 0; i < numFields; ++i) {
        const int at = offset + FIT_DEF_NUMFIELDS_OFFSET + 1 + i * FIT_DEF_FIELD_ENTRY_SIZE;
        FitFieldDefinition field;
        field.number = quint8(file.at(at));
        field.size = quint8(file.at(at + 1));
        field.baseType = quint8(file.at(at + 2));
        if (baseTypeWidth(field.baseType) == 0) {
            errorText = QStringLiteral("definition at %1 uses base type %2 we do not decode")
                            .arg(offset)
                            .arg(field.baseType, 2, 16, QLatin1Char('0'));
            return false;
        }
        def.fields.append(field);
    }
    const int localType = headerByte & FIT_HEADER_LOCAL_TYPE_MASK;
    definitions[localType] = def;
    return true;
}

void parseDataRecord(const QByteArray& file, int headerEnd, const FitLocalDefinition& def, GarminFitMessage& message)
{
    int at = headerEnd;
    for (const FitFieldDefinition& field : def.fields) {
        GarminFitFieldValue value;
        value.number = field.number;
        value.size = field.size;
        value.baseType = field.baseType;
        const QByteArray bytes = file.mid(at, field.size);
        at += field.size;
        value.invalid = fieldIsInvalid(bytes);
        if (field.baseType == FIT_BASE_STRING) {
            int end = bytes.indexOf('\0');
            if (end < 0) {
                end = bytes.size();
            }
            value.text = bytes.left(end);
        } else if (!value.invalid) {
            value.raw = decodeNumber(bytes, def.littleEndian, field.baseType);
        }
        message.fields.append(value);
    }
}

} // namespace

QString GarminFitFieldValue::valueText() const
{
    if (invalid) {
        return QStringLiteral("-");
    }
    if (baseType == FIT_BASE_STRING) {
        return QString::fromLatin1(text);
    }
    if (baseType == FIT_BASE_FLOAT32 || baseType == FIT_BASE_FLOAT64) {
        const int width = baseTypeWidth(baseType);
        double value = 0;
        quint64 bits = quint64(raw);
        if (width == 4) {
            const quint32 bits32 = quint32(bits);
            float f;
            memcpy(&f, &bits32, sizeof(f));
            value = f;
        } else {
            memcpy(&value, &bits, sizeof(value));
        }
        return QString::number(value, 'g', width == 4 ? 9 : 17);
    }
    return QString::number(raw);
}

bool parseGarminFitFile(const QByteArray& file, QVector<GarminFitMessage>& messages, QString& errorText)
{
    messages.clear();
    if (file.size() < FIT_HEADER_MIN_SIZE + FIT_FILE_CRC_SIZE) {
        errorText = QStringLiteral("only %1 bytes, too small for a FIT file").arg(file.size());
        return false;
    }

    const int headerSize = quint8(file.at(0));
    if (headerSize < FIT_HEADER_MIN_SIZE || headerSize > file.size() - FIT_FILE_CRC_SIZE) {
        errorText = QStringLiteral("header size %1 does not fit the file").arg(headerSize);
        return false;
    }
    if (quint8(file.at(FIT_HDR_PROTOCOL_OFFSET)) == 0) {
        errorText = QStringLiteral("protocol version 0");
        return false;
    }
    if (file.mid(FIT_HDR_MAGIC_OFFSET, 4) != QByteArrayLiteral(".FIT")) {
        errorText = QStringLiteral("no .FIT magic at offset %1").arg(FIT_HDR_MAGIC_OFFSET);
        return false;
    }

    quint32 dataSize = quint8(file.at(FIT_HDR_DATASIZE_OFFSET));
    dataSize |= quint32(quint8(file.at(FIT_HDR_DATASIZE_OFFSET + 1))) << 8;
    dataSize |= quint32(quint8(file.at(FIT_HDR_DATASIZE_OFFSET + 2))) << 16;
    dataSize |= quint32(quint8(file.at(FIT_HDR_DATASIZE_OFFSET + 3))) << 24;
    const int recordsEnd = headerSize + int(dataSize);
    if (recordsEnd > file.size() - FIT_FILE_CRC_SIZE) {
        errorText = QStringLiteral("header claims %1 data bytes, only %2 left").arg(dataSize).arg(file.size() - headerSize - FIT_FILE_CRC_SIZE);
        return false;
    }

    // The two bytes after the records are the crc of every byte before them,
    // header included. Same table as the GFDI messages.
    const QByteArray crcSpan = file.left(file.size() - FIT_FILE_CRC_SIZE);
    const quint16 calculatedCrc = computeCrc16(crcSpan);
    const quint16 storedCrc = quint16(quint8(file.at(file.size() - 2)) | (quint16(quint8(file.at(file.size() - 1))) << 8));
    if (calculatedCrc != storedCrc) {
        errorText = QStringLiteral("file crc is 0x%1, the bytes give 0x%2")
                        .arg(storedCrc, 4, 16, QLatin1Char('0'))
                        .arg(calculatedCrc, 4, 16, QLatin1Char('0'));
        return false;
    }

    QHash<int, FitLocalDefinition> definitions;
    int at = headerSize;
    while (at < recordsEnd) {
        const quint8 headerByte = quint8(file.at(at));
        if (headerByte & FIT_HEADER_TYPE_BIT) {
            errorText = QStringLiteral("compressed timestamp header 0x%1 at offset %2, not supported")
                            .arg(headerByte, 2, 16, QLatin1Char('0'))
                            .arg(at);
            return false;
        }
        const int localType = headerByte & FIT_HEADER_LOCAL_TYPE_MASK;
        const bool isDefinition = headerByte & FIT_HEADER_DEFINITION_BIT;

        if (isDefinition) {
            // 2-byte definition: header, reserved, endian, global u16,
            // numFields, then the field entries.
            if (at + FIT_DEF_NUMFIELDS_OFFSET >= recordsEnd) {
                errorText = QStringLiteral("definition at %1 is cut off").arg(at);
                return false;
            }
            if (!parseDefinition(file, at, definitions, errorText)) {
                return false;
            }
            const int numFields = quint8(file.at(at + FIT_DEF_NUMFIELDS_OFFSET));
            at += FIT_DEF_NUMFIELDS_OFFSET + 1 + numFields * FIT_DEF_FIELD_ENTRY_SIZE;
            continue;
        }

        if (headerByte & FIT_HEADER_DEV_DATA_BIT) {
            errorText = QStringLiteral("developer field data at offset %1, not supported").arg(at);
            return false;
        }
        if (!definitions.contains(localType)) {
            errorText = QStringLiteral("data record at %1 uses local type %2 with no definition")
                            .arg(at)
                            .arg(localType);
            return false;
        }
        const FitLocalDefinition def = definitions.value(localType);

        const int headerBytes = def.wideHeader ? 3 : 1;
        if (at + headerBytes > recordsEnd) {
            errorText = QStringLiteral("data record at %1 is cut off").arg(at);
            return false;
        }
        int recordSize = headerBytes;
        for (const FitFieldDefinition& field : def.fields) {
            recordSize += field.size;
        }
        if (at + recordSize > recordsEnd) {
            errorText = QStringLiteral("data record at %1 runs past the end of the records").arg(at);
            return false;
        }
        GarminFitMessage message;
        message.globalNumber = def.globalNumber;
        parseDataRecord(file, at + headerBytes, def, message);
        messages.append(message);
        at += recordSize;
    }
    if (at != recordsEnd) {
        errorText = QStringLiteral("record walk stopped at %1, records end at %2").arg(at).arg(recordsEnd);
        return false;
    }
    return true;
}

void logGarminFitSummary(const QVector<GarminFitMessage>& messages, int fileSize)
{
    // Count per message type, keep the first message of each type to print.
    QVector<quint16> order;
    QHash<quint16, int> counts;
    QHash<quint16, int> firstIndex;
    for (int i = 0; i < messages.size(); ++i) {
        const quint16 globalNumber = messages.at(i).globalNumber;
        if (!counts.contains(globalNumber)) {
            order.append(globalNumber);
            firstIndex[globalNumber] = i;
        }
        counts[globalNumber] = counts.value(globalNumber) + 1;
    }

    QStringList line;
    for (const quint16 globalNumber : order) {
        const char* name = namedMessageName(globalNumber);
        line << QStringLiteral("%1(%2) x%3").arg(name ? QLatin1String(name) : QStringLiteral("global%1").arg(globalNumber)).arg(globalNumber).arg(counts.value(globalNumber));
    }
    qDebug() << "Garmin:"
             << "FIT file of" << fileSize << "bytes," << line.join(", ");

    for (const quint16 globalNumber : order) {
        if (!namedMessageName(globalNumber)) {
            continue;
        }
        QStringList parts;
        const GarminFitMessage& message = messages.at(firstIndex.value(globalNumber));
        for (const GarminFitFieldValue& field : message.fields) {
            const char* name = namedFieldName(globalNumber, field.number);
            parts << QStringLiteral("%1=%2").arg(name ? QLatin1String(name) : QStringLiteral("field%1").arg(field.number)).arg(field.valueText());
        }
        qDebug() << "Garmin:" << namedMessageName(globalNumber) << parts.join(" ");
    }
}
