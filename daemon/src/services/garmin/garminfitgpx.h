#ifndef GARMINFITGPX_H
#define GARMINFITGPX_H

#include <QVector>

#include "garminfitreader.h"

// Builds GPX 1.1 track text from the record messages of a parsed FIT activity
// file, in the same shape Bangle.js writes so the UI viewers understand it.
QString garminFitGpxText(const QVector<GarminFitMessage>& messages);

#endif // GARMINFITGPX_H
