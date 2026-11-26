// SPDX-License-Identifier: Unlicense
#include "replay.hpp"

#include <json-c/json.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using namespace p4ui;
using namespace p4ui::preview;
using Json = json_object;

void check(bool okay, std::string_view message) {
    if (!okay) throw std::runtime_error(std::string(message));
}

bool near(double a, double b) { return std::abs(a - b) < 1e-7; }

const Stage& stage(const Task& task, std::string_view id) {
    const auto found = std::find_if(task.stages.begin(), task.stages.end(),
                                   [=](const auto& item) { return item.id == id; });
    check(found != task.stages.end(), id);
    return *found;
}

const Substep& child(const Stage& parent, std::string_view id) {
    const auto found = std::find_if(parent.substeps.begin(), parent.substeps.end(),
                                   [=](const auto& item) { return item.id == id; });
    check(found != parent.substeps.end(), id);
    return *found;
}

double at(const Route& route, std::string_view stage_id, std::string_view phase_id, double progress = .55) {
    const auto& step = route.phase_step(stage_id, phase_id);
    return step.from + (step.to - step.from) * progress;
}

void add(Json* o, const char* key, const std::string& value) {
    json_object_object_add(o, key, json_object_new_string(value.c_str()));
}

std::string json_task(const Task& task, const Route& route, bool trusted = true) {
    const std::unique_ptr<Json, decltype(&json_object_put)> root(json_object_new_object(), json_object_put);
    add(root.get(), "id", task.id);
    add(root.get(), "name", task.name);
    add(root.get(), "kind", task.kind);
    add(root.get(), "status", task.status);
    add(root.get(), "detail", task.detail);
    add(root.get(), "error", task.error);
    json_object_object_add(root.get(), "active", json_object_new_int(task.active));
    json_object_object_add(root.get(), "locked", json_object_new_boolean(task.locked));
    json_object_object_add(root.get(), "readonly_restored", json_object_new_boolean(task.readonly_restored));
    auto* stages = json_object_new_array();
    json_object_object_add(root.get(), "stages", stages);
    for (std::size_t i = 0; i < task.stages.size(); ++i) {
        const auto& from = task.stages[i];
        auto* to = json_object_new_object();
        json_object_array_add(stages, to);
        add(to, "id", from.id);
        add(to, "name", from.name);
        add(to, "status", from.state);
        auto* substeps = json_object_new_array();
        json_object_object_add(to, "substeps", substeps);
        for (std::size_t j = 0; j < from.substeps.size(); ++j) {
            const auto& sub = from.substeps[j];
            auto* to_sub = json_object_new_object();
            json_object_array_add(substeps, to_sub);
            add(to_sub, "id", sub.id);
            add(to_sub, "name", sub.name);
            add(to_sub, "status", sub.state);
            json_object_object_add(to_sub, "weight", json_object_new_double(sub.weight));
            json_object_object_add(to_sub, "measured", json_object_new_boolean(sub.measured));
            if (!sub.fraction) continue;
            json_object_object_add(to_sub, "done", json_object_new_int64(sub.done));
            json_object_object_add(to_sub, "total", json_object_new_int64(sub.total));
            add(to_sub, "unit", sub.unit);
            add(to_sub, "progress_source", evidence_source(route.stages[i], route.stages[i].phases[j]));
            json_object_object_add(to_sub, "progress_trusted", json_object_new_boolean(trusted));
        }
    }
    return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
}

