#include "garminfithrv.h"

#include <algorithm>
#include <cstring>

namespace {

// FIT protocol global message numbers of the HRV status records. Both are in
// the published FIT profile; the field numbers were checked against what the
// watch actually writes.
const quint16 FIT_MESSAGE_HRV_STATUS_SUMMARY = 370;
const quint16 FIT_MESSAGE_HRV_VALUE = 371;

const quint8 FIT_HRV_VALUE_TIMESTAMP = 253;
const quint8 FIT_HRV_VALUE_VALUE = 0;

// The FIT profile declares a scale of 128 for the value fields, so the raw
// number on the wire is milliseconds multiplied by 128. The watch also writes
// the fields as uint16, not as the float32 the profile lists.
const double FIT_HRV_VALUE_SCALE = 128.0;

// The reader keeps the IEEE bit pattern of float fields in raw.
const quint8 FIT_BASE_FLOAT32 = 0x88;
const quint8 FIT_BASE_FLOAT64 = 0x89;

// A FIT date_time field counts seconds since the FIT epoch, 1989-12-31 00:00 UTC.
const qint64 FIT_EPOCH_SECONDS = 631065600;

const GarminFitFieldValue* validField(const GarminFitMessage& message, quint8 number)
{
    for (const GarminFitFieldValue& field : message.fields) {
        if (field.number == number && !field.invalid) {
            return &field;
        }
    }
    return nullptr;
}

double fieldValue(const GarminFitFieldValue& field)
{
    if (field.baseType == FIT_BASE_FLOAT32) {
        quint32 bits = quint32(quint64(field.raw));
        float value = 0.0f;
        memcpy(&value, &bits, sizeof(value));
        return double(value);
    }
    quint64 bits = quint64(field.raw);
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

bool buildGarminHrvSamples(const QVector<GarminFitMessage>& messages, QVector<GarminHrvSample>& samples)
{
    samples.clear();

    // An HRV status file always writes the summary record. Activity and sleep
    // files never define it, so the rescan does not mistake those for HRV.
    bool hasSummary = false;
    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber == FIT_MESSAGE_HRV_STATUS_SUMMARY) {
            hasSummary = true;
            break;
        }
    }
    if (!hasSummary) {
        return false;
    }

    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber != FIT_MESSAGE_HRV_VALUE) {
            continue;
        }
        const GarminFitFieldValue* timestamp = validField(message, FIT_HRV_VALUE_TIMESTAMP);
        const GarminFitFieldValue* value = validField(message, FIT_HRV_VALUE_VALUE);
        if (!timestamp || !value) {
            continue;
        }
        const double ms = (value->baseType == FIT_BASE_FLOAT32 || value->baseType == FIT_BASE_FLOAT64)
            ? fieldValue(*value)
            : double(value->raw);
        // Intervals of a night without a reading carry invalid or zero values.
        const int valueMs = qRound(ms / FIT_HRV_VALUE_SCALE);
        if (valueMs <= 0) {
            continue;
        }
        GarminHrvSample sample;
        sample.timestamp = QDateTime::fromMSecsSinceEpoch((timestamp->raw + FIT_EPOCH_SECONDS) * 1000);
        sample.valueMs = valueMs;
        samples.append(sample);
    }

    std::sort(samples.begin(), samples.end(),
        [](const GarminHrvSample& a, const GarminHrvSample& b) { return a.timestamp < b.timestamp; });
    return !samples.isEmpty();
}
