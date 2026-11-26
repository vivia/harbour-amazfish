#include "garminfilehandler.h"
#include "garminsynchronizationmessage.h"

#include <QDir>
#include <QStandardPaths>

// Upper bound for one classic GFDI download. The root directory is all we ask
// for so far; FIT files need their own bound when the per-file pull is added.
static const qint32 MAX_TRANSFER_BYTES = 1024 * 1024;


GarminFileHandler::GarminFileHandler(CommunicatorV2* com, GarminDevice *dev):mCommunicator(com), mDevice(dev)
{
    QString devName = QFileInfo(mDevice->devicePath()).fileName(); // not really a File, but quickest way to get the device name part
    mStampActivity = devName + "/lastactivityfilestamp";
    mStampSleep = devName + "/lastsleepfilestamp";
    mStampHrv = devName + "/lasthrvfilestamp";
    mStampMonitor = devName + "/lastmonitorfilestamp";
    mStampMetrics = devName + "/lastmetricsfilestamp";
    mFileCategories.append(FileCategory { DIR_TYPE_FIT, DIR_SUBTYPE_ACTIVITY, "activity", mStampActivity, {} });
    mFileCategories.append(FileCategory { DIR_TYPE_FIT, DIR_SUBTYPE_SLEEP, "sleep", mStampSleep, {} });
    mFileCategories.append(FileCategory { DIR_TYPE_FIT, DIR_SUBTYPE_HRV, "hrv", mStampHrv, {} });
    mFileCategories.append(FileCategory { DIR_TYPE_FIT, DIR_SUBTYPE_MONITOR, "monitor", mStampMonitor, {} });
    mFileCategories.append(FileCategory { DIR_TYPE_FIT, DIR_SUBTYPE_METRICS, "metrics", mStampMetrics, {} });
    connect(this, &GarminFileHandler::fileParsed, this, &GarminFileHandler::onFileParsed, Qt::UniqueConnection);

}

bool GarminFileHandler::requestDirectoryListing()
{
    if (mFileTransferState != FileTransferState::Idle) {
        qDebug() << Q_FUNC_INFO << "Garmin: a file transfer is already running, ignoring the request";
        return false;
    }
    // The root directory is fileIndex 0. The watch answers with the size it wants to send.
    mFileTransferState = FileTransferState::WaitingDirStatus;
    if (mCommunicator) mCommunicator->sendMessage("DOWNLOAD REQUEST", GfdiMessageGenerator::downloadRequest(0, 0));
    return true;
}

bool GarminFileHandler::fetchData()
{
    if (mFileTransferState != FileTransferState::Idle) {
        qDebug() << Q_FUNC_INFO << "Garmin: a file transfer is already running, ignoring the request";
        return false;
    }
    // Send a filter message to get all data
    // The watch returns a FilterSTatusMessage which will trigger the directory download in the communicator
    GarminSynchronizationMessage *msg = new GarminSynchronizationMessage(mCommunicator);
    msg->sendFilterMessage(3);
    return true;
}

void GarminFileHandler::onDownloadRequestStatus(quint8 status, quint8 downloadStatus, qint32 fileSize)
{
    qDebug() << Q_FUNC_INFO << "Garmin: download status" << status << downloadStatus << "size" << fileSize;
    const bool forDirectory = mFileTransferState == FileTransferState::WaitingDirStatus;
    if (!forDirectory && mFileTransferState != FileTransferState::WaitingFileStatus) {
        return;
    }
    if (status != quint8(Status::Ack) || downloadStatus != 0) {
        qDebug() << Q_FUNC_INFO << "Garmin: watch refused the download";
        mFileTransferState = FileTransferState::Idle;
        return;
    }
    if (fileSize < 0 || fileSize > MAX_TRANSFER_BYTES) {
        qDebug() << Q_FUNC_INFO << "Garmin: implausible size" << fileSize;
        clearPendingFiles();
        mFileTransferState = FileTransferState::Idle;
        return;
    }
    if (fileSize == 0) {
        qDebug() << Q_FUNC_INFO << "Garmin: watch reports an empty" << (forDirectory ? "directory" : "file");
        clearPendingFiles();
        mFileTransferState = FileTransferState::Idle;
        return;
    }
    QString name = QStringLiteral("directory.bin");
    if (!forDirectory) {
        const DirectoryEntry& entry = mRequestedEntry;
        const QString category = QLatin1String(categoryName(mRequestedCategory));
        name = QStringLiteral("%1-%2-%3.fit").arg(category).arg(entry.fileNumber).arg(entry.timestamp);
    }
    mFileBytes.clear();
    mFileExpectedSize = fileSize;
    mFileRunningCrc = 0;
    openDebugFile(name);
    mFileTransferState = forDirectory ? FileTransferState::ReceivingDir : FileTransferState::ReceivingFile;
}

