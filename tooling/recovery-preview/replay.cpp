// SPDX-License-Identifier: Unlicense
#include "replay.hpp"

#include <json-c/json.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace p4ui::preview {
namespace {

using Json = json_object;

void require(bool okay, std::string_view message) {
    if (!okay) throw std::runtime_error(std::string(message));
}

Json* get(Json* object, const char* key, json_type type, bool optional = false) {
    Json* value = nullptr;
    if (json_object_object_get_ex(object, key, &value) &&
        (json_object_is_type(value, type) ||
         (type == json_type_double && json_object_is_type(value, json_type_int)))) return value;
    require(optional, std::string("Invalid design fixture field: ") + key);
    return nullptr;
}

std::string text(Json* object, const char* key) {
    return json_object_get_string(get(object, key, json_type_string));
}

double number(Json* object, const char* key) {
    const auto value = json_object_get_double(get(object, key, json_type_double));
    require(std::isfinite(value), key);
    return value;
}

int integer(Json* object, const char* key) {
    return json_object_get_int(get(object, key, json_type_int));
}

Label label(Json* object, const char* key, bool optional = false) {
    auto* value = get(object, key, json_type_object, optional);
    return value ? Label{text(value, "en"), text(value, "zh")} : Label{};
}

template <typename Read>
void each(Json* object, const char* key, Read read) {
    auto* array = get(object, key, json_type_array);
    for (std::size_t i = 0; i < json_object_array_length(array); ++i) {
        auto* item = json_object_array_get_idx(array, i);
        require(json_object_is_type(item, json_type_object), key);
        read(item);
    }
}

Phase read_phase(Json* object) {
    Phase p;
    p.id = text(object, "id");
    p.outcome = text(object, "outcome");
    p.weight = number(object, "weight");
    p.label = label(object, "label");
    p.note = label(object, "note", true);
    if (auto* meter = get(object, "meter", json_type_object, true)) {
        const auto unit = text(meter, "unit");
        const auto count = number(meter, "total");
        p.stop = number(meter, "stop");
        require(count > 0 && count <= 1048576 && p.stop > 0 && p.stop <= 1, "Invalid fixture meter");
        if (unit == "MiB") {
            p.total = std::llround(count * 1024 * 1024);
            p.unit = "bytes";
        } else {
            require(unit == "pass" || unit == "files" || unit == "packages", "Unknown fixture meter unit");
            p.total = std::llround(count);
            p.unit = unit == "pass" ? "percent" : unit;
        }
    }
    require(!p.id.empty() && p.weight >= 0, "Invalid fixture phase");
    return p;
}

Route read_route(Json* object) {
    Route route;
    route.mode = text(object, "mode");
    route.scenario = text(object, "scenario");
    route.duration = number(object, "duration");
    each(object, "stages", [&](Json* item) {
        Route::Main s;
        s.id = text(item, "id");
        s.label = label(item, "label");
        s.weight = number(item, "weight");
        each(item, "phases", [&](Json* p) { s.phases.push_back(read_phase(p)); });
        route.stages.push_back(std::move(s));
    });
    require((route.mode == "host" && route.stages.size() == 7) ||
            (route.mode == "headset" && route.stages.size() == 9), "Invalid fixture route");
    each(object, "segments", [&](Json* item) {
        Step step{text(item, "kind"), text(item, "result"), integer(item, "i"),
                  integer(item, "j"), number(item, "from"), number(item, "to")};
        require(step.from == (route.steps.empty() ? 0 : route.steps.back().to) &&
                step.to > step.from, "Invalid fixture timeline");
        if (step.kind == "close" || step.kind == "step") {
            require(step.stage >= 0 && static_cast<std::size_t>(step.stage) < route.stages.size(),
                    "Fixture stage is out of range");
            if (step.kind == "step")
                require(step.phase >= 0 && static_cast<std::size_t>(step.phase) <
                        route.stages[step.stage].phases.size(), "Fixture substep is out of range");
        } else {
            require(step.kind == "idle" || step.kind == "complete" || step.kind == "error" ||
                    step.kind == "handoff", "Unknown fixture segment");
        }
        route.steps.push_back(std::move(step));
    });
    require(!route.steps.empty() && route.steps.front().kind == "idle" &&
            route.steps.back().to == route.duration, "Incomplete fixture timeline");
    return route;
}

void sample(Substep& substep, const Phase& phase, double value) {
    if (!substep.measured) return;
    substep.unit = phase.unit;
    substep.total = phase.total;
    substep.done = std::llround(phase.total * std::clamp(value, 0., phase.stop));
    const double ratio = static_cast<double>(substep.done) / substep.total;
    substep.fraction = substep.state == "done" ? ratio : std::min(ratio, .999);
}

} // namespace

Catalog::Catalog(const std::filesystem::path& source) {
    std::ifstream input;
    if (source != "-") input.open(source);
    std::istream& stream = source == "-" ? std::cin : input;
    require(static_cast<bool>(stream), "Cannot open design fixture JSON");
    const std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    const std::unique_ptr<Json, decltype(&json_object_put)> root(
        json_tokener_parse(bytes.c_str()), json_object_put);
    require(root && json_object_is_type(root.get(), json_type_object), "Cannot read design fixture JSON");
    idle = label(root.get(), "idle");
    complete = label(root.get(), "complete");
    handoff = label(root.get(), "handoff");
    each(root.get(), "routes", [&](Json* route) { routes.push_back(read_route(route)); });
    require(!routes.empty(), "No design fixture routes");
}

