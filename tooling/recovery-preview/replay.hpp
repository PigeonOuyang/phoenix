// SPDX-License-Identifier: Unlicense
#pragma once

#include "ui.hpp"
#include <filesystem>

namespace p4ui::preview {

struct Label {
    std::string en, zh;
    const std::string& get(bool chinese) const { return chinese ? zh : en; }
};

struct Phase {
    std::string id, outcome, unit;
    Label label, note;
    double weight = 0, stop = 1;
    std::int64_t total = 0;
};

struct Step {
    std::string kind, result;
    int stage = -1, phase = -1;
    double from = 0, to = 0;
    double sample() const { return from + (to - from) * 0.55; }
};

struct Route {
    struct Main {
        std::string id;
        Label label;
        double weight = 0;
        std::vector<Phase> phases;
    };
    std::string mode, scenario;
    std::vector<Main> stages;
    std::vector<Step> steps;
    double duration = 0;
    const Step& phase_step(std::string_view stage, std::string_view phase) const;
};

struct Catalog {
    Label idle, complete, handoff;
    std::vector<Route> routes;
    explicit Catalog(const std::filesystem::path& source);
    std::size_t find(std::string_view mode, std::string_view scenario) const;
};

struct Frame {
    Task task;
    std::vector<std::string> logs;
    std::size_t step = 0;
};

Frame replay(const Catalog& catalog, const Route& route, double time, bool chinese);
const char* evidence_source(const Route::Main& stage, const Phase& phase);

} // namespace p4ui::preview