void GarminFileHandler::onFileTransferDataReceived(const FileTransferDataChunk& chunk)
{
    if (mFileTransferState != FileTransferState::ReceivingDir
        && mFileTransferState != FileTransferState::ReceivingFile) {
        qDebug() << Q_FUNC_INFO << "Garmin: file chunk while not receiving, dropping";
        return;
    }
    if (chunk.dataOffset != qint32(mFileBytes.size())) {
        qDebug() << Q_FUNC_INFO << "Garmin: chunk offset mismatch, expected" << mFileBytes.size()
                 << "got" << chunk.dataOffset;
        mFileOut.close();
        clearPendingFiles();
        mFileTransferState = FileTransferState::Idle;
        mFileBytes.clear();
        return;
    }
    // The crc in the chunk covers every file byte up to and including this chunk.
    const quint16 runningCrc = computeCrc16(mFileRunningCrc, chunk.bytes);
    if (runningCrc != chunk.crc) {
        qDebug() << Q_FUNC_INFO << "Garmin: chunk crc mismatch at offset" << chunk.dataOffset
                 << "computed" << QString::number(runningCrc, 16)
                 << "watch said" << QString::number(chunk.crc, 16);
        mFileOut.close();
        clearPendingFiles();
        mFileTransferState = FileTransferState::Idle;
        mFileBytes.clear();
        return;
    }
    mFileRunningCrc = runningCrc;
    mFileBytes.append(chunk.bytes);
    if (mFileOut.isOpen() && mFileOut.write(chunk.bytes) != chunk.bytes.size()) {
        // A failing debug copy must not abort the transfer itself.
        qDebug() << Q_FUNC_INFO << "Garmin: debug write failed" << mFileOut.errorString();
        mFileOut.close();
    }
    // Status frame for the chunk: echoed message id, ack, transfer status OK, then the number
    // of bytes we have received so far.
    QByteArray r;
    writeU16le(r, quint16(MessageId::FileTransferData));
    r.append(char(Status::Ack)); // Status: ACK
    r.append(char(0x00)); // TransferStatus: OK
    writeU32le(r, quint32(mFileBytes.size()));
    mCommunicator->sendMessage("CHUNK ACK", wrapInGfdiEnvelope(MessageId::Response, r));
    if (mFileBytes.size() < mFileExpectedSize) {
        return;
    }
    mFileOut.close();
    qDebug() << Q_FUNC_INFO << "Garmin: file complete," << mFileBytes.size()
             << "bytes, running crc" << QString::number(mFileRunningCrc, 16);
    if (!mFileTransferPath.isEmpty()) {
        qDebug() << Q_FUNC_INFO << "Garmin: saved" << mFileBytes.size() << "bytes to" << mFileTransferPath;
    }
    const bool wasDirectory = mFileTransferState == FileTransferState::ReceivingDir;
    mFileTransferState = FileTransferState::Idle;
    if (wasDirectory) {
        logDirectoryEntries();
        mFileBytes.clear();
        startFileDownloads();
        return;
    }
    qDebug() << Q_FUNC_INFO << "Garmin: " << categoryName(mRequestedCategory) << "file" << mRequestedEntry.fileIndex
             << (mFileBytes.mid(8, 4) == QByteArrayLiteral(".FIT") ? "starts with .FIT"
                                                                   : "has no .FIT magic");
    QVector<GarminFitMessage> fitMessages;
    QString fitError;
    if (parseGarminFitFile(mFileBytes, fitMessages, fitError)) {
        logGarminFitSummary(fitMessages, mFileBytes.size());
        emit fileParsed(mRequestedEntry.dataType, mRequestedEntry.subType, mRequestedEntry.timestamp, fitMessages);
        if (AmazfishConfig::instance()->deviceGarminDeleteDownloads()) {
            if (mCommunicator) mCommunicator->sendMessage("SET FILE FLAG", GfdiMessageGenerator::setFileFlag(mRequestedEntry.fileIndex, FILE_FLAG_ARCHIVE));
        }
    } else {
        qDebug() << Q_FUNC_INFO << "Garmin: cannot parse FIT file:" << fitError;
    }
    requestNextFile();
    mFileBytes.clear();
}

