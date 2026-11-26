#ifndef GARMINFILEHANDLER_H
#define GARMINFILEHANDLER_H

#include "amazfishconfig.h"
#include "garmintypes.h"
#include "communicator_v2.h"
#include "garmin/garmindevice.h"

#include <QObject>

class GarminDevice;

// Constants
// =============================================================================

// Directory entry type and subtype of the file categories this service
// downloads. FIT activities, sleep files, HRV status files, monitoring files
// and metrics files are all in directory type 128.
const quint8 DIR_TYPE_FIT = 128;
const quint8 DIR_SUBTYPE_ACTIVITY = 4;
const quint8 DIR_SUBTYPE_SLEEP = 49;
const quint8 DIR_SUBTYPE_HRV = 68;
const quint8 DIR_SUBTYPE_MONITOR = 32;
const quint8 DIR_SUBTYPE_METRICS = 44;

class GarminFileHandler : public QObject
{
    Q_OBJECT

public:
    GarminFileHandler(CommunicatorV2* com, GarminDevice *dev);

    // Ask the watch for its root directory. Everything else in a file transfer follows from the
    // listing, so this is also where a phone-initiated fetch starts.
    // Returns false when a file transfer is already running.
    bool requestDirectoryListing();
    bool fetchData();
    // Directory that debug downloads are saved to. Public so the device can
    // rescan the files it kept.
    QString garminFileDir();


signals:
    // A downloaded file was parsed. fileStamp is the file time from the watch's
    // directory entry, used to remember what was saved already. The type and
    // subtype say which category the file came from.
    void fileParsed(quint8 dataType, quint8 subType, qint32 fileStamp, const QVector<GarminFitMessage>& messages);

public slots:
    void onDownloadRequestStatus(quint8 status, quint8 downloadStatus, qint32 fileSize);
    void onFileParsed(quint8 type, quint8 subType, qint32 stamp, const QVector<GarminFitMessage>& messages);
    void onFileTransferDataReceived(const FileTransferDataChunk& chunk);

private:

    CommunicatorV2* mCommunicator = nullptr;
    GarminDevice* mDevice = nullptr;
    // Classic GFDI file download. Files are buffered in RAM, so a size cap keeps the transfer
    // safe. deviceGarminSaveDownloads keeps a copy of the raw bytes for re-parsing.
    enum class FileTransferState {
        Idle,
        WaitingDirStatus,
        ReceivingDir,
        WaitingFileStatus,
        ReceivingFile
    };
    FileTransferState mFileTransferState = FileTransferState::Idle;
    QByteArray mFileBytes;
    qint32 mFileExpectedSize = 0;
    quint16 mFileRunningCrc = 0;
    QFile mFileOut;
    QString mFileTransferPath;
    QVector<DirectoryEntry> mDirectory;

    // One queue per file category on the watch. A category is one directory
    // type and subtype, with its own config stamp for the last saved file.
    struct FileCategory {
        quint8 dataType;
        quint8 subType;
        const char* name; // used in logs and debug file names
        QString stampKey;
        QVector<DirectoryEntry> pending;
    };
    QVector<FileCategory> mFileCategories;
    DirectoryEntry mRequestedEntry;
    int mRequestedCategory = -1;

    void openDebugFile(const QString& name);
    void logDirectoryEntries();
    const char* categoryName(int index) const;
    void clearPendingFiles();
    void startFileDownloads();
    void requestNextFile();

    // Timestamps for last fetched activies
    // stored per device, name filled in constructor
    QString mStampActivity;
    QString mStampSleep;
    QString mStampHrv;
    QString mStampMonitor;
    QString mStampMetrics;
};

#endif // GARMINFILEHANDLER_H