std::size_t Catalog::find(std::string_view mode, std::string_view scenario) const {
    const auto it = std::find_if(routes.begin(), routes.end(), [&](const auto& r) {
        return r.mode == mode && r.scenario == scenario;
    });
    require(it != routes.end(), "Unknown route or scenario");
    return static_cast<std::size_t>(it - routes.begin());
}

const Step& Route::phase_step(std::string_view stage, std::string_view phase) const {
    const auto found = std::find_if(steps.begin(), steps.end(), [&](const Step& step) {
        if (step.kind != "step") return false;
        const auto& parent = stages[step.stage];
        return parent.id == stage && parent.phases[step.phase].id == phase;
    });
    require(found != steps.end(), "Unknown replay phase: " + std::string(stage) + "/" + std::string(phase));
    return *found;
}

const char* evidence_source(const Route::Main& stage, const Phase& phase) {
    if (phase.unit == "packages") return "local-package-count";
    if (phase.unit == "files") return "local-verified-files";
    if (phase.unit == "percent") return "local-check-progress";
    if (phase.unit != "bytes") return "";
    if (phase.id == "write") return "local-written-bytes";
    if (phase.id == "read" || phase.id == "existing-hash") return "local-readback-bytes";
    if (phase.id == "holo" || phase.id == "package-fetch") return "local-download-bytes";
    if (phase.id == "receive-assets") return "local-received-bytes";
    if (stage.id == "build") return "local-processed-bytes";
    throw std::runtime_error("Unknown fixture evidence: " + stage.id + "/" + phase.id);
}

Frame replay(const Catalog& catalog, const Route& route, double time, bool chinese) {
    Frame frame;
    auto& task = frame.task;
    task.kind = route.mode;
    task.active = -1;
    frame.logs.push_back(chinese ? "本地模拟；未连接头显，不执行安装。" :
                         "Local replay only. No headset is connected and nothing is installed.");
    frame.logs.push_back(catalog.idle.get(chinese));
    for (const auto& main : route.stages) {
        Stage stage{};
        stage.id = main.id;
        stage.name = main.label.get(chinese);
        for (const auto& phase : main.phases) {
            Substep substep{};
            substep.id = phase.id;
            substep.name = phase.label.get(chinese);
            substep.state = "pending";
            substep.weight = phase.weight;
            substep.measured = phase.total > 0;
            stage.substeps.push_back(std::move(substep));
        }
        task.stages.push_back(std::move(stage));
    }
    for (std::size_t i = 0; i < route.steps.size(); ++i) {
        const auto& step = route.steps[i];
        frame.step = i;
        const bool current = time < step.to || i + 1 == route.steps.size();
        if (step.kind == "step") {
            const auto& phase = route.stages[step.stage].phases[step.phase];
            auto& stage = task.stages[step.stage];
            auto& child = stage.substeps[step.phase];
            const auto& name = phase.label.get(chinese);
            const auto& note = phase.note.get(chinese);
            const auto progress = (time - step.from) / ((step.to - step.from) * .9);
            task.active = step.stage;
            task.detail = name;
            task.status = "running";
            stage.state = "running";
            if (phase.outcome == "skip") child.state = "skipped";
            else if (phase.outcome == "error") {
                child.state = stage.state = task.status = "error";
                task.error = name;
            } else if (phase.outcome == "wait" || (!current && phase.stop < 1)) {
                child.state = "waiting";
                if (current || phase.id == "handoff") stage.state = task.status = "waiting";
            } else {
                child.state = current ? "running" : "done";
            }
            if (phase.total > 0) sample(child, phase, (current ? std::clamp(progress, 0., 1.) : 1.) * phase.stop);
            frame.logs.push_back("[" + std::to_string(step.stage + 1) + "." +
                                 std::to_string(step.phase + 1) + "] " + name);
            if (!note.empty()) frame.logs.push_back("  " + note);
        } else if (step.kind == "close") {
            auto& stage = task.stages[step.stage];
            stage.state = step.result == "skip" ? "skipped" : "done";
            task.active = step.stage;
            task.detail = stage.name;
        } else if (step.kind == "complete") {
            task.active = -1;
            task.status = "done";
            task.detail = catalog.complete.get(chinese);
            frame.logs.push_back(task.detail);
        } else if (step.kind == "handoff") {
            task.detail = catalog.handoff.get(chinese);
            frame.logs.push_back(task.detail);
        }
        if (current) break;
    }
    if (task.status != "idle") {
        task.id = "preview-" + route.mode + "-" + route.scenario;
        task.name = "System installation";
    }
    task.locked = task.status == "running" || task.status == "waiting";
    task.readonly_restored = task.status == "done";
    for (auto& stage : task.stages) stage.fraction = weighted_progress(stage);
    return frame;
}

} // namespace p4ui::preview