QString GarminFileHandler::garminFileDir()
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/garmin");
    if (!dir.exists() && !dir.mkpath(".")) {
        qDebug() << Q_FUNC_INFO << "Garmin: cannot create" << dir.path();
        return QString();
    }
    return dir.path();
}

void GarminFileHandler::openDebugFile(const QString& name)
{
    mFileTransferPath.clear();
    if (!AmazfishConfig::instance()->deviceGarminSaveDownloads()) {
        return;
    }
    const QString dir = garminFileDir();
    if (dir.isEmpty()) {
        return;
    }
    mFileTransferPath = dir + "/" + name;
    mFileOut.setFileName(mFileTransferPath);
    if (!mFileOut.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qDebug() << Q_FUNC_INFO << "Garmin: cannot write" << mFileTransferPath << mFileOut.errorString();
        mFileTransferPath.clear();
    }
}

void GarminFileHandler::logDirectoryEntries()
{
    if (mFileBytes.size() % 16 != 0) {
        qDebug() << Q_FUNC_INFO << "Garmin: directory size" << mFileBytes.size() << "is not a multiple of 16";
    }
    mDirectory = parseDirectoryEntries(mFileBytes);
    for (const DirectoryEntry& entry : mDirectory) {
        const QString date = entry.timestamp == 0
            ? QStringLiteral("none")
            : QDateTime::fromMSecsSinceEpoch((qint64(entry.timestamp) + GARMIN_TIME_OFFSET) * 1000)
                  .toString(Qt::ISODate);
        qDebug() << Q_FUNC_INFO << "Garmin: dir entry" << entry.fileIndex << "type" << entry.dataType
                 << "subtype" << entry.subType << "number" << entry.fileNumber << "flags"
                 << QString::number(entry.specificFlags, 16) << QString::number(entry.fileFlags, 16)
                 << "size" << entry.size << "date" << date;
    }
}

const char* GarminFileHandler::categoryName(int index) const
{
    if (index < 0 || index >= mFileCategories.size()) {
        return "unknown";
    }
    return mFileCategories.at(index).name;
}

void GarminFileHandler::clearPendingFiles()
{
    for (FileCategory& category : mFileCategories) {
        category.pending.clear();
    }
}

void GarminFileHandler::startFileDownloads()
{
    // Refill every category queue from the directory listing. Only files
    // newer than the saved stamp are interesting. The config stamps are in
    // seconds on the watch's own epoch, the same unit as the entry timestamp.
    for (FileCategory& category : mFileCategories) {
        category.pending.clear();
        const qint32 lastStamp = AmazfishConfig::instance()->value(category.stampKey).toInt();
        for (const DirectoryEntry& entry : mDirectory) {
            if (entry.dataType == category.dataType && entry.subType == category.subType
                && entry.size > 0 && entry.timestamp > lastStamp) {
                category.pending.append(entry);
            }
        }
        if (category.pending.isEmpty()) {
            qDebug() << Q_FUNC_INFO << "Garmin: watch has no new" << category.name << "file to download";
        }
    }
    requestNextFile();
}

void GarminFileHandler::requestNextFile()
{
    // Categories are served in the order they were registered.
    int categoryIndex = -1;
    for (int c = 0; c < mFileCategories.size(); ++c) {
        if (!mFileCategories.at(c).pending.isEmpty()) {
            categoryIndex = c;
            break;
        }
    }
    if (categoryIndex < 0) {
        return;
    }
    FileCategory& category = mFileCategories[categoryIndex];
    // Oldest first. If the connection breaks mid-queue, the next attempt
    // continues where this one stopped.
    int oldest = 0;
    for (int i = 1; i < category.pending.size(); ++i) {
        if (category.pending.at(i).timestamp < category.pending.at(oldest).timestamp) {
            oldest = i;
        }
    }
    mRequestedEntry = category.pending.at(oldest);
    mRequestedCategory = categoryIndex;
    category.pending.removeAt(oldest);
    qDebug() << Q_FUNC_INFO << "Garmin: requesting" << category.name << "file" << mRequestedEntry.fileIndex
             << "size" << mRequestedEntry.size << "," << category.pending.size() << "to go";
    // The watch answers a download request with its own size, so ask with dataSize 0.
    mFileTransferState = FileTransferState::WaitingFileStatus;
    mCommunicator->sendMessage("DOWNLOAD REQUEST", GfdiMessageGenerator::downloadRequest(mRequestedEntry.fileIndex, 0));
}


