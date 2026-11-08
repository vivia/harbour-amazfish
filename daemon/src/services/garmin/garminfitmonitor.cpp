#include "garminfitmonitor.h"

#include <QHash>
#include <algorithm>

namespace {

// A FIT date_time field counts seconds since the FIT epoch, 1989-12-31 00:00 UTC.
const qint64 FIT_EPOCH_SECONDS = 631065600;

const quint16 FIT_MESSAGE_MONITORING = 55;

const quint8 FIT_MONITORING_CYCLES = 3;
const quint8 FIT_MONITORING_ACTIVITY_TYPE = 5;
const quint8 FIT_MONITORING_TYPE_INTENSITY = 24;
const quint8 FIT_MONITORING_DURATION_MIN = 29;
const quint8 FIT_MONITORING_HEART_RATE = 27;
const quint8 FIT_MONITORING_TIMESTAMP = 253;
const quint8 FIT_MONITORING_TIMESTAMP16 = 26;

// The short timestamps repeat every 18 hours, so a counter is per activity
// type only within one day of the watch.
const quint64 TIMESTAMP16_MODULO = 65536;
const quint8 TYPE_INTENSITY_TYPE_MASK = 0x1F;

const GarminFitFieldValue* validField(const GarminFitMessage& message, quint8 number)
{
    for (const GarminFitFieldValue& field : message.fields) {
        if (field.number == number && !field.invalid) {
            return &field;
        }
    }
    return nullptr;
}

// The activity type of a record. The snapshots written while watching carry
// it inside field 24, the day-summary records in their own field 5.
int recordActivityType(const GarminFitMessage& message)
{
    const GarminFitFieldValue* typeIntensity = validField(message, FIT_MONITORING_TYPE_INTENSITY);
    if (typeIntensity) {
        return int(typeIntensity->raw) & TYPE_INTENSITY_TYPE_MASK;
    }
    const GarminFitFieldValue* type = validField(message, FIT_MONITORING_ACTIVITY_TYPE);
    return type ? int(type->raw) : -1;
}

} // namespace

bool buildGarminMonitorSamples(const QVector<GarminFitMessage>& messages, QVector<GarminMonitorSample>& samples)
{
    samples.clear();

    // A MONITOR file always writes monitoring records. Activity, sleep and
    // HRV files never define them, so the rescan does not mistake those.
    bool hasMonitoring = false;
    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber == FIT_MESSAGE_MONITORING) {
            hasMonitoring = true;
            break;
        }
    }
    if (!hasMonitoring) {
        return false;
    }

    // Most records only hold a 16 bit second-of-day timestamp. A full
    // timestamp earlier in the file gives the missing high bits: add the
    // forward distance to the short one. Files are cut at sync times, not at
    // midnight, so the first record of a file can be the day-summary record
    // the previous file ended with.
    bool haveAnchor = false;
    qint64 anchor = 0;

    // The steps value is not a per-record count. It is a day counter the
    // watch keeps per activity type, so the steps of a record are the
    // distance its own counter moved since the previous record of that type.
    QHash<int, qint64> lastCycles;

    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber != FIT_MESSAGE_MONITORING) {
            continue;
        }
        const GarminFitFieldValue* timestamp = validField(message, FIT_MONITORING_TIMESTAMP);
        const GarminFitFieldValue* timestamp16 = validField(message, FIT_MONITORING_TIMESTAMP16);
        // A record can carry field 253 set to 0. That is no timestamp at all,
        // so treat it as absent: the anchor from an earlier record or the
        // 16 bit timestamp still place the sample.
        if (timestamp && timestamp->raw != 0) {
            anchor = timestamp->raw;
            haveAnchor = true;
        } else if (timestamp16 && haveAnchor) {
            anchor += (timestamp16->raw - (anchor % qint64(TIMESTAMP16_MODULO)) + qint64(TIMESTAMP16_MODULO))
                % qint64(TIMESTAMP16_MODULO);
        } else {
            continue;
        }

        GarminMonitorSample sample;
        sample.timestamp = QDateTime::fromMSecsSinceEpoch((anchor + FIT_EPOCH_SECONDS) * 1000);
        sample.heartrate = -1;
        sample.steps = -1;

        const GarminFitFieldValue* heartRate = validField(message, FIT_MONITORING_HEART_RATE);
        if (heartRate && heartRate->raw > 0) {
            sample.heartrate = int(heartRate->raw);
        }

        const GarminFitFieldValue* cycles = validField(message, FIT_MONITORING_CYCLES);
        if (cycles) {
            const int type = recordActivityType(message);
            if (!lastCycles.contains(type)) {
                // First record of this type in the file: no previous value to
                // compare with, so it only seeds the counter.
                lastCycles.insert(type, cycles->raw);
            } else {
                const qint64 previous = lastCycles.value(type);
                // A counter below its previous value is the watch starting a
                // new day for that type.
                const qint64 steps = cycles->raw >= previous ? cycles->raw - previous : cycles->raw;
                lastCycles[type] = cycles->raw;
                if (steps > 0) {
                    sample.steps = int(steps);
                }
            }
        }

        if (sample.heartrate >= 0 || sample.steps >= 0) {
            samples.append(sample);
        }
    }

    std::sort(samples.begin(), samples.end(),
        [](const GarminMonitorSample& a, const GarminMonitorSample& b) { return a.timestamp < b.timestamp; });

    // Records with the same resolved timestamp merge into one row: the two
    // day-summary records at the end of a file share their timestamp, and the
    // step counters of different activity types can tick at the same second.
    int merged = 0;
    for (int i = 1; i < samples.size(); ++i) {
        if (samples.at(i).timestamp != samples.at(merged).timestamp) {
            ++merged;
            samples[merged] = samples.at(i);
            continue;
        }
        GarminMonitorSample& into = samples[merged];
        if (samples.at(i).heartrate >= 0) {
            into.heartrate = samples.at(i).heartrate;
        }
        if (samples.at(i).steps >= 0) {
            into.steps = (into.steps >= 0 ? into.steps : 0) + samples.at(i).steps;
        }
    }
    samples.resize(merged + 1);

    return !samples.isEmpty();
}
