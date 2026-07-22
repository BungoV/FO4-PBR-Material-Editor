#include "normalderive.h"

#include <QtMath>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace NormalDerive {

namespace {

struct VectorField {
    int width = 0, height = 0;
    std::vector<float> x, y; // decoded [-1, 1]
    float& nx(int px, int py) { return x[size_t(py) * width + px]; }
    float& ny(int px, int py) { return y[size_t(py) * width + px]; }
    float nx(int px, int py) const { return x[size_t(py) * width + px]; }
    float ny(int px, int py) const { return y[size_t(py) * width + px]; }
};

VectorField decode(const QImage& normalMap)
{
    const QImage source = normalMap.convertToFormat(QImage::Format_RGBA8888);
    VectorField field;
    field.width = source.width();
    field.height = source.height();
    field.x.resize(size_t(field.width) * field.height);
    field.y.resize(size_t(field.width) * field.height);
    for (int y = 0; y < field.height; ++y) {
        const uchar* line = source.constScanLine(y);
        for (int x = 0; x < field.width; ++x) {
            field.nx(x, y) = line[x * 4] / 255.0f * 2.0f - 1.0f;
            field.ny(x, y) = line[x * 4 + 1] / 255.0f * 2.0f - 1.0f;
        }
    }
    return field;
}

inline int clampIndex(int value, int limit) { return std::clamp(value, 0, limit - 1); }

// Iterative in-place radix-2 complex FFT (power-of-two length).
void fft(std::complex<float>* data, int length, bool inverse)
{
    for (int i = 1, j = 0; i < length; ++i) {
        int bit = length >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (int step = 2; step <= length; step <<= 1) {
        const float angle = (inverse ? 2.0f : -2.0f) * float(M_PI) / step;
        const std::complex<float> twiddle(std::cos(angle), std::sin(angle));
        for (int block = 0; block < length; block += step) {
            std::complex<float> factor(1.0f, 0.0f);
            for (int offset = 0; offset < step / 2; ++offset) {
                const std::complex<float> even = data[block + offset];
                const std::complex<float> odd = data[block + offset + step / 2] * factor;
                data[block + offset] = even + odd;
                data[block + offset + step / 2] = even - odd;
                factor *= twiddle;
            }
        }
    }
    if (inverse)
        for (int i = 0; i < length; ++i) data[i] /= float(length);
}

void fft2d(std::vector<std::complex<float>>& data, int width, int height, bool inverse)
{
    std::vector<std::complex<float>> column(height);
    for (int y = 0; y < height; ++y)
        fft(data.data() + size_t(y) * width, width, inverse);
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) column[y] = data[size_t(y) * width + x];
        fft(column.data(), height, inverse);
        for (int y = 0; y < height; ++y) data[size_t(y) * width + x] = column[y];
    }
}

int nextPowerOfTwo(int value)
{
    int result = 1;
    while (result < value) result <<= 1;
    return result;
}

