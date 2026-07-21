#pragma once

#include "materialdocument.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <functional>

// Shared texture caching in front of TextureImage:
// - image(): decoded full-resolution images memoized in memory, so document
//   edits stop re-decoding every texture on each preview rebuild.
// - thumbnails: memory + persistent disk cache, decoded on one worker thread
//   (serialized on purpose — TextureArchive's index cache is not thread-safe).
// Cache keys include file size and mtime, so changed files self-invalidate.
namespace TextureCache {

QImage image(const QString& authoredPath, const QString& textureRoot, int arraySlice = -1);

// Channel-source swaps: slot values may carry swapR/swapG/swapB/swapA objects
// ({path, channel, invert}) redirecting single channels to another texture.
bool hasChannelSwaps(const QJsonObject& values);

// The effective packed image for a slot: the main texture with any swapped
// channels composited in. Falls back to image() when no swaps are authored.
// Array slices never composite (swaps are ignored for landscape arrays).
QImage packedImage(const Pbr::TextureSlotState& state, const QString& textureRoot, int arraySlice = -1);

// Returns the cached thumbnail immediately, or a null image when it is not
// available yet. Never decodes. The state-based overloads reflect channel
// swaps in the thumbnail.
QImage cachedThumbnail(const QString& authoredPath, const QString& textureRoot, const QSize& size);
QImage cachedThumbnail(const Pbr::TextureSlotState& state, const QString& textureRoot, const QSize& size);

// Decodes and scales on the worker thread, stores in the memory + disk caches,
// then invokes the callback on the receiver's thread. The callback is dropped
// if the receiver is destroyed first.
void requestThumbnail(const QString& authoredPath, const QString& textureRoot, const QSize& size,
    QObject* receiver, std::function<void(const QImage&)> callback);
void requestThumbnail(const Pbr::TextureSlotState& state, const QString& textureRoot, const QSize& size,
    QObject* receiver, std::function<void(const QImage&)> callback);

} // namespace TextureCache
