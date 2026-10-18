// SPDX-License-Identifier: MIT
#include "chatview/font_metrics.h"

#include <cstdint>
#include <cstring>

namespace curia::chatview {

namespace {

// The offset table: a 4-byte version, then the number of tables; the table records follow at 12,
// 16 bytes each (tag, checksum, offset, length).
constexpr std::size_t kTableCountAt = 4;
constexpr std::size_t kRecordsAt = 12;
constexpr std::size_t kRecordSize = 16;
// 'head': unitsPerEm at 18. 'hhea': ascender at 4, descender at 6.
constexpr std::size_t kUnitsPerEmAt = 18;
constexpr std::size_t kAscenderAt = 4;
constexpr std::size_t kDescenderAt = 6;
constexpr int kMinUnitsPerEm = 16;
constexpr int kMaxUnitsPerEm = 16384;

std::uint32_t u16(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 8U) | p[1];
}

std::uint32_t u32(const unsigned char* p) {
    return (u16(p) << 16U) | u16(p + 2);
}

int s16(const unsigned char* p) {
    const auto value = static_cast<std::uint16_t>(u16(p));
    return static_cast<std::int16_t>(value);
}

// The table's offset, or nothing when it is missing, reaches past `size` or is shorter than
// `needed` bytes.
std::optional<std::size_t> findTable(const unsigned char* data, std::size_t size, const char* tag,
                                     std::size_t needed) {
    const std::size_t count = u16(data + kTableCountAt);
    if (kRecordsAt + count * kRecordSize > size) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char* record = data + kRecordsAt + i * kRecordSize;
        if (std::memcmp(record, tag, 4) != 0) {
            continue;
        }
        const std::size_t offset = u32(record + 8);
        const std::size_t length = u32(record + 12);
        if (offset > size || length > size - offset || length < needed) {
            return std::nullopt;
        }
        return offset;
    }
    return std::nullopt;
}

}  // namespace

float FontVerticalMetrics::heightPerEm() const {
    return static_cast<float>(ascender - descender) / static_cast<float>(unitsPerEm);
}

std::optional<FontVerticalMetrics> readVerticalMetrics(const unsigned char* data,
                                                       std::size_t size) {
    if (data == nullptr || size < kRecordsAt || std::memcmp(data, "ttcf", 4) == 0) {
        return std::nullopt;
    }
    const std::optional<std::size_t> head = findTable(data, size, "head", kUnitsPerEmAt + 2);
    const std::optional<std::size_t> hhea = findTable(data, size, "hhea", kDescenderAt + 2);
    if (!head.has_value() || !hhea.has_value()) {
        return std::nullopt;
    }
    FontVerticalMetrics metrics;
    metrics.unitsPerEm = static_cast<int>(u16(data + *head + kUnitsPerEmAt));
    metrics.ascender = s16(data + *hhea + kAscenderAt);
    metrics.descender = s16(data + *hhea + kDescenderAt);
    if (metrics.unitsPerEm < kMinUnitsPerEm || metrics.unitsPerEm > kMaxUnitsPerEm ||
        metrics.ascender <= metrics.descender) {
        return std::nullopt;
    }
    return metrics;
}

}  // namespace curia::chatview
