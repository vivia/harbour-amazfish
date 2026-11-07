#ifndef GARMINFITSUMMARY_H
#define GARMINFITSUMMARY_H

#include <QVector>

#include "activitysummary.h"
#include "garminfitreader.h"

// Fills the summary from the session message of a parsed FIT activity file:
// kind, start and end time, base position and the metadata the UI can show.
// Returns false when the messages contain no session.
bool buildGarminFitSummary(const QVector<GarminFitMessage>& messages, ActivitySummary& summary);

#endif // GARMINFITSUMMARY_H
