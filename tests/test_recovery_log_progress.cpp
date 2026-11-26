// SPDX-License-Identifier: Unlicense
#include "../src/recovery-ui/locale.hpp"
#include "../src/recovery-ui/log_progress_policy.hpp"
#include "../src/recovery-ui/progress_layout.hpp"
#include "../tooling/recovery-preview/fixture-backend.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace p4ui;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void show(App& app, const char* source) {
    app.page = "LOG_VIEW";
    app.log_source = source;
    app.compose();
}

bool compact(const App& app) {
    return std::ranges::any_of(app.nodes, [](const Node& n) {
        return n.reader_row < 0 && n.kind == Kind::box && n.layer == Layer::button && n.y == 1642 && n.h == 6;
    });
}

bool footer(const App& app, std::string_view key) {
    return std::ranges::any_of(app.nodes, [&](const Node& n) {
        return n.reader_row < 0 && n.kind == Kind::text && n.y == 1612 &&
            n.value == localized(key, app.settings().locale == "en");
    });
}

bool stripe(const App& app, std::string_view marker) {
    return std::ranges::any_of(app.nodes, [&](const Node& n) {
        return n.kind == Kind::stripes && n.value == marker;
    });
}

Task install_task() {
    Task task;
    task.id = "observed-install";
    task.kind = "host";
    task.status = "running";
    task.locked = true;
    task.stages = {{"Check inputs", "running", {}, "check"}};
    return task;
}

void visibility() {
    App app;
    show(app, "install-live");
    check(footer(app, msg::installation_has_not_started_waiting_for_the_host) &&
          !compact(app) && !progress_ui::has_log_deck(app), "Idle install log has guidance, not a planned rail");
    show(app, "task");
    check(!compact(app) && !progress_ui::has_log_deck(app), "Idle task log has no rail");
    app.backend.task.locked = true;
    app.backend.task.status = "waiting";
    show(app, "install-live");
    check(!footer(app, msg::installation_has_not_started_waiting_for_the_host) &&
          !compact(app) && !progress_ui::has_log_deck(app), "Lock before receipt proves neither idle nor progress");

    app.backend.task = install_task();
    for (const char* source : {"install-live", "task", "/run/overpressure/task.log"}) {
        show(app, source);
        check(progress_ui::has_log_deck(app) && !compact(app) && log_progress::watches_task_log(app),
              "Matching log has a deck and receives task notifications");
        check(log_progress::watches_boot_log(app) == (app.log_source == "install-live"),
              "Only the combined install stream receives both notifications");
    }
    app.follow_install_log = false;
    show(app, "install-live");
    check(progress_ui::has_log_deck(app) && !app.follow_install_log, "Reading older rows in the same install stream keeps its deck");
    for (const char* source : {"boot", "/run/overpressure/boot.log", "kernel", "pstore", "services",
                               "build", "/run/overpressure/report/build.txt", "history",
                               "/archive/task.log", "/run/overpressure/install/dd.log"}) {
        show(app, source);
        check(!progress_ui::has_log_deck(app) && !compact(app) && !log_progress::watches_task_log(app),
              "Unrelated source has no rail or task-driven redraw during a locked install");
        check(log_progress::watches_boot_log(app) ==
                (app.log_source == "boot" || app.log_source == "/run/overpressure/boot.log"),
              "Only the boot log aliases receive boot notifications");
    }
    show(app, "task");
    app.page = "CURRENT_TASK";
    check(!progress_ui::has_log_deck(app), "Overview keeps its separate presentation");

    auto& task = app.backend.task;
    task.stages.clear();
    show(app, "install-live");
    check(footer(app, msg::in_progress) && !compact(app) && !progress_ui::has_log_deck(app),
          "Stage-less receipt shows only known task status");
    task = install_task();
    task.stages[0].id.clear();
    show(app, "install-live");
    check(compact(app) && stripe(app, "") && !progress_ui::has_log_deck(app), "Legacy receipt uses its actual compact stage");

    task.kind = "single";
    task.id = "ui-143";
    app.backend.install_preview = true;
    show(app, "install-live");
    check(!compact(app) && !progress_ui::has_log_deck(app), "Preview flag cannot turn maintenance into a SteamOS install");
    show(app, "task");
    check(compact(app) && stripe(app, "") && !progress_ui::has_log_deck(app), "Corresponding local task uses compact activity");
    task.stages[0].fraction = .375;
    app.compose();
    check(compact(app) && !stripe(app, "") && !stripe(app, "still"), "Measured compact stage has no activity stripes");
    task.stages[0].fraction.reset();
    task.stages[0].state = task.status = "waiting";
    app.compose();
    check(stripe(app, "still") && !stripe(app, ""), "A waiting compact stage never schedules animated activity");
    task.stages[0].state = task.status = "done";
    task.locked = false;
    app.compose();
    check(compact(app) && footer(app, msg::completed_2) && !stripe(app, ""), "Current completed task retains its static receipt");
    task.id = "install-143";
    app.compose();
    check(!compact(app) && !progress_ui::has_log_deck(app), "Standalone command does not own the rolling task log");

    task = {};
    app.seen_install_task = "observed-install";
    show(app, "install-live");
    check(!footer(app, msg::installation_has_not_started_waiting_for_the_host), "Missing prior receipt does not say install never started");
}