void GarminFileHandler::onFileParsed(quint8 type, quint8 subType, qint32 stamp, const QVector<GarminFitMessage>& messages)
{
    // The communicator only queues files newer than the saved stamp. The stamp
    // is in seconds on the watch's own epoch.
    if (type == DIR_TYPE_FIT && subType == DIR_SUBTYPE_SLEEP) {
        QVector<GarminSleepSample> samples;
        QDateTime assessmentStart;
        int score = 0;
        buildGarminSleepSamples(messages, samples);
        buildGarminSleepAssessment(messages, assessmentStart, score);
        if (samples.isEmpty() && !assessmentStart.isValid()) {
            // A rest detection with no stages and no score: nothing to store,
            // but the file is done with. Advance the stamp so it is not
            // fetched again on every sync.
            qDebug() << Q_FUNC_INFO << "Garmin: sleep file has no stages and no score, skipping";
            AmazfishConfig::instance()->setValue(mStampSleep, stamp);
            return;
        }
        if (mDevice->saveSleepData(samples, assessmentStart, score)) {
            AmazfishConfig::instance()->setValue(mStampSleep, stamp);
        }
        return;
    }
    if (type == DIR_TYPE_FIT && subType == DIR_SUBTYPE_HRV) {
        QVector<GarminHrvSample> samples;
        buildGarminHrvSamples(messages, samples);
        if (samples.isEmpty()) {
            // A file with no 5 minute values: nothing to store, but the file
            // is done with. Advance the stamp so it is not fetched again.
            qDebug() << Q_FUNC_INFO << "Garmin: hrv file has no values, skipping";
            AmazfishConfig::instance()->setValue(mStampHrv, stamp);
            return;
        }
        if (mDevice->saveHrvData(samples)) {
            AmazfishConfig::instance()->setValue(mStampHrv, stamp);
        }
        return;
    }
    if (type == DIR_TYPE_FIT && subType == DIR_SUBTYPE_MONITOR) {
        QVector<GarminMonitorSample> samples;
        QVector<GarminSpo2Sample> spo2Samples;
        QVector<GarminStressSample> stressSamples;
        QVector<GarminDailyCaloriesSample> caloriesSamples;
        buildGarminMonitorSamples(messages, samples);
        buildGarminSpo2Samples(messages, spo2Samples);
        buildGarminStressSamples(messages, stressSamples);
        buildGarminDailyCaloriesSamples(messages, caloriesSamples);
        if (samples.isEmpty() && spo2Samples.isEmpty() && stressSamples.isEmpty() && caloriesSamples.isEmpty()) {
            // No heart rate, no step movement, no blood oxygen, no stress and
            // no calories: nothing to store, but the file is done with.
            // Advance the stamp.
            qDebug() << Q_FUNC_INFO << "Garmin: monitor file has no samples, skipping";
            AmazfishConfig::instance()->setValue(mStampMonitor, stamp);
            return;
        }
        bool saved = true;
        if (!samples.isEmpty() && !mDevice->saveMonitorData(samples)) {
            saved = false;
        }
        if (!spo2Samples.isEmpty() && !mDevice->saveSpo2Data(spo2Samples)) {
            saved = false;
        }
        if (!stressSamples.isEmpty() && !mDevice->saveStressData(stressSamples)) {
            saved = false;
        }
        if (!caloriesSamples.isEmpty() && !mDevice->saveDailyCaloriesData(caloriesSamples)) {
            saved = false;
        }
        // Move the stamp only when every domain the file offered is stored.
        // Otherwise the file is fetched again and the rest can still arrive.
        if (saved) {
            AmazfishConfig::instance()->setValue(mStampMonitor, stamp);
        }
        return;
    }
    if (type == DIR_TYPE_FIT && subType == DIR_SUBTYPE_METRICS) {
        QVector<GarminVo2maxSample> samples;
        buildGarminVo2maxSamples(messages, samples);
        if (samples.isEmpty()) {
            // Most metrics files hold other metrics and no VO2max reading.
            // Nothing to store, but the file is done with. Advance the stamp
            // so it is not fetched again.
            qDebug() << Q_FUNC_INFO << "Garmin: metrics file has no vo2max reading, skipping";
            AmazfishConfig::instance()->setValue(mStampMetrics, stamp);
            return;
        }
        if (mDevice->saveVo2maxData(samples)) {
            AmazfishConfig::instance()->setValue(mStampMetrics, stamp);
        }
        return;
    }
    if (mDevice->saveActivityMessages(messages)) {
        AmazfishConfig::instance()->setValue(mStampActivity, stamp);
    }
}

