// SPDX-License-Identifier: Unlicense
#include "render-check.hpp"
#include "fixture-backend.hpp"
#include "preview-painter.hpp"

#include "progress_layout.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace p4ui::preview {
namespace {

using namespace progress_ui;

void check(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}

void check_layout() {
    for (const auto count : {7U, 9U}) {
        const auto equal = main_rail(count, count);
        const auto expanded = main_rail(count, 2);
        check(equal.size() == count && expanded.size() == count && expanded[2].w == expanded_width &&
              equal[2].w == equal[0].w && expanded[2].w > equal[2].w &&
              std::abs(expanded.back().x + expanded.back().w - (main_left + main_width)) < .001,
              "Main rail keeps all stages inside its approved width");
    }
    const std::array<unsigned char, 5> five{0, 1, 0, 1, 0};
    const std::array<unsigned char, 6> six{0, 1, 0, 1, 0, 1};
    const auto anchored = child_layout(five, center - 160);
    const auto clamped = child_layout(five, main_left);
    const auto centered = child_layout(six, main_left);
    check(anchored.rows.size() == 1 && anchored.x == center - 160 &&
          clamped.rows.size() == 1 && clamped.x > main_left && centered.x == center,
          "Five substeps anchor to their parent; six align to the eye center");
    for (const auto& item : {anchored, clamped, centered})
        for (const auto& row : item.rows)
            check(2 * std::abs(item.x - center) + row.w <= chord(row.y) + .001 &&
                  row.status >= min_status_width, "Substep row exceeds the concentric bound");
}

} // namespace

void verify_native_render(const Catalog& catalog, const std::string& fonts, const std::string& atlas,
                          const std::string& png) {
    check_layout();
    set_fixture_locale("en");
    App app(true);
    app.depth.zero = true;
    set_fixture_telemetry(app.backend.telemetry);
    constexpr int pitch = canvas_width * 4;
    std::vector<unsigned char> first(static_cast<std::size_t>(pitch) * eye_size), second(first.size());
    PreviewPainter painter(fonts, atlas, app);
    auto scene = [&](const Route& route, double at, std::string_view locale) {
        set_fixture_locale(locale);
        app.backend.preferences = app.backend.load_ui_preferences();
        const auto frame = replay(catalog, route, at, locale == "zh-CN");
        set_fixture_logs(frame.logs);
        app.backend.task = frame.task;
        app.log_layout.sampled_at = 0;
        app.compose();
    };
    auto deck = [&] {
        Animation motion;
        std::vector<Node> nodes;
        draw_log_deck(app, motion, [&](auto label, double size, bool bold) {
            return painter.text_width(label, size, bold);
        }, nodes);
        return nodes;
    };
    auto contains = [](const std::vector<Node>& nodes, std::string_view expected) {
        return std::any_of(nodes.begin(), nodes.end(), [&](const auto& n) {
            return n.kind == Kind::text && n.value.find(expected) != std::string::npos;
        });
    };

    app.reduced_motion = true;
    for (auto mode : {"host", "headset"}) {
        const auto& route = catalog.routes[catalog.find(mode, "observed")];
        for (auto locale : {"en", "zh-CN"}) {
            scene(route, route.phase_step("super", "flush").sample(), locale);
            const auto nodes = deck();
            check(contains(nodes, locale == std::string_view("en") ? "Total unknown" : "总量未知"),
                  "Unknown work must retain its localized status in the native deck");
            const auto labels = std::count_if(nodes.begin(), nodes.end(), [](const auto& n) {
                return n.kind == Kind::text && n.y == main_axis + 13 * scale;
            });
            check(labels == static_cast<std::ptrdiff_t>(route.stages.size() * 2),
                  "The native rail must retain every numbered main-stage label");
        }
    }
    const auto& host = catalog.routes[catalog.find("host", "observed")];
    scene(host, host.phase_step("super", "flush").sample(), "en");
    app.frame = 1;
    app.animation_phase = 15.2;
    painter.draw(first.data(), pitch, app);
    app.animation_phase = 15.45;
    painter.draw(second.data(), pitch, app);
    check(first == second, "Reduced motion changed pixels while evidence was stationary");
    app.reduced_motion = false;
    painter.draw(first.data(), pitch, app);
    app.animation_phase = 15.6;
    painter.draw(second.data(), pitch, app);
    check(first != second, "Normal motion should visibly animate unknown work");
    if (!png.empty()) {
        const auto& headset = catalog.routes[catalog.find("headset", "observed")];
        app.reduced_motion = true;
        scene(headset, headset.phase_step("build", "package-install").sample(), "en");
        painter.draw(first.data(), pitch, app);
        save_preview_png(png, first.data(), pitch);
        std::cout << "RECOVERY_PREVIEW_PNG " << png << '\n';
    }
    std::cout << "RECOVERY_NATIVE_RENDER_OK: 7/9 labels, both locales, concentric layout, reduced motion\n";
}

} // namespace p4ui::preview
