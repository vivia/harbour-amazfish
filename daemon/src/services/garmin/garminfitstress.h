#ifndef GARMINFITSTRESS_H
#define GARMINFITSTRESS_H

#include <QDateTime>
#include <QVector>

#include "garminfitreader.h"

// One stress reading from a MONITOR file. Garmin measures these on its own,
// so every stored row is an automatic reading.
struct GarminStressSample {
    QDateTime timestamp;
    int value;
};

bool buildGarminStressSamples(const QVector<GarminFitMessage>& messages, QVector<GarminStressSample>& samples);

#endif // GARMINFITSTRESS_H