void export_on_device(const Catalog& catalog, const std::filesystem::path& folder) {
    // Replay real UI receipts from the approved scenarios; no installer is involved.
    std::filesystem::create_directories(folder);
    std::ofstream timeline(folder / "timeline");
    std::ofstream boot_log(folder / "boot.log");
    boot_log << "On-headset simulation. Nothing is installed.\n"
                "Power: play / pause. Volume + / -: next / previous step.\n";
    int frame = 0, initial = -1;
    for (const auto& key : {std::pair{"headset", "observed"}, {"host", "observed"},
                            {"headset", "download-error"}, {"host", "hash-error"}}) {
        const auto& route = catalog.routes[catalog.find(key.first, key.second)];
        for (const auto& step : route.steps) {
            const bool first = &step == &route.steps.front();
            const bool last = &step == &route.steps.back();
            const int subdiv = step.kind == "step" ? 8 : 2;
            const int ms = static_cast<int>(std::lround(1000 * (step.to - step.from) / subdiv));
            for (int ix = 0; ix < subdiv; ++ix, ++frame) {
                const double time = step.from + (step.to - step.from) * ix / subdiv;
                auto data = replay(catalog, route, time, false);
                if (key.first == std::string_view("headset") && key.second == std::string_view("observed") &&
                    &step == &route.phase_step("build", "package-install") && ix == 4) initial = frame;
                timeline << (last && ix + 1 == subdiv ? 0 : ms) << ' ' << (ix == 0) << '\n';
                const auto name = "task." + std::to_string(frame);
                std::ofstream receipt(folder / (name + ".json"));
                receipt << json_task(data.task, route) << '\n';
                std::ofstream log(folder / (name + ".log"));
                if (first) log << "Replay: " << key.first << " / " << key.second << '\n';
                for (const auto& line : data.logs)
                    if (!line.starts_with("Local replay only.")) log << line << '\n';
                check(receipt.good() && log.good(), "could not write replay frame");
            }
        }
    }
    check(initial >= 0 && timeline.good() && boot_log.good(), "could not write replay timeline");
    std::ofstream cursor(folder / "initial");
    cursor << initial << '\n';
    check(cursor.good(), "could not write initial replay cursor");
    std::cout << "RECOVERY_HEADSET_FIXTURES_OK frames=" << frame << " initial=" << initial << '\n';
}

void parser_roundtrip(const Task& synthetic, const Route& route) {
    Task parsed;
    check(decode_task(json_task(synthetic, route), parsed), "Production parser rejected approved fixture");
    check(parsed.id == synthetic.id && parsed.kind == synthetic.kind && parsed.status == synthetic.status &&
          parsed.detail == synthetic.detail && parsed.error == synthetic.error &&
          parsed.locked == synthetic.locked && parsed.readonly_restored == synthetic.readonly_restored &&
          (synthetic.active < 0 || parsed.active == synthetic.active), "Task differs after parser round trip");
    check(parsed.stages.size() == synthetic.stages.size(), "Stage count changed after parsing");
    for (std::size_t i = 0; i < synthetic.stages.size(); ++i) {
        const auto& expected = synthetic.stages[i];
        const auto& actual = parsed.stages[i];
        check(expected.id == actual.id && expected.name == actual.name && expected.state == actual.state &&
              expected.fraction == actual.fraction, "Stage identity or progress differs");
        check(expected.substeps.size() == actual.substeps.size(), "Substep count changed after parsing");
        for (std::size_t j = 0; j < expected.substeps.size(); ++j) {
            const auto& a = actual.substeps[j];
            const auto& e = expected.substeps[j];
            check(a.id == e.id && a.name == e.name && a.state == e.state && a.weight == e.weight &&
                  a.measured == e.measured && a.fraction == e.fraction && a.done == e.done &&
                  a.total == e.total && a.unit == e.unit, "Substep evidence differs");
        }
    }
}

