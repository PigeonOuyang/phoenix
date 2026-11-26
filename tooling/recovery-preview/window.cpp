// SPDX-License-Identifier: Unlicense
#include "fixture-backend.hpp"
#include "preview-painter.hpp"
#include "render-check.hpp"
#include "replay.hpp"

#include <SDL.h>
#include <linux/input.h>
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using namespace p4ui;
using namespace p4ui::preview;

void sdl_check(bool okay, const char* operation) {
    if (!okay) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

struct Display {
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{nullptr, SDL_DestroyWindow};
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer{nullptr, SDL_DestroyRenderer};
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture{nullptr, SDL_DestroyTexture};

    Display() {
        SDL_Rect area{};
        const int size = SDL_GetDisplayUsableBounds(0, &area) == 0
            ? std::clamp(std::min(area.w, area.h) - 120, 500, 1080) : 900;
        window.reset(SDL_CreateWindow("PICO 4 Recovery | Local replay", SDL_WINDOWPOS_CENTERED,
                     SDL_WINDOWPOS_CENTERED, size, size, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI));
        sdl_check(static_cast<bool>(window), "Create native preview window");
        // Software texture upload keeps the presentation path independent of /dev/dri.
        renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE));
        sdl_check(static_cast<bool>(renderer), "Create preview presenter");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        sdl_check(SDL_RenderSetLogicalSize(renderer.get(), eye_size, eye_size) == 0, "Set preview aspect");
        texture.reset(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING,
                                        eye_size, eye_size));
        sdl_check(static_cast<bool>(texture), "Create native frame texture");
    }

    void present(const unsigned char* pixels, int pitch) {
        sdl_check(SDL_UpdateTexture(texture.get(), nullptr, pixels, pitch) == 0, "Upload native frame");
        sdl_check(SDL_RenderClear(renderer.get()) == 0 &&
                  SDL_RenderCopy(renderer.get(), texture.get(), nullptr, nullptr) == 0, "Draw native frame");
        SDL_RenderPresent(renderer.get());
    }
};

class Preview {
    const Catalog& catalog;
    std::size_t route_index;
    std::size_t step = 0;
    double time = 0;
    double enter_at = 0;
    bool playing = false;
    bool reduced;
    SDL_Keycode held_enter = SDLK_UNKNOWN;
    int held_volume = 0;
    std::vector<std::string> previous_logs;

public:
    App app{true};

    Preview(const Catalog& source, std::size_t initial, bool zh, bool rm, bool play, std::string_view at)
        : catalog(source), route_index(initial), playing(play), reduced(rm) {
        app.depth.zero = true;
        app.draft = app.depth;
        set_fixture_telemetry(app.backend.telemetry);
        sync_locale(zh);
        if (at != "-") {
            const auto split = at.find('/');
            if (split == std::string_view::npos) throw std::runtime_error("Expected stage/substep for initial position");
            time = route().phase_step(at.substr(0, split), at.substr(split + 1)).sample();
        }
    }

    const Route& route() const { return catalog.routes.at(route_index); }

    void sync_locale(bool chinese) {
        set_fixture_locale(chinese ? "zh-CN" : "en");
        app.backend.preferences = app.backend.load_ui_preferences();
        app.log_layout.sampled_at = 0;
    }

    void restart(std::size_t index) {
        route_index = index;
        time = 0;
        step = 0;
        playing = false;
    }

    void cycle_route(int direction) {
        const auto begin = catalog.find(route().mode, "observed");
        auto end = begin;
        while (end < catalog.routes.size() && catalog.routes[end].mode == route().mode) ++end;
        const auto count = static_cast<int>(end - begin);
        const auto next = (static_cast<int>(route_index - begin) + direction + count) % count;
        restart(begin + static_cast<std::size_t>(next));
    }

