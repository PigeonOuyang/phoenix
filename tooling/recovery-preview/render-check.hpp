// SPDX-License-Identifier: Unlicense
#pragma once

#include "replay.hpp"

namespace p4ui::preview {

void verify_native_render(const Catalog& catalog, const std::string& fonts, const std::string& atlas,
                          const std::string& png);

} // namespace p4ui::preview