void deck_updates() {
    App app;
    app.backend.task = install_task();
    show(app, "install-live");
    auto& task = app.backend.task;
    auto& stage = task.stages[0];
    progress_ui::Animation motion;
    std::vector<Node> nodes;
    const auto draw = [&] {
        nodes.clear();
        progress_ui::draw_log_deck(app, motion, [](std::string_view label, double size, bool) {
            return label.size() * size * .45;
        }, nodes);
    };
    const auto activity = [&] {
        return std::ranges::any_of(nodes, [](const Node& n) {
            return n.kind == Kind::stripes && n.value == "progress-main";
        });
    };
    app.animation_phase = 1;
    draw();
    check(motion.activity && activity(), "Unknown running stage uses activity, not a percentage");
    const log_progress::DeckInput unknown(app);
    app.animation_phase = 2;
    app.follow_install_log = false;
    check(unknown.matches(app), "Clock and reader position do not invalidate stable layout");
    stage.fraction = .4;
    check(!unknown.matches(app), "New measured count invalidates layout");
    draw();
    check(motion.tween && !motion.activity && !activity(), "Measured fill still requests its tween");
    app.animation_phase = 3;
    draw();
    check(!motion.tween && !motion.activity && !activity(), "Settled measured deck needs no animation");
    stage.state = task.status = "done";
    task.locked = false;
    app.reduced_motion = true;
    draw();
    check(!nodes.empty() && !motion.tween && !motion.activity && !activity(), "Completed install remains visible with no animation");

    stage.substeps.push_back({"sample", "sample", "done", 1., true, 1., "bytes", 1, 1});
    const log_progress::DeckInput completed(app);
    stage.substeps[0].total = 2;
    check(!completed.matches(app), "Counter text changes invalidate the deck even with a stable fraction");
    stage.substeps[0].total = 1;
    app.backend.preferences.locale = "zh-CN";
    check(!completed.matches(app), "Language change invalidates deck text");
    app.backend.preferences.locale = "en";
    app.reduced_motion = false;
    check(!completed.matches(app), "Motion preference invalidates deck geometry");
    show(app, "boot");
    draw();
    check(nodes.empty() && !motion.activity && !motion.tween, "Hiding the deck clears animation state");

    task.status = stage.state = "running";
    stage.fraction = std::numeric_limits<double>::quiet_NaN();
    check(log_progress::compact_activity(task), "Invalid percentage remains unknown");
    task.status = "error";
    check(!log_progress::compact_activity(task), "A terminal receipt never requests compact animation");
}
}

int main() {
    try {
        p4ui::preview::set_fixture_locale("en");
        p4ui::preview::set_fixture_logs({"Observed task output"});
        visibility();
        deck_updates();
        std::cout << "LOG_PROGRESS_POLICY_OK\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
