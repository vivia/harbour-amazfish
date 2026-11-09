#include "garminfitstress.h"

#include <algorithm>

namespace {

// A FIT date_time field counts seconds since the FIT epoch, 1989-12-31 00:00 UTC.
const qint64 FIT_EPOCH_SECONDS = 631065600;

const quint16 FIT_MESSAGE_STRESS = 227;

const quint8 FIT_STRESS_VALUE = 0;
const quint8 FIT_STRESS_TIMESTAMP = 1;
const quint8 FIT_STRESS_TIMESTAMP_253 = 253;

const GarminFitFieldValue* validField(const GarminFitMessage& message, quint8 number)
{
    for (const GarminFitFieldValue& field : message.fields) {
        if (field.number == number && !field.invalid) {
            return &field;
        }
    }
    return nullptr;
}

} // namespace

bool buildGarminStressSamples(const QVector<GarminFitMessage>& messages, QVector<GarminStressSample>& samples)
{
    samples.clear();

    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber != FIT_MESSAGE_STRESS) {
            continue;
        }
        const GarminFitFieldValue* reading = validField(message, FIT_STRESS_VALUE);
        if (!reading) {
            continue;
        }
        // stress_level records carry their own full timestamp in field 1.
        // Fall back to the common timestamp field 253 if a watch writes that
        // instead. Without either the reading cannot be placed on a timeline.
        const GarminFitFieldValue* timestamp = validField(message, FIT_STRESS_TIMESTAMP);
        if (!timestamp || timestamp->raw == 0) {
            timestamp = validField(message, FIT_STRESS_TIMESTAMP_253);
        }
        if (!timestamp || timestamp->raw == 0) {
            continue;
        }
        // The scale is 0 to 100. Anything outside it, including the all-0xFF
        // invalid mark, is the watch failing to measure.
        if (reading->raw > 100 || reading->raw < 0) {
            continue;
        }

        GarminStressSample sample;
        sample.timestamp = QDateTime::fromMSecsSinceEpoch((timestamp->raw + FIT_EPOCH_SECONDS) * 1000);
        sample.value = int(reading->raw);
        samples.append(sample);
    }

    std::sort(samples.begin(), samples.end(),
        [](const GarminStressSample& a, const GarminStressSample& b) { return a.timestamp < b.timestamp; });

    return !samples.isEmpty();
}
