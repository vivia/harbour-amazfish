#include "garminfitsleep.h"

namespace {

// FIT protocol global message numbers of the sleep records. 21 and 275 are
// from the published FIT profile; 273 and 346 are Garmin-private, their field
// numbers come from what the watches actually write.
const quint16 FIT_MESSAGE_SLEEP_EVENT = 21;
const quint16 FIT_MESSAGE_SLEEP_START = 273;
const quint16 FIT_MESSAGE_SLEEP_LEVEL = 275;
const quint16 FIT_MESSAGE_SLEEP_ASSESSMENT = 346;

const quint8 FIT_SLEEP_EVENT_NUMBER = 0;
const quint8 FIT_SLEEP_EVENT_NUMBER_SLEEP = 74;
const quint8 FIT_SLEEP_EVENT_TIMESTAMP = 253;
const quint8 FIT_SLEEP_START_TIMESTAMP = 253;
const quint8 FIT_SLEEP_LEVEL_TIMESTAMP = 253;
const quint8 FIT_SLEEP_LEVEL_STAGE = 0;
const quint8 FIT_SLEEP_ASSESSMENT_OVERALL_SCORE = 6;

// sleep_level values from the FIT profile.
const qint64 FIT_SLEEP_LEVEL_AWAKE = 1;
const qint64 FIT_SLEEP_LEVEL_LAST = 4;
const char* const FIT_SLEEP_STAGES[] = { "unmeasurable", "awake", "light", "deep", "rem" };

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

const GarminFitMessage* firstMessage(const QVector<GarminFitMessage>& messages, quint16 globalNumber)
{
    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber == globalNumber) {
            return &message;
        }
    }
    return nullptr;
}

QDateTime fitDateTime(const GarminFitFieldValue* field)
{
    return QDateTime::fromMSecsSinceEpoch((field->raw + FIT_EPOCH_SECONDS) * 1000);
}

// Night start from the sleep event record: Garmin writes the published event
// message with its own sleep event number, once when sleep starts and once
// when it ends. The timestamps are UTC, and the stage rows always begin
// after the night, so the earliest one is the night start.
QDateTime sleepEventStart(const QVector<GarminFitMessage>& messages)
{
    QDateTime start;
    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber != FIT_MESSAGE_SLEEP_EVENT) {
            continue;
        }
        const GarminFitFieldValue* number = validField(message, FIT_SLEEP_EVENT_NUMBER);
        const GarminFitFieldValue* timestamp = validField(message, FIT_SLEEP_EVENT_TIMESTAMP);
        if (!number || number->raw != FIT_SLEEP_EVENT_NUMBER_SLEEP || !timestamp) {
            continue;
        }
        const QDateTime eventStart = fitDateTime(timestamp);
        if (!start.isValid() || eventStart < start) {
            start = eventStart;
        }
    }
    return start;
}

} // namespace

bool buildGarminSleepSamples(const QVector<GarminFitMessage>& messages, QVector<GarminSleepSample>& samples)
{
    samples.clear();
    bool hasRealSleep = false;
    for (const GarminFitMessage& message : messages) {
        if (message.globalNumber != FIT_MESSAGE_SLEEP_LEVEL) {
            continue;
        }
        const GarminFitFieldValue* timestamp = validField(message, FIT_SLEEP_LEVEL_TIMESTAMP);
        const GarminFitFieldValue* stage = validField(message, FIT_SLEEP_LEVEL_STAGE);
        if (!timestamp || !stage || stage->raw < 0 || stage->raw > FIT_SLEEP_LEVEL_LAST) {
            continue;
        }
        GarminSleepSample sample;
        sample.timestamp = fitDateTime(timestamp);
        sample.stage = QLatin1String(FIT_SLEEP_STAGES[stage->raw]);
        samples.append(sample);
        if (stage->raw > FIT_SLEEP_LEVEL_AWAKE) {
            hasRealSleep = true;
        }
    }

    // A file that never goes past unmeasurable and awake was not slept in.
    // Storing those rows would make the sleep graph show rest detections.
    if (!hasRealSleep) {
        samples.clear();
        return false;
    }
    return true;
}

bool buildGarminSleepAssessment(const QVector<GarminFitMessage>& messages, QDateTime& start, int& score)
{
    const GarminFitMessage* assessment = firstMessage(messages, FIT_MESSAGE_SLEEP_ASSESSMENT);
    if (!assessment) {
        return false;
    }
    const GarminFitFieldValue* value = validField(*assessment, FIT_SLEEP_ASSESSMENT_OVERALL_SCORE);
    if (!value) {
        return false;
    }

    // Use the sleep event start as the night start. Fall back to the first
    // stage row, then to the sleep_start record, which only carries the time
    // the watch wrote the file and fits a file with a score but no stages.
    start = sleepEventStart(messages);
    if (start.isValid()) {
        score = int(value->raw);
        return true;
    }
    QVector<GarminSleepSample> samples;
    if (buildGarminSleepSamples(messages, samples)) {
        start = samples.first().timestamp;
        for (const GarminSleepSample& sample : samples) {
            if (sample.timestamp < start) {
                start = sample.timestamp;
            }
        }
    } else {
        const GarminFitMessage* sleepStart = firstMessage(messages, FIT_MESSAGE_SLEEP_START);
        const GarminFitFieldValue* timestamp = sleepStart ? validField(*sleepStart, FIT_SLEEP_START_TIMESTAMP) : nullptr;
        if (!timestamp) {
            return false;
        }
        start = fitDateTime(timestamp);
    }

    score = int(value->raw);
    return true;
}
