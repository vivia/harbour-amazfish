#ifndef GARMINFITSLEEP_H
#define GARMINFITSLEEP_H

#include <QDateTime>
#include <QString>
#include <QVector>

#include "garminfitreader.h"

// One stage change of a sleep night: valid from the timestamp until the next
// stage row. The text is one of unmeasurable, awake, light, deep, rem.
struct GarminSleepSample {
    QDateTime timestamp;
    QString stage;
};

// Turns the sleep_level records of a parsed FIT file into stage rows.
// Returns false when the file holds no sleep: no records at all, or only
// unmeasurable and awake ones, which is a rest detection, not sleep.
bool buildGarminSleepSamples(const QVector<GarminFitMessage>& messages, QVector<GarminSleepSample>& samples);

// The overall score of one night from the sleep_assessment record. The record
// carries no time of its own, so start gets the sleep_start time, falling
// back to the earliest stage row. Returns false when the file has no
// assessment record.
bool buildGarminSleepAssessment(const QVector<GarminFitMessage>& messages, QDateTime& start, int& score);

#endif // GARMINFITSLEEP_H