    void seek(int direction, bool stage_only) {
        const int last = static_cast<int>(route().steps.size()) - 1;
        int next = std::clamp(static_cast<int>(step) + direction, 0, last);
        if (stage_only) {
            const int current_stage = route().steps[step].stage;
            while (next > 0 && next < last) {
                const auto i = static_cast<std::size_t>(next);
                const auto& candidate = route().steps[i];
                if (candidate.kind == "step" && candidate.phase == 0 &&
                    (candidate.stage != current_stage || direction < 0)) break;
                next += direction;
            }
        }
        step = static_cast<std::size_t>(next);
        time = route().steps[step].sample();
        playing = false;
    }

    void release_keys() {
        held_volume = 0;
        held_enter = SDLK_UNKNOWN;
        app.power_held = false;
        app.held_power.clear();
        app.cancel_reader_scroll();
    }

    bool needs_frame(double now, double last_draw) const {
        const auto& scroll = app.reader_scroll;
        return playing || app.language_save.valid() || scroll.key || scroll.animating(now) ||
            (!app.reduced_motion && app.page != "LOG_VIEW" && app.backend.task.locked && now - last_draw > .1);
    }

    bool event(const SDL_Event& e, double now) {
        if (e.type == SDL_QUIT) return false;
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            release_keys();
            return true;
        }
        if (e.type != SDL_KEYDOWN && e.type != SDL_KEYUP) return true;
        const auto key = e.key.keysym.sym;
        if (e.type == SDL_KEYUP) {
            if (held_enter != SDLK_UNKNOWN && key == held_enter) {
                held_enter = SDLK_UNKNOWN;
                const bool eligible = app.power_held && now - enter_at < .8 && app.held_power == app.activation();
                app.power_held = false;
                app.held_power.clear();
                if (eligible) app.key(KEY_POWER);
            }
            const int volume = key == SDLK_UP ? KEY_VOLUMEUP : key == SDLK_DOWN ? KEY_VOLUMEDOWN : 0;
            if (volume && volume == held_volume) {
                app.end_reader_scroll(volume, now);
                held_volume = 0;
            }
            return true;
        }
        if (key == SDLK_UP || key == SDLK_DOWN) {
            app.power_held = false;
            app.held_power.clear();
            const int volume = key == SDLK_UP ? KEY_VOLUMEUP : KEY_VOLUMEDOWN;
            if (app.reader_view()) {
                if (!e.key.repeat) {
                    if (held_volume) app.end_reader_scroll(held_volume, now);
                    if (app.begin_reader_scroll(volume, now)) held_volume = volume;
                }
            } else {
                app.key(volume);
            }
            return true;
        }
        if (e.key.repeat) return true;
        if (key != SDLK_RETURN && key != SDLK_KP_ENTER && held_enter != SDLK_UNKNOWN) release_keys();
        switch (key) {
        case SDLK_ESCAPE: return false;
        case SDLK_1: restart(catalog.find("host", "observed")); break;
        case SDLK_2: restart(catalog.find("headset", "observed")); break;
        case SDLK_LEFTBRACKET: cycle_route(-1); break;
        case SDLK_RIGHTBRACKET: cycle_route(1); break;
        case SDLK_LEFT: seek(-1, (e.key.keysym.mod & KMOD_SHIFT) != 0); break;
        case SDLK_RIGHT: seek(1, (e.key.keysym.mod & KMOD_SHIFT) != 0); break;
        case SDLK_HOME: restart(route_index); break;
        case SDLK_END: time = route().duration; playing = false; break;
        case SDLK_SPACE:
            if (time >= route().duration) time = 0;
            playing = !playing;
            break;
        case SDLK_l: sync_locale(app.settings().locale != "zh-CN"); break;
        case SDLK_m: reduced = !reduced; break;
        case SDLK_TAB:
            release_keys();
            if (app.page == "LOG_VIEW") {
                if (!app.stack.empty() && app.stack.back().page == "CURRENT_TASK") app.back();
                else app.navigate("CURRENT_TASK", "", 1);
            } else if (app.page == "CURRENT_TASK" && !app.stack.empty() && app.stack.back().page == "LOG_VIEW") {
                app.back();
            } else {
                app.navigate("LOG_VIEW", "install-live", 0);
            }
            break;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            if (held_enter == SDLK_UNKNOWN) {
                app.cancel_reader_scroll();
                held_volume = 0;
                app.compose();
                app.held_power = app.activation();
                app.power_held = true;
                held_enter = key;
                enter_at = now;
            }
            break;
        default: break;
        }
        return true;
    }

    void update(double now, double elapsed) {
        if (playing) {
            time = std::min(time + elapsed, route().duration);
            playing = time < route().duration;
        }
        app.animation_phase = now;
        app.reduced_motion = reduced;
        const bool render_chinese = app.settings().locale == "zh-CN";
        auto frame = replay(catalog, route(), time, render_chinese);
        step = frame.step;
        app.backend.task = std::move(frame.task);
        if (frame.logs != previous_logs) {
            previous_logs = std::move(frame.logs);
            set_fixture_logs(previous_logs);
            app.log_layout.sampled_at = 0;
        }
        app.tick_reader_scroll(now);
        app.compose();
    }

    std::string title() const {
        const auto& current = route().steps[step];
        std::string label = current.kind;
        if (current.stage >= 0) {
            const auto& parent = route().stages[current.stage];
            label = parent.id;
            if (current.phase >= 0) label += "/" + parent.phases[current.phase].id;
        }
        return "PICO 4 Recovery | LOCAL REPLAY | " + std::string(route().mode == "host" ? "PC" : "Headset") +
            " / " + route().scenario + " | " + label + " | " + std::to_string(step + 1) + "/" +
            std::to_string(route().steps.size()) + (playing ? " playing | " : " paused | ") +
            app.settings().locale + (reduced ? " | reduced motion" : "");
    }
};

} // namespace

