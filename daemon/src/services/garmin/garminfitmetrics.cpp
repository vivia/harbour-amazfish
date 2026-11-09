#include "garminfitmetrics.h"

#include <algorithm>

namespace {

// A FIT date_time field counts seconds since the FIT epoch, 1989-12-31 00:00 UTC.
const qint64 FIT_EPOCH_SECONDS = 631065600;

const quint16 FIT_MESSAGE_MAX_MET_DATA = 229;

const quint8 FIT_MAX_MET_UPDATE_TIME = 0;
const quint8 FIT_MAX_MET_VO2MAX = 2;

// The FIT profile declares scale 10 for the value, so the number on the wire
// is tenth mL/kg/min.
const double FIT_MAX_MET_SCALE = 10.0;

const double FIT_VO2MAX_MIN = 10.0;
const double FIT_VO2MAX_MAX = 100.0;

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

bool buildGarminVo2maxSamples(const QVector<GarminFitMessage>& messages, QVector<GarminVo2maxSample>& samples)
{
    samples.clear();

    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber != FIT_MESSAGE_MAX_MET_DATA) {
            continue;
        }
        const GarminFitFieldValue* reading = validField(message, FIT_MAX_MET_VO2MAX);
        // max_met_data records have no common timestamp field 253. Field 0,
        // the time the estimate was calculated, is the only way to place one.
        const GarminFitFieldValue* timestamp = validField(message, FIT_MAX_MET_UPDATE_TIME);
        if (!reading || !timestamp || timestamp->raw == 0) {
            continue;
        }
        const double value = double(reading->raw) / FIT_MAX_MET_SCALE;
        if (value < FIT_VO2MAX_MIN || value > FIT_VO2MAX_MAX) {
            continue;
        }

        GarminVo2maxSample sample;
        sample.timestamp = QDateTime::fromMSecsSinceEpoch((timestamp->raw + FIT_EPOCH_SECONDS) * 1000);
        sample.value = value;
        samples.append(sample);
    }

    std::sort(samples.begin(), samples.end(),
        [](const GarminVo2maxSample& a, const GarminVo2maxSample& b) { return a.timestamp < b.timestamp; });

    return !samples.isEmpty();
}