// Poisson-integrates the slope field into a height field, mirrored boundary
// (even extension makes the periodic FFT solve artifact-free at the seams).
std::vector<float> solveHeight(const VectorField& field)
{
    const int width = field.width, height = field.height;
    const int paddedWidth = nextPowerOfTwo(width * 2);
    const int paddedHeight = nextPowerOfTwo(height * 2);
    std::vector<std::complex<float>> divergence(size_t(paddedWidth) * paddedHeight, {0.0f, 0.0f});
    // Slopes from normals: g = -n_xy / n_z.
    const auto slopeX = [&field](int x, int y) {
        const float nx = field.nx(x, y), ny = field.ny(x, y);
        const float nz = std::sqrt(std::max(0.04f, 1.0f - nx * nx - ny * ny));
        return -nx / nz;
    };
    const auto slopeY = [&field](int x, int y) {
        const float nx = field.nx(x, y), ny = field.ny(x, y);
        const float nz = std::sqrt(std::max(0.04f, 1.0f - nx * nx - ny * ny));
        return -ny / nz;
    };
    const auto mirrored = [](int value, int limit) {
        const int period = 2 * limit;
        int m = ((value % period) + period) % period;
        return m < limit ? m : period - 1 - m;
    };
    for (int y = 0; y < paddedHeight; ++y) {
        const int sourceY = mirrored(y, height);
        for (int x = 0; x < paddedWidth; ++x) {
            const int sourceX = mirrored(x, width);
            const float divX = (slopeX(clampIndex(sourceX + 1, width), sourceY)
                - slopeX(clampIndex(sourceX - 1, width), sourceY)) * 0.5f;
            const float divY = (slopeY(sourceX, clampIndex(sourceY + 1, height))
                - slopeY(sourceX, clampIndex(sourceY - 1, height))) * 0.5f;
            // Mirroring flips slope signs across seams; the even extension of
            // the divergence handles that implicitly because the mirrored
            // SOURCE indices flip the finite-difference direction.
            float value = divX + divY;
            const bool flippedX = (x / width) % 2 == 1;
            const bool flippedY = (y / height) % 2 == 1;
            Q_UNUSED(flippedX); Q_UNUSED(flippedY);
            divergence[size_t(y) * paddedWidth + x] = {value, 0.0f};
        }
    }
    fft2d(divergence, paddedWidth, paddedHeight, false);
    for (int y = 0; y < paddedHeight; ++y) {
        for (int x = 0; x < paddedWidth; ++x) {
            // Discrete Laplacian eigenvalues for exact FD inversion.
            const float lx = 2.0f * std::sin(float(M_PI) * x / paddedWidth);
            const float ly = 2.0f * std::sin(float(M_PI) * y / paddedHeight);
            const float eigen = lx * lx + ly * ly;
            auto& bin = divergence[size_t(y) * paddedWidth + x];
            bin = eigen > 1e-6f ? bin / (-eigen) : std::complex<float>(0.0f, 0.0f);
        }
    }
    fft2d(divergence, paddedWidth, paddedHeight, true);
    std::vector<float> result(size_t(width) * height);
    float minimum = 1e30f, maximum = -1e30f;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float value = divergence[size_t(y) * paddedWidth + x].real();
            result[size_t(y) * width + x] = value;
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
    const float range = std::max(maximum - minimum, 1e-6f);
    for (float& value : result) value = (value - minimum) / range;
    return result;
}

QImage grayscale(const std::vector<float>& values, int width, int height)
{
    QImage image(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        uchar* line = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const uchar value = uchar(std::lround(std::clamp(values[size_t(y) * width + x], 0.0f, 1.0f) * 255.0f));
            line[x * 4] = value;
            line[x * 4 + 1] = value;
            line[x * 4 + 2] = value;
            line[x * 4 + 3] = 255;
        }
    }
    return image;
}

} // namespace

float noiseMetric(const QImage& normalMap)
{
    const VectorField field = decode(normalMap);
    if (field.width < 8 || field.height < 8) return 0.0f;
    double fine = 0.0, coarse = 0.0;
    qint64 samples = 0;
    for (int y = 0; y < field.height; y += 2) {
        for (int x = 0; x + 4 < field.width; x += 2) {
            fine += std::abs(field.nx(x + 1, y) - field.nx(x, y)) + std::abs(field.ny(x + 1, y) - field.ny(x, y));
            coarse += (std::abs(field.nx(x + 4, y) - field.nx(x, y)) + std::abs(field.ny(x + 4, y) - field.ny(x, y))) / 4.0;
            ++samples;
        }
    }
    if (samples == 0) return 0.0f;
    // Structure contributes to both scales; noise concentrates in the fine one.
    return float(std::max(0.0, fine / samples - coarse / samples));
}

