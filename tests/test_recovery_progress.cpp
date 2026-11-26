// SPDX-License-Identifier: Unlicense
#include "ui.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void expect(bool okay, const char* why) {
    if (!okay) throw std::runtime_error(why);
}

constexpr const char* boot = R"({"id":"boot","name":"Boot Image","status":"done"})";
constexpr const char* later = R"({"id":"data","name":"Data","status":"pending"})";

std::string task(const std::string& status, const std::string& steps, const std::string& parent = "running") {
    return R"({"id":"test-run","kind":"host","active":1,"locked":true,"status":")" + status +
        R"(","stages":[)" + boot + R"(,{"id":"super","name":"System Image","status":")" + parent +
        R"(","substeps":[)" + steps + "]}," + later + "]}";
}

p4ui::Task load(const std::string& encoded) {
    p4ui::Task decoded;
    expect(p4ui::decode_task(encoded, decoded), "receipt must decode");
    expect(decoded.active == 1 && decoded.stages[2].state == "pending", "later stage must stay pending");
    return decoded;
}

const std::string check = R"({"id":"inspect","status":"done","weight":5},)";
const std::string rest = R"(,{"id":"read","status":"pending","weight":20},{"id":"digest","status":"pending","weight":5})";

std::string write(const std::string& source, const std::string& status, int done, bool trusted = true) {
    return R"({"id":"write","status":")" + status +
        R"(","weight":70,"measured":true,"unit":"bytes","progress_source":")" + source +
        R"(","progress_trusted":)" + (trusted ? "true" : "false") + R"(,"done":)" + std::to_string(done) + R"(,"total":100})";
}

void fraction(const p4ui::Task& task, double expected) {
    const auto& stage = task.stages[1];
    expect(!stage.substeps.empty() && stage.fraction.has_value(), "full weighted plan required");
    expect(std::abs(*stage.fraction - expected) < 1e-8, "unexpected earned progress");
}
}

int main() {
    try {
        fraction(load(task("running", check + write("local-written-bytes", "running", 50) + rest)), .40);
        fraction(load(task("running", check + write("host-input-bytes", "running", 90) + rest)), .05);
        fraction(load(task("running", check + write("local-written-bytes", "running", 90, false) + rest)), .05);
        fraction(load(task("running", check + write("local-written-bytes", "running", 110) + rest)), .05);
        const auto failed = load(task("error", check + write("local-written-bytes", "error", 50) + rest, "error"));
        fraction(failed, .40);
        expect(failed.stages[1].substeps[2].state == "pending", "failure must retain unexecuted work in the plan");
        fraction(load(task("running", check + write("local-written-bytes", "running", 100) + rest)), .7493);
        fraction(load(task("running", check + write("local-written-bytes", "done", 100) + rest)), .75);
        fraction(load(task("done", check + write("local-written-bytes", "done", 100) + rest, "done")), 1);

        const auto unmetered = load(task("running", check +
            R"({"id":"write","status":"running","weight":70,"measured":true})" + rest));
        fraction(unmetered, .05);
        expect(unmetered.stages[1].substeps[1].measured && !unmetered.stages[1].substeps[1].fraction,
            "planned meter type must survive a missing first sample");
        const auto malformed = load(task("running", check + write("local-written-bytes", "running", 50) +
            R"(,{"id":"write","status":"done","weight":100})"));
        expect(!malformed.stages[1].fraction, "a duplicate ID must not inflate progress");
        std::cout << "RECOVERY_PROGRESS_OK\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
