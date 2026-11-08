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

#endif // GARMINFITMONITOR_H
