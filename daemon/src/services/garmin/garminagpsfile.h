#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

// -----------------------------------------------------------------------------
// Local AGPS ephemeris files served in place of api.gcs.garmin.com,
// following Gadgetbridge's AgpsInterceptor. The user places the files in
// <AppDataLocation>/garmin/agps; names are the constants below.
// -----------------------------------------------------------------------------
namespace GarminAgpsFile {

enum class Kind {
    None,
    RxNetworks,
    Constellations,
};

// Which endpoint a request addresses. Same rules as Gadgetbridge: the
// constellations query parameter wins, then the rxnetworks path.
Kind kindFor(const QUrl& url);

// Local file name for an endpoint, empty for Kind::None.
QString fileName(Kind kind);

// Directory the user places the files in. Single source for that path.
QString dirPath();

// One line status for the debug page: size and mtime, or where to put the file.
QString describe(Kind kind);

// Constellations a request asks for, e.g. GPS, GLONASS, GALILEO.
QStringList requestedConstellations(const QUrl& url);

// Gadgetbridge's sanity checks per file format, to refuse a file that is
// not what the endpoint expects.
bool isValid(Kind kind, const QByteArray& bytes, const QStringList& constellations);

} // namespace GarminAgpsFile