void verify_cases(const Catalog& catalog) {
    check(catalog.routes.size() == 10, "Expected all ten approved replay routes");
    for (const auto& route : catalog.routes) {
        check(replay(catalog, route, 0, false).task.status == "idle", "A replay starts in waiting-for-host");
        for (bool chinese : {false, true}) {
            for (const auto& step : route.steps) {
                try {
                    parser_roundtrip(replay(catalog, route, step.sample(), chinese).task, route);
                } catch (const std::exception& e) {
                    throw std::runtime_error(route.mode + "/" + route.scenario + " " +
                        step.kind + " " + std::to_string(step.stage) + "/" + std::to_string(step.phase) +
                        ": " + e.what());
                }
            }
        }
        const auto final = replay(catalog, route, route.duration, false).task;
        if (route.scenario == "handoff")
            check(final.status == "waiting" && final.detail == catalog.handoff.en &&
                  weighted_progress(stage(final, "finish")) < 1, "Handoff cannot certify first boot");
        else if (route.scenario != "hash-error" && route.scenario != "download-error")
            check(final.status == "done" && final.readonly_restored &&
                  std::all_of(final.stages.begin(), final.stages.end(),
                    [](const Stage& s) { return weighted_progress(s) == 1; }), "Retained-Recovery completion");
    }

    const auto& stall = catalog.routes[catalog.find("headset", "download-error")];
    const auto wait_early = at(stall, "build", "holo-stall", .1);
    const auto wait_late = at(stall, "build", "holo-stall", .9);
    const auto waiting = replay(catalog, stall, wait_early, false).task;
    const auto late = replay(catalog, stall, wait_late, false).task;
    const auto failed = replay(catalog, stall, stall.duration, false).task;
    const auto& build = stage(waiting, "build");
    const auto& holo = child(build, "holo");
    check(waiting.status == "waiting" && holo.state == "waiting" &&
          holo.done == (86LL << 20) && holo.total == (367LL << 20), "Stall keeps its trusted Holo byte count");
    check(!child(build, "holo-stall").fraction &&
          near(weighted_progress(build), (6. + 35. * 86. / 367.) / stall.stages[2].weight) &&
          near(weighted_progress(build), weighted_progress(stage(late, "build"))) &&
          near(weighted_progress(build), weighted_progress(stage(failed, "build"))),
          "Waiting and failure add no credit and preserve the full planned denominator");
    check(failed.status == "error" && stage(failed, "boot").state == "pending" &&
          stage(failed, "super").state == "pending", "A failed download never reaches writes");

    const auto& host = catalog.routes[catalog.find("host", "hash-error")];
    const auto writing = replay(catalog, host, at(host, "super", "write"), false).task;
    Task untrusted;
    check(decode_task(json_task(writing, host, false), untrusted) &&
          !child(stage(untrusted, "super"), "write").fraction &&
          weighted_progress(stage(untrusted, "super")) < weighted_progress(stage(writing, "super")),
          "No trusted flag means no measured byte credit");
    const auto sync_a = replay(catalog, host, at(host, "super", "flush", .1), false).task;
    const auto sync_b = replay(catalog, host, at(host, "super", "flush", .9), false).task;
    check(!child(stage(sync_a, "super"), "flush").fraction &&
          near(weighted_progress(stage(sync_a, "super")), weighted_progress(stage(sync_b, "super"))),
          "Unknown fsync never advances confirmed progress");
    const auto mismatch = replay(catalog, host, host.duration, true).task;
    check(mismatch.status == "error" && stage(mismatch, "super").state == "error" &&
          weighted_progress(stage(mismatch, "super")) < 1 &&
          stage(mismatch, "assets").state == "pending" && stage(mismatch, "finish").state == "pending",
          "Readback mismatch must not complete the failed stage or unlock later steps");
    check(mismatch.error != replay(catalog, host, host.duration, false).task.error,
          "Locales present the same failure with different localized text");
}

} // namespace

int main(int argc, char** argv) try {
    if (argc == 4 && std::string_view(argv[1]) == "--export-on-device") {
        export_on_device(Catalog(argv[2]), argv[3]);
        return 0;
    }
    if (argc != 2) throw std::runtime_error("usage: accept-recovery-preview FIXTURES");
    verify_cases(Catalog(argv[1]));
    std::cout << "RECOVERY_REPLAY_ACCEPTANCE_OK: all 10 routes, both locales, real task decoder\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "Recovery replay acceptance: " << e.what() << '\n';
    return 1;
}
