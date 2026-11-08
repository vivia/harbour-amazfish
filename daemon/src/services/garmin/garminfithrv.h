#ifndef GARMINFITHRV_H
#define GARMINFITHRV_H

#include <QDateTime>
#include <QVector>

#include "garminfitreader.h"

// One 5 minute RMSSD reading from a HRV status file.
struct GarminHrvSample {
    QDateTime timestamp;
    int valueMs;
};

bool buildGarminHrvSamples(const QVector<GarminFitMessage>& messages, QVector<GarminHrvSample>& samples);

#endif // GARMINFITHRV_H
