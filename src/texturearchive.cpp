#include "texturearchive.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSharedPointer>
#include <QtEndian>

#include <zlib.h>

namespace TextureArchive {
namespace {

quint16 read16(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size()) return 0;
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

quint32 read32(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

quint64 read64(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 8 > data.size()) return 0;
    return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

void write32(QByteArray& data, qsizetype offset, quint32 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar*>(data.data() + offset));
}

QString canonicalPath(QString path)
{
    path = path.trimmed().replace(u'\\', u'/');
    while (path.startsWith(u'/')) path.remove(0, 1);
    if (path.startsWith(QStringLiteral("textures/"), Qt::CaseInsensitive)) path.remove(0, 9);
    else if (path.startsWith(QStringLiteral("materials/"), Qt::CaseInsensitive)) path.remove(0, 10);
    return path.toLower();
}

struct Chunk {
    quint64 offset = 0;
    quint32 packedSize = 0;
    quint32 unpackedSize = 0;
};

struct Entry {
    QString displayPath;
    bool textureRecord = false;
    quint64 offset = 0;
    quint32 packedSize = 0;
    quint32 unpackedSize = 0;
    quint16 width = 0;
    quint16 height = 0;
    quint8 mipLevels = 1;
    quint8 dxgiFormat = 0;
    bool cubeMap = false;
    QVector<Chunk> chunks;
};

QByteArray inflate(const QByteArray& packed, quint32 expected, QString* error)
{
    QByteArray output(static_cast<qsizetype>(expected), Qt::Uninitialized);
    uLongf size = expected;
    const int result = ::uncompress(reinterpret_cast<Bytef*>(output.data()), &size,
        reinterpret_cast<const Bytef*>(packed.constData()), static_cast<uLong>(packed.size()));
    if (result != Z_OK || size != expected) {
        if (error) *error = QStringLiteral("Could not decompress BA2 texture data (zlib error %1).").arg(result);
        return {};
    }
    return output;
}

QByteArray ddsHeader(const Entry& entry)
{
    QByteArray data(148, '\0');
    data.replace(0, 4, "DDS ");
    write32(data, 4, 124);
    const bool compressed = (entry.dxgiFormat >= 70 && entry.dxgiFormat <= 99);
    write32(data, 8, compressed ? 0x000A1007u : 0x0002100Fu);
    write32(data, 12, entry.height);
    write32(data, 16, entry.width);
    quint32 blockBytes = 0;
    if (entry.dxgiFormat == 71 || entry.dxgiFormat == 72 || entry.dxgiFormat == 80 || entry.dxgiFormat == 81) blockBytes = 8;
    else if (compressed) blockBytes = 16;
    const quint32 pitch = blockBytes
        ? blockBytes * quint32((entry.width + 3) / 4) * quint32((entry.height + 3) / 4)
        : quint32(entry.width) * 4u;
    write32(data, 20, pitch);
    write32(data, 28, qMax(1, int(entry.mipLevels)));
    write32(data, 76, 32);
    write32(data, 80, 0x04);
    write32(data, 84, 0x30315844); // DX10
    write32(data, 108, 0x00401008);
    if (entry.cubeMap) {
        write32(data, 112, 0x0000FE00);
        write32(data, 136, 0x04);
    }
    write32(data, 128, entry.dxgiFormat);
    write32(data, 132, 3); // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    write32(data, 140, 1);
    return data;
}

class Ba2Index {
public:
    bool load(const QString& path, QString* error)
    {
        m_path = QFileInfo(path).absoluteFilePath();
        m_modified = QFileInfo(path).lastModified();
        QFile file(m_path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = file.errorString();
            return false;
        }
        const QByteArray header = file.read(24);
        if (header.size() != 24 || header.first(4) != QByteArrayLiteral("BTDX")) {
            if (error) *error = QStringLiteral("Not a Fallout 4 BA2 archive.");
            return false;
        }
        const quint32 version = read32(header, 4);
        // 1 = Fallout 4 and early Fallout 76; 7/8 = later Fallout 76 updates
        // with the identical table layout, only the number changed.
        if (version != 1 && version != 7 && version != 8) {
            if (error) *error = QStringLiteral("Unsupported BA2 version %1 (Fallout 4/76 archives use 1, 7, or 8).").arg(version);
            return false;
        }
        const QByteArray kind = header.mid(8, 4);
        const quint32 count = read32(header, 12);
        const quint64 namesOffset = read64(header, 16);
        if (count > 2000000u || namesOffset >= quint64(file.size())) {
            if (error) *error = QStringLiteral("Invalid BA2 header or name table.");
            return false;
        }

        QVector<Entry> records;
        records.reserve(int(count));
        if (kind == QByteArrayLiteral("GNRL")) {
            const QByteArray table = file.read(qint64(count) * 36);
            if (table.size() != qsizetype(count) * 36) {
                if (error) *error = QStringLiteral("Truncated BA2 general file table.");
                return false;
            }
            for (quint32 i = 0; i < count; ++i) {
                const qsizetype p = qsizetype(i) * 36;
                Entry entry;
                entry.offset = read64(table, p + 16);
                entry.packedSize = read32(table, p + 24);
                entry.unpackedSize = read32(table, p + 28);
                records.append(entry);
            }
        } else if (kind == QByteArrayLiteral("DX10")) {
            for (quint32 i = 0; i < count; ++i) {
                const QByteArray record = file.read(24);
                if (record.size() != 24) {
                    if (error) *error = QStringLiteral("Truncated BA2 texture table.");
                    return false;
                }
                Entry entry;
                entry.textureRecord = true;
                const int chunkCount = quint8(record[13]);
                entry.height = read16(record, 16);
                entry.width = read16(record, 18);
                entry.mipLevels = quint8(record[20]);
                entry.dxgiFormat = quint8(record[21]);
                entry.cubeMap = (read16(record, 22) & 1u) != 0;
                if (chunkCount <= 0 || chunkCount > 64) {
                    if (error) *error = QStringLiteral("Invalid BA2 texture chunk count.");
                    return false;
                }
                const QByteArray chunks = file.read(chunkCount * 24);
                if (chunks.size() != chunkCount * 24) {
                    if (error) *error = QStringLiteral("Truncated BA2 texture chunks.");
                    return false;
                }
                for (int chunk = 0; chunk < chunkCount; ++chunk) {
                    const qsizetype p = chunk * 24;
                    entry.chunks.append({read64(chunks, p), read32(chunks, p + 8), read32(chunks, p + 12)});
                }
                records.append(entry);
            }
        } else {
            if (error) *error = QStringLiteral("Unsupported BA2 type %1.").arg(QString::fromLatin1(kind));
            return false;
        }

        if (!file.seek(qint64(namesOffset))) {
            if (error) *error = QStringLiteral("Could not seek to the BA2 name table.");
            return false;
        }
        for (quint32 i = 0; i < count; ++i) {
            const QByteArray lengthData = file.read(2);
            if (lengthData.size() != 2) {
                if (error) *error = QStringLiteral("Truncated BA2 name table.");
                return false;
            }
            const quint16 length = read16(lengthData, 0);
            const QByteArray name = file.read(length);
            if (name.size() != length) {
                if (error) *error = QStringLiteral("Truncated BA2 file name.");
                return false;
            }
            QString display = QString::fromUtf8(name).replace(u'\\', u'/');
            while (display.startsWith(u'/')) display.remove(0, 1);
            const bool texture = display.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive);
            // Textures keep their historical "material form" (textures/
            // stripped); everything else keeps the full archive path so
            // callers can mirror the tree.
            if (texture && display.startsWith(QStringLiteral("textures/"), Qt::CaseInsensitive)) display.remove(0, 9);
            records[int(i)].displayPath = display;
            const QString key = canonicalPath(display);
            if (!m_lookup.contains(key)) {
                m_lookup.insert(key, m_entries.size());
                m_entries.append(records[int(i)]);
            }
        }
        return true;
    }

