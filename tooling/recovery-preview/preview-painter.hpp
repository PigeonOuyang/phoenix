// SPDX-License-Identifier: Unlicense
#pragma once

#include "ui.hpp"
#include <memory>

namespace p4ui::preview {

// Presentation-only handle to the production Cairo painter; the caller owns
// the RGB24 stereo buffer and the App for its whole lifetime.
class PreviewPainter {
public:
    PreviewPainter(const std::string& fonts, const std::string& atlas, App& app);
    ~PreviewPainter();
    void draw(unsigned char* pixels, int pitch, App& app);
    double text_width(std::string_view value, double size, bool bold);
    bool moving() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

void save_preview_png(const std::string& path, unsigned char* pixels, int pitch);

} // namespace p4ui::preview
