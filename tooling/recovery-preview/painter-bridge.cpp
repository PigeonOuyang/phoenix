// SPDX-License-Identifier: Unlicense
#include "preview-painter.hpp"

// The production painter is private to this translation unit. Rename the
// device entry point; the preview link drops it and its KMS/evdev services.
#define main preview_unlinked_recovery_main
#include "main.cpp"
#undef main

namespace p4ui::preview {

struct PreviewPainter::Impl {
    Painter painter;
    Impl(const std::string& fonts, const std::string& atlas) : painter(fonts, atlas) {}
};

PreviewPainter::PreviewPainter(const std::string& fonts, const std::string& atlas, App& app)
    : impl(std::make_unique<Impl>(fonts, atlas)) {
    impl->painter.bind_log_metrics(app);
}

PreviewPainter::~PreviewPainter() = default;

void PreviewPainter::draw(unsigned char* pixels, int pitch, App& app) {
    if (app.log_layout.row_count > 0 && impl->painter.stable_reader(app))
        impl->painter.reader_scene(pixels, pitch, app);
    else
        impl->painter.scene(pixels, pitch, app);
}

double PreviewPainter::text_width(std::string_view value, double size, bool bold) {
    return impl->painter.progress_width(value, size, bold);
}

bool PreviewPainter::moving() const {
    return impl->painter.animating() || impl->painter.progress_moving();
}

void save_preview_png(const std::string& path, unsigned char* pixels, int pitch) {
    const std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> eye(
        cairo_image_surface_create_for_data(pixels, CAIRO_FORMAT_RGB24, eye_size, eye_size, pitch),
        cairo_surface_destroy);
    const auto status = cairo_surface_write_to_png(eye.get(), path.c_str());
    if (status != CAIRO_STATUS_SUCCESS)
        throw std::runtime_error(std::string("Save preview PNG: ") + cairo_status_to_string(status));
}

} // namespace p4ui::preview
