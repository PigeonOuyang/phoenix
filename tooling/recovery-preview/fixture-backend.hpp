// SPDX-License-Identifier: Unlicense
#pragma once

#include "ui.hpp"

namespace p4ui::preview {

void set_fixture_logs(const std::vector<std::string>& log);
void set_fixture_locale(std::string_view locale);
void set_fixture_telemetry(Telemetry& telemetry);

} // namespace p4ui::preview
