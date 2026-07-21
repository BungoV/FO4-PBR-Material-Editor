#include "texturecache.h"

#include "textureimage.h"

#include <QCache>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRunnable>
#include <QStandardPaths>
#include <QThreadPool>

namespace TextureCache {

namespace {

QMutex& cacheMutex()
{
    static QMutex mutex;
    return mutex;
}

// Serializes every TextureImage decode: the worker thread and the UI thread
// both funnel through TextureArchive's unguarded index cache.
QMutex& decodeMutex()
{
    static QMutex mutex;
    return mutex;
}

// Cost unit is kilobytes.
QCache<QString, QImage>& imageCache()
{
    static QCache<QString, QImage> cache(256 * 1024);
    return cache;
}

QCache<QString, QImage>& thumbnailCache()
{
    static QCache<QString, QImage> cache(16 * 1024);
    return cache;
}

QThreadPool& workerPool()
{
    static QThreadPool pool;
    static bool configured = false;
    if (!configured) {
        // One worker on purpose: decoding funnels through TextureArchive's
        // in-memory index cache, which is not guarded for concurrent use.
        pool.setMaxThreadCount(1);
        configured = true;
    }
    return pool;
}

QString sourceKey(const QString& authoredPath, const QString& textureRoot)
{
    const QString resolved = TextureImage::resolvePath(authoredPath, textureRoot);
    if (!resolved.isEmpty() && QFileInfo::exists(resolved)) {
        const QFileInfo info(resolved);
        return QDir::cleanPath(resolved).toLower() + QLatin1Char('|')
            + QString::number(info.size()) + QLatin1Char('|')
            + QString::number(info.lastModified().toSecsSinceEpoch());
    }
    // Archived (BA2) sources: key on the authored path plus the newest archive
    // timestamp so repacked archives self-invalidate.
    qint64 newestArchive = 0;
    for (const QString& archive : TextureImage::archivePaths())
        newestArchive = qMax(newestArchive, QFileInfo(archive).lastModified().toSecsSinceEpoch());
    return QStringLiteral("ba2|") + authoredPath.trimmed().toLower() + QLatin1Char('|') + QString::number(newestArchive);
}

int imageCost(const QImage& image)
{
    return qMax(1, int(image.sizeInBytes() / 1024));
}

QString thumbnailDirectory()
{
    static QString directory;
    if (directory.isEmpty()) {
        directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/thumbnails");
        QDir().mkpath(directory);
        // Bound the disk footprint: drop the oldest half once past the budget.
        const QFileInfoList entries = QDir(directory).entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Time | QDir::Reversed);
        qint64 total = 0;
        for (const QFileInfo& entry : entries) total += entry.size();
        if (total > 64 * 1024 * 1024)
            for (int i = 0; i < entries.size() / 2; ++i) QFile::remove(entries[i].absoluteFilePath());
    }
    return directory;
}

const QStringList& swapChannelNames()
{
    static const QStringList names{QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B"), QStringLiteral("A")};
    return names;
}

QString packedSourceKey(const Pbr::TextureSlotState& state, const QString& textureRoot)
{
    if (!hasChannelSwaps(state.values)) return sourceKey(state.path, textureRoot);
    QString key = QStringLiteral("packed|") + sourceKey(state.path, textureRoot);
    for (const QString& channel : swapChannelNames()) {
        const QJsonObject swap = state.values.value(QStringLiteral("swap") + channel).toObject();
        const QString path = swap.value(QStringLiteral("path")).toString().trimmed();
        if (path.isEmpty()) continue;
        key += QLatin1Char('|') + channel + QLatin1Char(':') + sourceKey(path, textureRoot)
            + QLatin1Char(':') + swap.value(QStringLiteral("channel")).toString(QStringLiteral("R"))
            + QLatin1Char(':') + (swap.value(QStringLiteral("invert")).toBool(false) ? QLatin1Char('1') : QLatin1Char('0'));
    }
    return key;
}

QString thumbnailKey(const Pbr::TextureSlotState& state, const QString& textureRoot, const QSize& size)
{
    return packedSourceKey(state, textureRoot) + QLatin1Char('|')
        + QString::number(size.width()) + QLatin1Char('x') + QString::number(size.height());
}

Pbr::TextureSlotState pathOnlyState(const QString& authoredPath)
{
    Pbr::TextureSlotState state;
    state.enabled = true;
    state.path = authoredPath;
    return state;
}

QString thumbnailFilePath(const QString& key)
{
    const QByteArray hash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
    return thumbnailDirectory() + QLatin1Char('/') + QString::fromLatin1(hash) + QStringLiteral(".png");
}

class ThumbnailJob final : public QRunnable {
public:
    ThumbnailJob(Pbr::TextureSlotState state, QString textureRoot, QSize size,
        QObject* receiver, std::function<void(const QImage&)> callback)
        : m_state(std::move(state)), m_textureRoot(std::move(textureRoot)), m_size(size)
        , m_receiver(receiver), m_callback(std::move(callback))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        const QString key = thumbnailKey(m_state, m_textureRoot, m_size);
        QImage thumbnail;
        {
            QMutexLocker locker(&cacheMutex());
            if (const QImage* cached = thumbnailCache().object(key)) thumbnail = *cached;
        }
        if (thumbnail.isNull()) {
            const QString diskPath = thumbnailFilePath(key);
            if (QFileInfo::exists(diskPath)) thumbnail.load(diskPath);
        }
        if (thumbnail.isNull()) {
            const QImage full = packedImage(m_state, m_textureRoot);
            if (!full.isNull()) {
                thumbnail = full.scaled(m_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                thumbnail.save(thumbnailFilePath(key), "PNG");
            }
        }
        if (!thumbnail.isNull()) {
            QMutexLocker locker(&cacheMutex());
            thumbnailCache().insert(key, new QImage(thumbnail), imageCost(thumbnail));
        }
        if (!m_receiver) return;
        QMetaObject::invokeMethod(m_receiver, [callback = std::move(m_callback), thumbnail] {
            callback(thumbnail);
        }, Qt::QueuedConnection);
    }

private:
    Pbr::TextureSlotState m_state;
    QString m_textureRoot;
    QSize m_size;
    QPointer<QObject> m_receiver;
    std::function<void(const QImage&)> m_callback;
};

} // namespace

QImage image(const QString& authoredPath, const QString& textureRoot, int arraySlice)
{
    if (authoredPath.trimmed().isEmpty()) return {};
    const QString key = sourceKey(authoredPath, textureRoot)
        + (arraySlice >= 0 ? QStringLiteral("|slice") + QString::number(arraySlice) : QString());
    {
        QMutexLocker locker(&cacheMutex());
        if (const QImage* cached = imageCache().object(key)) return *cached;
    }
    QImage decoded;
    {
        QMutexLocker decodeLocker(&decodeMutex());
        {
            // Another thread may have decoded it while we waited.
            QMutexLocker locker(&cacheMutex());
            if (const QImage* cached = imageCache().object(key)) return *cached;
        }
        decoded = arraySlice >= 0
            ? TextureImage::loadArraySlice(authoredPath, arraySlice, textureRoot)
            : TextureImage::load(authoredPath, textureRoot);
    }
    if (!decoded.isNull()) {
        QMutexLocker locker(&cacheMutex());
        imageCache().insert(key, new QImage(decoded), imageCost(decoded));
    }
    return decoded;
}

bool hasChannelSwaps(const QJsonObject& values)
{
    for (const QString& channel : swapChannelNames()) {
        const QJsonObject swap = values.value(QStringLiteral("swap") + channel).toObject();
        if (!swap.value(QStringLiteral("path")).toString().trimmed().isEmpty()) return true;
    }
    return false;
}

QImage packedImage(const Pbr::TextureSlotState& state, const QString& textureRoot, int arraySlice)
{
    if (arraySlice >= 0 || !hasChannelSwaps(state.values))
        return image(state.path, textureRoot, arraySlice);

    const QString key = packedSourceKey(state, textureRoot);
    {
        QMutexLocker locker(&cacheMutex());
        if (const QImage* cached = imageCache().object(key)) return *cached;
    }

    QImage base = state.path.trimmed().isEmpty() ? QImage() : image(state.path, textureRoot);
    struct SwapSource { int targetChannel; QImage source; int sourceChannel; bool invert; };
    QVector<SwapSource> swaps;
    QSize dimensions = base.isNull() ? QSize() : base.size();
    for (int target = 0; target < swapChannelNames().size(); ++target) {
        const QJsonObject swap = state.values.value(QStringLiteral("swap") + swapChannelNames()[target]).toObject();
        const QString path = swap.value(QStringLiteral("path")).toString().trimmed();
        if (path.isEmpty()) continue;
        QImage source = image(path, textureRoot);
        if (source.isNull()) continue;
        const QString channelName = swap.value(QStringLiteral("channel")).toString(QStringLiteral("R"));
        const int sourceChannel = channelName == QStringLiteral("G") ? 1
            : channelName == QStringLiteral("B") ? 2
            : channelName == QStringLiteral("A") ? 3
            : channelName == QStringLiteral("Luminance") ? 4 : 0;
        swaps.append({target, std::move(source), sourceChannel, swap.value(QStringLiteral("invert")).toBool(false)});
        if (dimensions.isEmpty()) dimensions = swaps.last().source.size();
    }
    if (dimensions.isEmpty()) return base;

    QImage composite = base.isNull() ? QImage(dimensions, QImage::Format_RGBA8888)
        : base.convertToFormat(QImage::Format_RGBA8888);
    if (base.isNull()) composite.fill(QColor(0, 0, 0, 255));
    for (const SwapSource& swap : swaps) {
        QImage source = swap.source.convertToFormat(QImage::Format_RGBA8888);
        if (source.size() != composite.size())
            source = source.scaled(composite.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (int y = 0; y < composite.height(); ++y) {
            uchar* destination = composite.scanLine(y);
            const uchar* from = source.constScanLine(y);
            for (int x = 0; x < composite.width(); ++x) {
                int value;
                if (swap.sourceChannel == 4)
                    value = (from[x * 4] * 299 + from[x * 4 + 1] * 587 + from[x * 4 + 2] * 114) / 1000;
                else
                    value = from[x * 4 + swap.sourceChannel];
                if (swap.invert) value = 255 - value;
                destination[x * 4 + swap.targetChannel] = uchar(value);
            }
        }
    }
    QMutexLocker locker(&cacheMutex());
    imageCache().insert(key, new QImage(composite), imageCost(composite));
    return composite;
}

QImage cachedThumbnail(const QString& authoredPath, const QString& textureRoot, const QSize& size)
{
    return cachedThumbnail(pathOnlyState(authoredPath), textureRoot, size);
}

QImage cachedThumbnail(const Pbr::TextureSlotState& state, const QString& textureRoot, const QSize& size)
{
    if (state.path.trimmed().isEmpty() && !hasChannelSwaps(state.values)) return {};
    const QString key = thumbnailKey(state, textureRoot, size);
    {
        QMutexLocker locker(&cacheMutex());
        if (const QImage* cached = thumbnailCache().object(key)) return *cached;
    }
    const QString diskPath = thumbnailFilePath(key);
    if (!QFileInfo::exists(diskPath)) return {};
    QImage thumbnail;
    if (!thumbnail.load(diskPath) || thumbnail.isNull()) return {};
    QMutexLocker locker(&cacheMutex());
    thumbnailCache().insert(key, new QImage(thumbnail), imageCost(thumbnail));
    return thumbnail;
}

void requestThumbnail(const QString& authoredPath, const QString& textureRoot, const QSize& size,
    QObject* receiver, std::function<void(const QImage&)> callback)
{
    requestThumbnail(pathOnlyState(authoredPath), textureRoot, size, receiver, std::move(callback));
}

void requestThumbnail(const Pbr::TextureSlotState& state, const QString& textureRoot, const QSize& size,
    QObject* receiver, std::function<void(const QImage&)> callback)
{
    if ((state.path.trimmed().isEmpty() && !hasChannelSwaps(state.values)) || !receiver) return;
    workerPool().start(new ThumbnailJob(state, textureRoot, size, receiver, std::move(callback)));
}

} // namespace TextureCache
