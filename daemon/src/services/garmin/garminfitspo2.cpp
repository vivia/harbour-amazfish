#include "garminfitspo2.h"

#include <algorithm>

namespace {

// A FIT date_time field counts seconds since the FIT epoch, 1989-12-31 00:00 UTC.
const qint64 FIT_EPOCH_SECONDS = 631065600;

const quint16 FIT_MESSAGE_SPO2 = 269;

const quint8 FIT_SPO2_READING_SPO2 = 0;
const quint8 FIT_SPO2_MODE = 2;
const quint8 FIT_SPO2_TIMESTAMP = 253;

// spo2_measurement_type in the FIT profile: 1 is a spot check the user
// started. Periodic (3) and continuous (2) readings are the watch measuring
// on its own.
const quint8 FIT_SPO2_MODE_SPOT_CHECK = 1;

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

bool buildGarminSpo2Samples(const QVector<GarminFitMessage>& messages, QVector<GarminSpo2Sample>& samples)
{
    samples.clear();

    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber != FIT_MESSAGE_SPO2) {
            continue;
        }
        const GarminFitFieldValue* reading = validField(message, FIT_SPO2_READING_SPO2);
        const GarminFitFieldValue* timestamp = validField(message, FIT_SPO2_TIMESTAMP);
        if (!reading || !timestamp || timestamp->raw == 0) {
            continue;
        }
        // Real readings are 1 to 100 percent. Zero and the all-0xFF invalid
        // mark are the watch failing to measure.
        if (reading->raw == 0 || reading->raw > 100) {
            continue;
        }

        GarminSpo2Sample sample;
        sample.timestamp = QDateTime::fromMSecsSinceEpoch((timestamp->raw + FIT_EPOCH_SECONDS) * 1000);
        sample.value = int(reading->raw);
        const GarminFitFieldValue* mode = validField(message, FIT_SPO2_MODE);
        sample.automatic = !mode || mode->raw != FIT_SPO2_MODE_SPOT_CHECK;
        samples.append(sample);
    }

    std::sort(samples.begin(), samples.end(),
        [](const GarminSpo2Sample& a, const GarminSpo2Sample& b) { return a.timestamp < b.timestamp; });

    return !samples.isEmpty();
}
