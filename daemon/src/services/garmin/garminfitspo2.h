#ifndef GARMINFITSPO2_H
#define GARMINFITSPO2_H

#include <QDateTime>
#include <QVector>

#include "garminfitreader.h"

// One blood oxygen reading from a MONITOR file. Spot checks are not
// automatic, everything else the watch measured on its own is.
struct GarminSpo2Sample {
    QDateTime timestamp;
    int value;
    bool automatic;
};

bool buildGarminSpo2Samples(const QVector<GarminFitMessage>& messages, QVector<GarminSpo2Sample>& samples);

#endif // GARMINFITSPO2_H