int main(int argc, char** argv) try {
    if ((argc == 5 || argc == 6) && std::string_view(argv[1]) == "--render-check") {
        verify_native_render(Catalog(argv[2]), argv[3], argv[4], argc == 6 ? argv[5] : "");
        return 0;
    }
    if (argc != 10) {
        std::cerr << "usage: recovery-preview FIXTURES FONTS ATLAS MODE SCENARIO LOCALE MOTION PLAY STAGE/SUBSTEP\n";
        return 2;
    }
    const Catalog catalog(argv[1]);
    Preview preview(catalog, catalog.find(argv[4], argv[5]), std::string_view(argv[6]) == "zh-CN",
                    std::string_view(argv[7]) == "1", std::string_view(argv[8]) == "1", argv[9]);
    sdl_check(SDL_Init(SDL_INIT_VIDEO) == 0, "Initialize WSLg video");
    const auto sdl_quit = std::unique_ptr<void, void(*)(void*)>(&preview, [](void*) { SDL_Quit(); });
    Display display;
    PreviewPainter painter(argv[2], argv[3], preview.app);
    constexpr int pitch = canvas_width * 4;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(pitch) * eye_size);
    std::string title;
    auto last = monotonic_seconds();
    bool ready = false, show = true;
    while (show) {
        SDL_Event e{};
        const int timeout = !ready || preview.needs_frame(monotonic_seconds(), last) ||
            (!preview.app.reduced_motion && painter.moving()) ? 16 : 100;
        const bool event = SDL_WaitEventTimeout(&e, ready ? timeout : 0) == 1;
        const double now = monotonic_seconds();
        if (event) show = preview.event(e, now);
        while (show && SDL_PollEvent(&e)) show = preview.event(e, now);
        if (!show) break;
        if (ready && !event && !preview.needs_frame(now, last) &&
            (preview.app.reduced_motion || !painter.moving())) continue;
        preview.update(now, now - last);
        painter.draw(pixels.data(), pitch, preview.app);
        display.present(pixels.data(), pitch);
        ++preview.app.frame;
        const auto next = preview.title();
        if (next != title) {
            title = next;
            SDL_SetWindowTitle(display.window.get(), title.c_str());
        }
        if (!ready) {
            ready = true;
            std::cout << "RECOVERY_PREVIEW_READY " << SDL_GetCurrentVideoDriver() << ' ' << title << std::endl;
        }
        last = now;
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "Recovery preview: " << e.what() << '\n';
    return 1;
}