    bool current() const { return QFileInfo(m_path).lastModified() == m_modified; }
    QStringList paths() const
    {
        QStringList result;
        result.reserve(m_entries.size());
        for (const Entry& entry : m_entries)
            if (entry.displayPath.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive))
                result.append(entry.displayPath);
        return result;
    }
    QStringList pathsWithSuffix(const QString& suffix) const
    {
        QStringList result;
        for (const Entry& entry : m_entries)
            if (entry.displayPath.endsWith(suffix, Qt::CaseInsensitive))
                result.append(entry.displayPath);
        return result;
    }
    bool contains(const QString& path) const { return m_lookup.contains(canonicalPath(path)); }

    QByteArray extract(const QString& path, QString* error) const
    {
        const auto it = m_lookup.constFind(canonicalPath(path));
        if (it == m_lookup.cend()) return {};
        const Entry& entry = m_entries[*it];
        QFile file(m_path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = file.errorString();
            return {};
        }
        const auto readBlock = [&](quint64 offset, quint32 packedSize, quint32 unpackedSize) -> QByteArray {
            if (offset > quint64(file.size()) || !file.seek(qint64(offset))) return {};
            const quint32 diskSize = packedSize ? packedSize : unpackedSize;
            const QByteArray bytes = file.read(diskSize);
            if (bytes.size() != diskSize) return {};
            return packedSize ? inflate(bytes, unpackedSize, error) : bytes;
        };
        if (!entry.textureRecord) {
            QByteArray result = readBlock(entry.offset, entry.packedSize, entry.unpackedSize);
            if (result.isEmpty() && entry.unpackedSize && error && error->isEmpty()) *error = QStringLiteral("Could not read BA2 file data.");
            return result;
        }
        QByteArray result = ddsHeader(entry);
        for (const Chunk& chunk : entry.chunks) {
            const QByteArray bytes = readBlock(chunk.offset, chunk.packedSize, chunk.unpackedSize);
            if (bytes.isEmpty() && chunk.unpackedSize) {
                if (error && error->isEmpty()) *error = QStringLiteral("Could not read BA2 texture chunk.");
                return {};
            }
            result.append(bytes);
        }
        return result;
    }

private:
    QString m_path;
    QDateTime m_modified;
    QVector<Entry> m_entries;
    QHash<QString, int> m_lookup;
};

