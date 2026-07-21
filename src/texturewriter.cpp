#include "texturewriter.h"

#define RGBCX_IMPLEMENTATION
#include "rgbcx.h"
#include "bc7enc.h"

#include <QDataStream>
#include <QSaveFile>

#include <cstring>

namespace TextureWriter {

namespace {

bool isBlockCompressed(DdsFormat format)
{
    return format != DdsFormat::Rgba8 && format != DdsFormat::Rgba8Srgb;
}

quint32 dxgiFormat(DdsFormat format)
{
    switch (format) {
    case DdsFormat::Bc1: return 71;       // DXGI_FORMAT_BC1_UNORM
    case DdsFormat::Bc4: return 80;       // DXGI_FORMAT_BC4_UNORM
    case DdsFormat::Bc5: return 83;       // DXGI_FORMAT_BC5_UNORM
    case DdsFormat::Bc7: return 98;       // DXGI_FORMAT_BC7_UNORM
    case DdsFormat::Bc7Srgb: return 99;   // DXGI_FORMAT_BC7_UNORM_SRGB
    case DdsFormat::Rgba8: return 28;     // DXGI_FORMAT_R8G8B8A8_UNORM
    case DdsFormat::Rgba8Srgb: return 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    }
    return 28;
}

int bytesPerBlock(DdsFormat format)
{
    switch (format) {
    case DdsFormat::Bc1:
    case DdsFormat::Bc4: return 8;
    default: return 16;
    }
}

void encodersInitialized()
{
    static bool initialized = false;
    if (initialized) return;
    rgbcx::init();
    bc7enc_compress_block_init();
    initialized = true;
}

QByteArray encodeMipLevel(const QImage& mip, DdsFormat format)
{
    const QImage source = mip.convertToFormat(QImage::Format_RGBA8888);
    if (!isBlockCompressed(format)) {
        QByteArray data;
        data.reserve(source.width() * source.height() * 4);
        for (int y = 0; y < source.height(); ++y)
            data.append(reinterpret_cast<const char*>(source.constScanLine(y)), source.width() * 4);
        return data;
    }

    bc7enc_compress_block_params bc7Params;
    bc7enc_compress_block_params_init(&bc7Params);

    const int blocksX = (source.width() + 3) / 4;
    const int blocksY = (source.height() + 3) / 4;
    QByteArray data(blocksX * blocksY * bytesPerBlock(format), Qt::Uninitialized);
    quint8 blockPixels[16 * 4];
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            // Gather a 4x4 block, clamping at the edges.
            for (int py = 0; py < 4; ++py) {
                const int y = qMin(by * 4 + py, source.height() - 1);
                const uchar* line = source.constScanLine(y);
                for (int px = 0; px < 4; ++px) {
                    const int x = qMin(bx * 4 + px, source.width() - 1);
                    std::memcpy(blockPixels + (py * 4 + px) * 4, line + x * 4, 4);
                }
            }
            char* block = data.data() + (by * blocksX + bx) * bytesPerBlock(format);
            switch (format) {
            case DdsFormat::Bc1:
                rgbcx::encode_bc1(rgbcx::MAX_LEVEL, block, blockPixels, false, false);
                break;
            case DdsFormat::Bc4:
                rgbcx::encode_bc4(block, blockPixels, 4);
                break;
            case DdsFormat::Bc5:
                rgbcx::encode_bc5(block, blockPixels, 0, 1, 4);
                break;
            case DdsFormat::Bc7:
            case DdsFormat::Bc7Srgb:
                bc7enc_compress_block(block, blockPixels, &bc7Params);
                break;
            default:
                break;
            }
        }
    }
    return data;
}

} // namespace

bool saveDds(const QImage& image, const QString& filePath, DdsFormat format,
    bool generateMips, QString* error)
{
    if (image.isNull()) {
        if (error) *error = QStringLiteral("There is no composed image to export.");
        return false;
    }
    encodersInitialized();

    QVector<QImage> mips;
    mips.append(image.convertToFormat(QImage::Format_RGBA8888));
    if (generateMips) {
        while (mips.last().width() > 1 || mips.last().height() > 1) {
            const QImage& previous = mips.last();
            mips.append(previous.scaled(qMax(1, previous.width() / 2), qMax(1, previous.height() / 2),
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888));
        }
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);

    const quint32 width = quint32(image.width()), height = quint32(image.height());
    const bool compressed = isBlockCompressed(format);
    const quint32 pitchOrLinear = compressed
        ? quint32(((width + 3) / 4) * ((height + 3) / 4) * bytesPerBlock(format))
        : width * 4;
    // DDSD_CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT + pitch kind. Compressed
    // formats use a DX10 extension header; uncompressed RGBA uses the legacy
    // masked pixel format for the broadest reader compatibility (the sRGB
    // distinction is a metadata-only loss there).
    quint32 flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (compressed ? 0x80000 : 0x8);
    stream << quint32(0x20534444);                       // 'DDS '
    stream << quint32(124) << flags << height << width << pitchOrLinear;
    stream << quint32(0) << quint32(mips.size());        // depth, mipMapCount
    for (int i = 0; i < 11; ++i) stream << quint32(0);   // reserved
    if (compressed) {
        stream << quint32(32) << quint32(0x4);           // ddspf size, DDPF_FOURCC
        stream << quint32(0x30315844);                   // 'DX10'
        for (int i = 0; i < 5; ++i) stream << quint32(0);
    } else {
        stream << quint32(32) << quint32(0x40 | 0x1);    // DDPF_RGB|DDPF_ALPHAPIXELS
        stream << quint32(0) << quint32(32);             // no fourCC, 32bpp
        stream << quint32(0x000000FF) << quint32(0x0000FF00) << quint32(0x00FF0000) << quint32(0xFF000000);
    }
    stream << quint32(0x1000 | 0x400000 | 0x8);          // caps: TEXTURE|MIPMAP|COMPLEX
    stream << quint32(0) << quint32(0) << quint32(0) << quint32(0);
    if (compressed)
        stream << dxgiFormat(format) << quint32(3) << quint32(0) << quint32(1) << quint32(0); // DX10 block

    for (const QImage& mip : mips) {
        const QByteArray data = encodeMipLevel(mip, format);
        if (file.write(data) != data.size()) {
            if (error) *error = file.errorString();
            return false;
        }
    }
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace TextureWriter
