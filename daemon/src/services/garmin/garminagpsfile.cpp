#include "garminagpsfile.h"

#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrlQuery>

#include <cstring>

#include <zlib.h>

namespace {

// Gadgetbridge's GarminAgpsDataType: requested constellation name to the
// member file inside the constellations tar.
struct AgpsMember {
    const char* constellation;
    const char* member;
};

const AgpsMember kAgpsMembers[] = {
    { "GPS", "CPE_GPS.BIN" },
    { "GLONASS", "CPE_GLO.BIN" },
    { "GALILEO", "CPE_GAL.BIN" },
    { "QZSS", "CPE_QZSS.BIN" },
};

// ustar layout: name at 0, octal size at 124, magic at 257, 512 byte blocks.
const int kTarNameOffset = 0;
const int kTarNameLength = 100;
const int kTarSizeOffset = 124;
const int kTarSizeLength = 12;
const int kTarMagicOffset = 257;
const QByteArray kTarMagic("ustar\0", 6);
const int kTarBlockSize = 512;

// An rxnetworks file is gzip. Uncompressed it starts with a version, then
// a big endian u32 creation time. Gadgetbridge allows up to 7 days old.
const int kRxNetworksHeaderSize = 6;
const int kMaxRxNetworksAge = 7 * 24 * 60 * 60;

QString memberFor(const QString& constellation)
{
    for (const AgpsMember& entry : kAgpsMembers) {
        if (constellation == QLatin1String(entry.constellation)) {
            return QLatin1String(entry.member);
        }
    }
    return QString();
}

// Inflate the first bytes of a gzip stream. Enough to check a header
// without inflating a several hundred kilobyte file.
bool gunzipHeader(const QByteArray& data, QByteArray& out, int want)
{
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    // 15 + 16 tells zlib to read a gzip wrapper.
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        return false;
    }
    QByteArray buffer(want, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    stream.avail_in = data.size();
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = want;
    // Stopping with the output full returns Z_OK; Z_STREAM_END would mean
    // the stream was shorter than want.
    const int result = inflate(&stream, Z_NO_FLUSH);
    const quint32 produced = stream.total_out;
    inflateEnd(&stream);
    if (result != Z_OK || produced < quint32(want)) {
        return false;
    }
    out = buffer.left(want);
    return true;
}

bool tarHasMember(const QByteArray& tar, const QString& member)
{
    // Same walk as Gadgetbridge's GBTarFile.
    int offset = 0;
    while (tar.size() >= offset + kTarBlockSize
        && tar.mid(offset + kTarMagicOffset, kTarMagic.size()) == kTarMagic) {
        QByteArray name(tar.mid(offset + kTarNameOffset, kTarNameLength));
        const int nameEnd = name.indexOf('\0');
        if (nameEnd >= 0) {
            name.truncate(nameEnd);
        }
        if (QString::fromLatin1(name) == member) {
            return true;
        }

        QByteArray sizeText(tar.mid(offset + kTarSizeOffset, kTarSizeLength));
        const int sizeEnd = sizeText.indexOf('\0');
        if (sizeEnd >= 0) {
            sizeText.truncate(sizeEnd);
        }
        bool ok = false;
        const quint64 size = sizeText.trimmed().toULongLong(&ok, 8);
        if (!ok) {
            return false;
        }
        offset += ((int(size + kTarBlockSize - 1) / kTarBlockSize) + 1) * kTarBlockSize;
    }
    return false;
}

} // namespace

namespace GarminAgpsFile {

Kind kindFor(const QUrl& url)
{
    // Same order as Gadgetbridge: the query parameter wins.
    if (QUrlQuery(url).hasQueryItem(QStringLiteral("constellations"))) {
        return Kind::Constellations;
    }
    if (url.path().contains(QLatin1String("/rxnetworks/"))) {
        return Kind::RxNetworks;
    }
    return Kind::None;
}

QString fileName(Kind kind)
{
    switch (kind) {
    case Kind::RxNetworks:
        return QStringLiteral("7days");
    case Kind::Constellations:
        return QStringLiteral("constellations");
    case Kind::None:
        break;
    }
    return QString();
}

QStringList requestedConstellations(const QUrl& url)
{
    const QString value = QUrlQuery(url).queryItemValue(QStringLiteral("constellations"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    return value.split(QLatin1Char(','), Qt::SkipEmptyParts);
#else
    return value.split(QLatin1Char(','), QString::SkipEmptyParts);
#endif
}

bool isValid(Kind kind, const QByteArray& bytes, const QStringList& constellations)
{
    if (kind == Kind::Constellations) {
        if (bytes.size() < kTarBlockSize || bytes.mid(kTarMagicOffset, kTarMagic.size()) != kTarMagic) {
            qWarning().noquote() << QStringLiteral("Garmin: agps file is not a ustar archive");
            return false;
        }
        for (const QString& constellation : constellations) {
            const QString member = memberFor(constellation);
            if (member.isEmpty()) {
                qWarning().noquote() << QStringLiteral("Garmin: unknown agps data type %1").arg(constellation);
                return false;
            }
            if (!tarHasMember(bytes, member)) {
                qWarning().noquote()
                    << QStringLiteral("Garmin: agps archive is missing %1 for %2").arg(member, constellation);
                return false;
            }
        }
        return true;
    }

    if (kind == Kind::RxNetworks) {
        if (bytes.size() < 2 || quint8(bytes.at(0)) != 0x1f || quint8(bytes.at(1)) != 0x8b) {
            qWarning().noquote() << QStringLiteral("Garmin: agps file is not gzip");
            return false;
        }
        QByteArray header;
        if (!gunzipHeader(bytes, header, kRxNetworksHeaderSize)) {
            qWarning().noquote() << QStringLiteral("Garmin: agps file does not inflate as gzip");
            return false;
        }
        if (quint8(header.at(0)) != 0x01 || quint8(header.at(1)) != 0x00) {
            qWarning().noquote() << QStringLiteral("Garmin: inflated agps header is not rxnetworks: %1")
                                        .arg(QString::fromLatin1(header.toHex()));
            return false;
        }
        const quint32 stamp = (quint8(header.at(2)) << 24) | (quint8(header.at(3)) << 16)
            | (quint8(header.at(4)) << 8) | quint8(header.at(5));
        const qint64 age = QDateTime::currentMSecsSinceEpoch() / 1000 - qint64(stamp);
        if (age < 0) {
            qWarning().noquote() << QStringLiteral("Garmin: agps timestamp %1 is in the future").arg(stamp);
            return false;
        }
        if (age > kMaxRxNetworksAge) {
            qWarning().noquote() << QStringLiteral("Garmin: agps file is %1 s old, more than 7 days").arg(age);
            return false;
        }
        qDebug().noquote() << QStringLiteral("Garmin: agps rxnetworks age: %1 s").arg(age);
        return true;
    }

    return false;
}

QString dirPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/garmin/agps");
}

QString describe(Kind kind)
{
    const QString path = QDir(dirPath()).filePath(fileName(kind));
    QFileInfo info(path);
    if (!info.exists()) {
        return QStringLiteral("missing, place file at %1").arg(path);
    }
    const QString stamp = info.lastModified().toString(QStringLiteral("yyyy-MM-dd hh:mm"));
    return QStringLiteral("%1 KB, updated %2").arg(info.size() / 1024.0, 0, 'f', 0).arg(stamp);
}

} // namespace GarminAgpsFile
