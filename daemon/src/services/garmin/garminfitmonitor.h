#ifndef GARMINFITMONITOR_H
#define GARMINFITMONITOR_H

#include <QDateTime>
#include <QVector>

#include "garminfitreader.h"

// One monitoring sample from a MONITOR file: a heart rate reading, the steps
// walked since the previous reading, or both. -1 means the part is absent.
struct GarminMonitorSample {
    QDateTime timestamp;
    int heartrate;
    int steps;
};

bool buildGarminMonitorSamples(const QVector<GarminFitMessage>& messages, QVector<GarminMonitorSample>& samples);

// The calories of one day of the watch: the active calories summed over all
// activity types, and the basal value in kcal per day the watch reports for
// its user. -1 means the file did not carry that part.
struct GarminDailyCaloriesSample {
    QDate day;
    int activeCalories;
    int restingMetabolicRate;
};

bool buildGarminDailyCaloriesSamples(const QVector<GarminFitMessage>& messages,
    QVector<GarminDailyCaloriesSample>& samples);

#endif // GARMINFITMONITOR_H