QHash<QString, QSharedPointer<Ba2Index>>& cache()
{
    static QHash<QString, QSharedPointer<Ba2Index>> value;
    return value;
}

QSharedPointer<Ba2Index> indexFor(const QString& archivePath, QString* error)
{
    const QString path = QFileInfo(archivePath).absoluteFilePath();
    auto found = cache().value(path);
    if (found && found->current()) return found;
    auto loaded = QSharedPointer<Ba2Index>::create();
    if (!loaded->load(path, error)) return {};
    cache().insert(path, loaded);
    return loaded;
}

} // namespace

QStringList texturePaths(const QString& archivePath, QString* error)
{
    const auto index = indexFor(archivePath, error);
    return index ? index->paths() : QStringList{};
}

QStringList entryPaths(const QString& archivePath, const QString& suffix, QString* error)
{
    const auto index = indexFor(archivePath, error);
    return index ? index->pathsWithSuffix(suffix) : QStringList{};
}

bool contains(const QString& archivePath, const QString& texturePath)
{
    const auto index = indexFor(archivePath, nullptr);
    return index && index->contains(texturePath);
}

QByteArray extract(const QString& archivePath, const QString& texturePath, QString* error)
{
    const auto index = indexFor(archivePath, error);
    return index ? index->extract(texturePath, error) : QByteArray{};
}

void clearCache() { cache().clear(); }

} // namespace TextureArchive
