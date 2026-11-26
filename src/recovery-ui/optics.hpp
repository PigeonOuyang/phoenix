// SPDX-License-Identifier: Unlicense
#pragma once

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace p4ui {
// Per-eye native pixels. Diameter includes the whole stroke.
struct OpticsRing {
    std::string id;
    int diameter, stroke, x, y, expires;
    bool operator==(const OpticsRing&) const = default;
};

inline std::optional<OpticsRing> parse_optics(std::string_view line, int eye_size) {
    if (line.ends_with('\n')) line.remove_suffix(1);
    if (line.size() > 96) return {};
    const auto split = line.find(' ');
    const auto id = line.substr(0, split);
    if (split == line.npos || id.size() != 16 ||
        id.find_first_not_of("0123456789abcdef") != line.npos) return {};
    line.remove_prefix(split + 1);
    std::array<int, 5> fields{};
    for (auto& field : fields) {
        const auto end = line.find(' ');
        const auto token = line.substr(0, end);
        if (token.empty() || token.find_first_not_of("0123456789") != token.npos) return {};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), field);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return {};
        line = end == line.npos ? std::string_view{} : line.substr(end + 1);
        if (&field == &fields.back() && end != token.npos) return {};
    }
    const auto [d, s, x, y, expires] = fields;
    if (d < 80 || d > eye_size || s < 4 || s > 128 || s > d / 4 ||
        x < 0 || x > eye_size || y < 0 || y > eye_size || expires <= 0) return {};
    if (d > 2 * x || d > 2 * y || d > 2 * (eye_size - x) || d > 2 * (eye_size - y)) return {};
    return OpticsRing{std::string(id), d, s, x, y, expires};
}
}