QImage denoiseNormal(const QImage& normalMap, const Options& options)
{
    VectorField field = decode(normalMap);
    const int width = field.width, height = field.height;
    if (options.forceMedian) {
        VectorField filtered = field;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                float xs[9], ys[9];
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        xs[count] = field.nx(clampIndex(x + dx, width), clampIndex(y + dy, height));
                        ys[count] = field.ny(clampIndex(x + dx, width), clampIndex(y + dy, height));
                        ++count;
                    }
                std::nth_element(xs, xs + 4, xs + 9);
                std::nth_element(ys, ys + 4, ys + 9);
                filtered.nx(x, y) = xs[4];
                filtered.ny(x, y) = ys[4];
            }
        field = filtered;
    }
    // Edge-preserving bilateral on the vector field: flat regions smooth,
    // real creases survive the range sigma.
    VectorField result = field;
    const float rangeSigma = std::max(options.bilateralRangeSigma, 1e-3f);
    const float twoRange2 = 2.0f * rangeSigma * rangeSigma;
    static const float spatial[3] = {1.0f, 0.6f, 0.15f};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float centerX = field.nx(x, y), centerY = field.ny(x, y);
            float sumX = 0.0f, sumY = 0.0f, weightSum = 0.0f;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const float sampleX = field.nx(clampIndex(x + dx, width), clampIndex(y + dy, height));
                    const float sampleY = field.ny(clampIndex(x + dx, width), clampIndex(y + dy, height));
                    const float dr2 = (sampleX - centerX) * (sampleX - centerX) + (sampleY - centerY) * (sampleY - centerY);
                    const float weight = spatial[std::abs(dx) > std::abs(dy) ? std::abs(dx) : std::abs(dy)]
                        * std::exp(-dr2 / twoRange2);
                    sumX += sampleX * weight;
                    sumY += sampleY * weight;
                    weightSum += weight;
                }
            result.nx(x, y) = sumX / weightSum;
            result.ny(x, y) = sumY / weightSum;
        }
    QImage image(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        uchar* line = image.scanLine(y);
        for (int x = 0; x < width; ++x) {
            line[x * 4] = uchar(std::lround(std::clamp(result.nx(x, y) * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
            line[x * 4 + 1] = uchar(std::lround(std::clamp(result.ny(x, y) * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
            line[x * 4 + 2] = 128;
            line[x * 4 + 3] = 255;
        }
    }
    return image;
}

Results derive(const QImage& normalMap, const Options& options)
{
    Results results;
    if (normalMap.isNull()) return results;
    results.noiseMetric = noiseMetric(normalMap);
    QImage workingMap = normalMap;
    if (options.forceMedian || (options.autoDenoise && results.noiseMetric > options.denoiseThreshold)) {
        Options effective = options;
        // Strong pixel-scale noise gets the median pre-pass as well; the
        // bilateral alone treats large-amplitude dither as edges.
        if (results.noiseMetric > options.denoiseThreshold * 2.0f) effective.forceMedian = true;
        workingMap = denoiseNormal(normalMap, effective);
        results.denoised = true;
    }
    const VectorField field = decode(workingMap);
    const int width = field.width, height = field.height;
    if (width < 4 || height < 4) return results;

    // Signed curvature at full resolution: 0.5 - k * div(n_xy). A pit's
    // normals converge (negative divergence) -> above 0.5 = concave; a dome's
    // diverge -> below 0.5 = convex.
    std::vector<float> curvature(size_t(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float divX = (field.nx(clampIndex(x + 1, width), y) - field.nx(clampIndex(x - 1, width), y)) * 0.5f;
            const float divY = (field.ny(x, clampIndex(y + 1, height)) - field.ny(x, clampIndex(y - 1, height))) * 0.5f;
            curvature[size_t(y) * width + x] = 0.5f - options.curvatureScale * (divX + divY);
        }
    results.curvature = grayscale(curvature, width, height);

    // Height and AO at the capped derivation resolution.
    QImage derivationMap = workingMap;
    if (std::max(width, height) > options.derivationSize)
        derivationMap = workingMap.scaled(
            width >= height ? options.derivationSize : options.derivationSize * width / height,
            height > width ? options.derivationSize : options.derivationSize * height / width,
            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const VectorField derivationField = decode(derivationMap);
    const int dWidth = derivationField.width, dHeight = derivationField.height;
    const std::vector<float> heightField = solveHeight(derivationField);
    results.height = grayscale(heightField, dWidth, dHeight)
        .scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // Horizon-scan detail AO over the reconstructed height field.
    std::vector<float> ao(size_t(dWidth) * dHeight, 1.0f);
    const int directions = std::clamp(options.aoDirections, 2, 32);
    const int radius = std::clamp(options.aoRadius, 2, 128);
    for (int y = 0; y < dHeight; ++y)
        for (int x = 0; x < dWidth; ++x) {
            const float base = heightField[size_t(y) * dWidth + x] * options.heightScale;
            float occlusion = 0.0f;
            for (int direction = 0; direction < directions; ++direction) {
                const float angle = 2.0f * float(M_PI) * direction / directions;
                const float dirX = std::cos(angle), dirY = std::sin(angle);
                float horizon = 0.0f;
                for (int step = 1; step <= radius; ++step) {
                    const int sx = clampIndex(x + int(std::lround(dirX * step)), dWidth);
                    const int sy = clampIndex(y + int(std::lround(dirY * step)), dHeight);
                    const float rise = heightField[size_t(sy) * dWidth + sx] * options.heightScale - base;
                    horizon = std::max(horizon, rise / float(step));
                }
                occlusion += horizon / std::sqrt(1.0f + horizon * horizon); // sin(atan)
            }
            ao[size_t(y) * dWidth + x] = std::clamp(1.0f - options.aoStrength * occlusion / directions, 0.0f, 1.0f);
        }
    results.ao = grayscale(ao, dWidth, dHeight)
        .scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return results;
}

} // namespace NormalDerive
