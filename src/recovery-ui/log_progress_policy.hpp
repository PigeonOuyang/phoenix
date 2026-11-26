// SPDX-License-Identifier: Unlicense
#pragma once

#include "ui.hpp"

#include <algorithm>
#include <tuple>

namespace p4ui::log_progress {

enum class Presentation { none, awaiting_install, summary, compact, deck };

constexpr bool task_log_source(std::string_view source) {
    return source == "task" || source == "/run/overpressure/task.log";
}

inline bool watches_task_log(const App& app) {
    return app.page == "LOG_VIEW" && (app.log_source == "install-live" || task_log_source(app.log_source));
}

inline bool watches_boot_log(const App& app) {
    return app.page == "LOG_VIEW" && (app.log_source == "install-live" || app.log_source == "boot" ||
        app.log_source == "/run/overpressure/boot.log");
}

// is_installation is supplied by the same classifier that selects which task
// output to include in install-live. Scrolling that stream does not change its source.
inline Presentation presentation(const App& app, bool is_installation) {
    if (!watches_task_log(app)) return Presentation::none;
    const bool live = app.log_source == "install-live";

    const auto& task = app.backend.task;
    if (task.status == "idle" && task.id.empty() && !task.locked)
        return live && app.seen_install_task.empty() ? Presentation::awaiting_install : Presentation::none;
    if (task.id.empty() || (live && !is_installation)) return Presentation::none;
    // Standalone pico4-install commands publish receipts but don't rotate the
    // shared log; it can still contain the previous task's output.
    if (task.kind == "single" && task.id.starts_with("install-")) return Presentation::none;
    const std::string_view status = task.status;
    if (status != "pending" && status != "starting" && status != "queued" &&
        status != "running" && status != "waiting" && status != "done" &&
        status != "success" && status != "error" && status != "failed")
        return Presentation::none;

    if (task.stages.empty()) return Presentation::summary;
    if (is_installation && std::ranges::any_of(task.stages, [](const Stage& stage) {
        return !stage.id.empty() || !stage.substeps.empty();
    })) return Presentation::deck;
    // The compact rail's all-terminal waiting caption is specific to first boot.
    if (!is_installation && status == "waiting" && std::ranges::all_of(task.stages, [](const Stage& stage) {
        return stage.state == "done" || stage.state == "skipped";
    })) return Presentation::summary;
    return Presentation::compact;
}

inline bool compact_activity(const Task& task) {
    return task.status == "running" && std::ranges::any_of(task.stages, [](const Stage& stage) {
        return stage.state == "running" &&
            (!stage.fraction || !(*stage.fraction >= 0 && *stage.fraction <= 1));
    });
}

// The stripe phase is painted separately. A settled layout only depends on
// the receipt fields read by the deck, locale, and the motion/preview modes.
struct DeckInput {
    Task task;
    std::string locale;
    bool preview, reduced_motion;

    explicit DeckInput(const App& app)
        : task(app.backend.task), locale(app.settings().locale),
          preview(app.backend.install_preview), reduced_motion(app.reduced_motion) {}

    bool matches(const App& app) const {
        const auto& next = app.backend.task;
        const auto receipt = [](const Task& t) {
            return std::tie(t.id, t.kind, t.status, t.detail, t.error, t.active);
        };
        const auto step = [](const Substep& s) {
            return std::tie(s.id, s.name, s.state, s.weight, s.measured, s.fraction, s.unit, s.done, s.total);
        };
        const auto stage = [&](const Stage& a, const Stage& b) {
            return std::tie(a.id, a.name, a.state, a.fraction) == std::tie(b.id, b.name, b.state, b.fraction) &&
                std::ranges::equal(a.substeps, b.substeps, {}, step, step);
        };
        return locale == app.settings().locale && preview == app.backend.install_preview &&
            reduced_motion == app.reduced_motion && receipt(task) == receipt(next) &&
            std::ranges::equal(task.stages, next.stages, stage);
    }
};

} // namespace p4ui::log_progress
