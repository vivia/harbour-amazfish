#ifndef GARMINFITMETRICS_H
#define GARMINFITMETRICS_H

#include <QDateTime>
#include <QVector>

#include "garminfitreader.h"

// One VO2 max estimate from a METRICS file. The watch recalculates it only
// after a hard enough activity, so there is rarely more than one per week.
struct GarminVo2maxSample {
    QDateTime timestamp;
    double value;
};

bool buildGarminVo2maxSamples(const QVector<GarminFitMessage>& messages, QVector<GarminVo2maxSample>& samples);

#endif // GARMINFITMETRICS_H
