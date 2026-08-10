#include "spatial_midi/ui/sdl_app.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <ranges>
#include <stdexcept>
#include <system_error>

namespace spatial_midi {
    namespace {
        constexpr int kWindowWidth = 1280;
        constexpr int kWindowHeight = 720;
        constexpr int kWindowIconSize = 32;
        constexpr int kGridSize = 32;
        constexpr int kFullNoteGridInterval = 16;
        constexpr int kNodeRadius = 15;
        constexpr int kRelayHalfSize = 13;
        constexpr int kStatusHeight = 30;
        constexpr int kHelpHeight = 172;

        constexpr SDL_Color kBackground{18, 20, 26, 255};
        constexpr SDL_Color kGrid{38, 42, 52, 255};
        constexpr SDL_Color kEdge{112, 128, 154, 255};
        constexpr SDL_Color kHighlightedEdge{174, 190, 216, 255};
        constexpr SDL_Color kEdgeLabel{184, 195, 214, 255};
        constexpr SDL_Color kCounterEdge{77, 201, 181, 255};
        constexpr SDL_Color kHighlightedCounterEdge{139, 232, 216, 255};
        constexpr SDL_Color kCounterEdgeLabel{147, 235, 221, 255};
        constexpr SDL_Color kRelayEdge{117, 91, 169, 255};
        constexpr SDL_Color kHighlightedRelayEdge{190, 160, 232, 255};
        constexpr SDL_Color kRelayCounterEdge{172, 111, 202, 255};
        constexpr SDL_Color kHighlightedRelayCounterEdge{224, 174, 241, 255};
        constexpr SDL_Color kNodeFill{48, 94, 135, 255};
        constexpr SDL_Color kNodeBorder{134, 194, 235, 255};
        constexpr SDL_Color kRelayFill{73, 57, 108, 255};
        constexpr SDL_Color kRelayBorder{190, 160, 232, 255};
        constexpr SDL_Color kRestFill{48, 50, 58, 255};
        constexpr SDL_Color kRestBorder{212, 138, 90, 255};
        constexpr SDL_Color kParaphonic{185, 164, 255, 255};
        constexpr SDL_Color kSelected{255, 197, 84, 255};
        constexpr SDL_Color kStart{105, 220, 150, 255};
        constexpr SDL_Color kPlayhead{255, 103, 104, 255};
        constexpr SDL_Color kText{225, 230, 240, 255};
        constexpr SDL_Color kMutedText{157, 166, 184, 255};
        constexpr SDL_Color kPanel{25, 29, 38, 255};
        constexpr SDL_Color kPanelBorder{57, 64, 78, 255};

        const std::array<std::vector<std::string>, 4> kHelpColumns{
            {
                {
                    "Click empty: create musical node",
                    "Shift+click empty: create relay",
                    "Click/drag node: select/move",
                    "Middle-drag/WASD: pan",
                    "Home: grid origin",
                    "Z: toggle 1x/2x grid spacing",
                    "H: toggle help",
                    "Esc: cancel",
                },
                {
                    "F1: save project",
                    "F2: load project",
                    "F5: reconnect MIDI output",
                    "R: set start node",
                    "M: toggle note/rest",
                    "Up/Down: first pitch semitone",
                    "Shift+Up/Down: first pitch octave",
                    "Ctrl+Up/Down: last pitch semitone",
                },
                {
                    "Ctrl+Shift+Up/Down: last pitch octave",
                    "Left/Right: velocity -/+ 1",
                    "Shift+Left/Right: velocity -/+ 10",
                    "P: add pitch (up to 6)",
                    "Shift+P: remove pitch (down to 1)",
                    "C+click: add edge",
                    "X+click: remove edge",
                    "U: remove outgoing edges",
                },
                {
                    "Delete: delete node",
                    "O: Random/Round-robin",
                    "Space: play/stop",
                    "Enter/Pause: pause/resume",
                    "[ / ]: tempo -/+ 5",
                    ", / .: Release Gap -/+ 1/8 (0/8-4/8)",
                    "K: toggle MIDI Clock output",
                    "I: toggle MIDI Clock input",
                },
            }
        };

        void set_color(SDL_Renderer *renderer, SDL_Color color) {
            SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        }

        std::string lowercase(std::string value) {
            std::ranges::transform(value, value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        bool is_font_file(const std::filesystem::path &path) {
            const std::string extension = lowercase(path.extension().string());
            return extension == ".ttf" || extension == ".otf";
        }

        std::optional<std::filesystem::path> find_generic_system_font() {
            std::vector<std::filesystem::path> roots{
                "/usr/share/fonts",
                "/usr/local/share/fonts",
            };
            if (const char *home = std::getenv("HOME")) {
                roots.emplace_back(std::filesystem::path(home) / ".local/share/fonts");
                roots.emplace_back(std::filesystem::path(home) / ".fonts");
            }

            std::optional<std::filesystem::path> first_font;
            for (const auto &root: roots) {
                std::error_code error;
                if (!std::filesystem::is_directory(root, error)) {
                    continue;
                }

                std::filesystem::recursive_directory_iterator iterator(
                    root, std::filesystem::directory_options::skip_permission_denied, error);
                const std::filesystem::recursive_directory_iterator end;
                while (iterator != end) {
                    if (error) {
                        error.clear();
                        iterator.increment(error);
                        continue;
                    }

                    const auto path = iterator->path();
                    if (iterator->is_regular_file(error) && !error && is_font_file(path)) {
                        const std::string filename = lowercase(path.filename().string());
                        if (filename.find("mono") != std::string::npos) {
                            error.clear();
                            iterator.increment(error);
                            continue;
                        }
                        if (!first_font) {
                            first_font = path;
                        }
                        if (filename.find("sans") != std::string::npos) {
                            return path;
                        }
                    }
                    error.clear();
                    iterator.increment(error);
                }
            }
            return first_font;
        }

        int allow_editor_event(void *userdata, SDL_Event *event) {
            if (!event) {
                return 0;
            }
            const auto note_event_type = *static_cast<const Uint32 *>(userdata);
            if (event->type == note_event_type) {
                return 1;
            }

            switch (event->type) {
                case SDL_QUIT:
                case SDL_KEYDOWN:
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                case SDL_MOUSEMOTION:
                    return 1;
                case SDL_WINDOWEVENT:
                    switch (event->window.event) {
                        case SDL_WINDOWEVENT_SHOWN:
                        case SDL_WINDOWEVENT_EXPOSED:
                        case SDL_WINDOWEVENT_RESIZED:
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                        case SDL_WINDOWEVENT_MAXIMIZED:
                        case SDL_WINDOWEVENT_RESTORED:
                        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
                            return 1;
                        default:
                            return 0;
                    }
                default:
                    return 0;
            }
        }
    }

    void SdlApp::initialize(const std::optional<std::filesystem::path> &font_path) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
        }

        // The editor does not need high-volume SDL events such as mouse-wheel,
        // text-input, or controller traffic. Filtering them keeps the event queue
        // bounded during long sessions.
        note_input_event_type_ = SDL_RegisterEvents(1);
        if (note_input_event_type_ == static_cast<Uint32>(-1)) {
            SDL_Quit();
            throw std::runtime_error(std::string("SDL_RegisterEvents: ") + SDL_GetError());
        }
        SDL_SetEventFilter(allow_editor_event, &note_input_event_type_);

        if (TTF_Init() != 0) {
            SDL_Quit();
            throw std::runtime_error(std::string("TTF_Init: ") + TTF_GetError());
        }

        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        window_ = SDL_CreateWindow("Spatial MIDI Graph Sequencer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   kWindowWidth, kWindowHeight, SDL_WINDOW_RESIZABLE);
        if (!window_) {
            throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
        }

        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!renderer_) {
            throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

        const auto font_file = locate_font(font_path);
        font_ = TTF_OpenFont(font_file.string().c_str(), 19);
        small_font_ = TTF_OpenFont(font_file.string().c_str(), 16);
        if (!font_ || !small_font_) {
            throw std::runtime_error(std::string("TTF_OpenFont: ") + TTF_GetError());
        }

        pitch_cache_ = std::make_unique<TextCache>(renderer_, small_font_, kText);
        velocity_cache_ = std::make_unique<TextCache>(renderer_, small_font_, kMutedText);
        paraphonic_cache_ = std::make_unique<TextCache>(renderer_, small_font_, kParaphonic);
        edge_cache_ = std::make_unique<TextCache>(renderer_, small_font_, kEdgeLabel, kPanel);
        counter_edge_cache_ = std::make_unique<TextCache>(renderer_, small_font_, kCounterEdgeLabel, kPanel);

        cache_static_help_text();
        set_window_icon();
    }

    void SdlApp::shutdown() noexcept {
        note_input_worker_.reset();
        note_input_.reset();

        destroy_static_text();
        pitch_cache_.reset();
        velocity_cache_.reset();
        paraphonic_cache_.reset();
        edge_cache_.reset();
        counter_edge_cache_.reset();

        if (small_font_) {
            TTF_CloseFont(small_font_);
            small_font_ = nullptr;
        }
        if (font_) {
            TTF_CloseFont(font_);
            font_ = nullptr;
        }
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
        if (TTF_WasInit()) {
            TTF_Quit();
        }
        SDL_Quit();
    }

    void SdlApp::set_window_icon() {
        SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormat(0, kWindowIconSize, kWindowIconSize, 32,
                                                           SDL_PIXELFORMAT_RGBA32);
        if (!icon) {
            return;
        }

        SDL_FillRect(icon, nullptr, SDL_MapRGBA(icon->format, 0, 0, 0, 0));
        constexpr Point center{kWindowIconSize / 2, kWindowIconSize / 2,};

        if (SDL_LockSurface(icon) == 0) {
            auto *pixels = static_cast<std::uint32_t *>(icon->pixels);
            const int stride = icon->pitch / static_cast<int>(sizeof(std::uint32_t));
            const auto fill = SDL_MapRGBA(icon->format, kNodeFill.r, kNodeFill.g, kNodeFill.b, 255);
            const auto border = SDL_MapRGBA(icon->format, kNodeBorder.r, kNodeBorder.g, kNodeBorder.b, 255);

            for (int y = 0; y < kWindowIconSize; ++y) {
                for (int x = 0; x < kWindowIconSize; ++x) {
                    const int dx = x - center.x;
                    const int dy = y - center.y;
                    const int distance_squared = dx * dx + dy * dy;

                    if (distance_squared <= kNodeRadius * kNodeRadius) {
                        pixels[y * stride + x] = fill;
                    }
                    if (distance_squared <= kNodeRadius * kNodeRadius && distance_squared >= (kNodeRadius - 2) * (
                            kNodeRadius - 2)) {
                        pixels[y * stride + x] = border;
                    }
                }
            }
            SDL_UnlockSurface(icon);
        }

        SDL_SetWindowIcon(window_, icon);
        SDL_FreeSurface(icon);
    }

    void SdlApp::cache_static_help_text() {
        for (const auto &column: kHelpColumns) {
            std::vector<StaticText> rendered;
            rendered.reserve(column.size());
            for (const std::string &line: column) {
                rendered.push_back(make_text(small_font_, line, kMutedText));
            }
            help_lines_.push_back(std::move(rendered));
        }
    }

    void SdlApp::destroy_static_text() noexcept {
        for (auto &column: help_lines_) {
            for (StaticText &text: column) {
                if (text.texture) {
                    SDL_DestroyTexture(text.texture);
                }
            }
        }
        help_lines_.clear();
    }


    void SdlApp::draw(const TransportSnapshot &transport, double now) {
        set_color(renderer_, kBackground);
        SDL_RenderClear(renderer_);
        draw_grid();
        draw_edges();
        draw_nodes(transport);
        draw_panels(transport, now);
    }

    void SdlApp::draw_grid() {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        (void) height;

        const int bottom = canvas_bottom();
        const int pitch = grid_pitch();
        const int first_column = static_cast<int>(std::floor(-camera_x_ / pitch));
        const int last_column = static_cast<int>(std::floor((width - camera_x_) / pitch));

        for (int column = first_column; column <= last_column; ++column) {
            const int x = static_cast<int>(std::lround(camera_x_ + column * pitch));
            draw_thick_line({x, 0}, {x, bottom}, column % kFullNoteGridInterval == 0 ? 2 : 1, kGrid);
        }

        const int first_row = static_cast<int>(std::floor(-camera_y_ / pitch));
        const int last_row = static_cast<int>(std::floor((bottom - camera_y_) / pitch));

        for (int row = first_row; row <= last_row; ++row) {
            const int y = static_cast<int>(std::lround(camera_y_ + row * pitch));
            draw_thick_line({0, y}, {width, y}, row % kFullNoteGridInterval == 0 ? 2 : 1, kGrid);
        }
    }

    void SdlApp::draw_edges() {
        std::optional<int> highlighted_source_id;
        if (selected_id_) {
            const Node *selected = graph_.find_node(*selected_id_);
            if (selected) {
                highlighted_source_id = selected->id;
            }
        }

        const auto edge_is_highlighted = [&](const Edge &edge) {
            return highlighted_source_id && edge.source_id == *highlighted_source_id;
        };

        // Non-highlighted relay output edges are the lowest graph layer. Their
        // screen-space length is purely visual; the graph model assigns zero ticks.
        const auto draw_relay_edges = [&](bool highlighted) {
            for (const Edge &edge: graph_.edges()) {
                if (edge_is_highlighted(edge) != highlighted) {
                    continue;
                }

                const Node *source = graph_.find_node(edge.source_id);
                const Node *target = graph_.find_node(edge.target_id);
                if (!source || !target || !is_relay(*source)) {
                    continue;
                }

                const Point start = grid_to_screen(source->x, source->y);
                const Point target_center = grid_to_screen(target->x, target->y);
                const double dx = static_cast<double>(target_center.x - start.x);
                const double dy = static_cast<double>(target_center.y - start.y);
                const double length = std::hypot(dx, dy);
                if (length <= 0.0) {
                    continue;
                }

                const double unit_x = dx / length;
                const double unit_y = dy / length;
                const double target_margin = kNodeRadius + 4.0;
                const Point tip{
                    static_cast<int>(std::lround(target_center.x - unit_x * target_margin)),
                    static_cast<int>(std::lround(target_center.y - unit_y * target_margin)),
                };

                const bool counter = source->routing_mode == RoutingMode::Counter;
                const SDL_Color color = highlighted
                                            ? (counter ? kHighlightedRelayCounterEdge : kHighlightedRelayEdge)
                                            : (counter ? kRelayCounterEdge : kRelayEdge);
                draw_thick_line(start, tip, 2, color);

                const Point base{
                    static_cast<int>(std::lround(tip.x - unit_x * 9.0)),
                    static_cast<int>(std::lround(tip.y - unit_y * 9.0)),
                };
                const Point wing_a{
                    static_cast<int>(std::lround(base.x - unit_y * 5.0)),
                    static_cast<int>(std::lround(base.y + unit_x * 5.0)),
                };
                const Point wing_b{
                    static_cast<int>(std::lround(base.x + unit_y * 5.0)),
                    static_cast<int>(std::lround(base.y - unit_x * 5.0)),
                };
                draw_filled_triangle(tip, wing_a, wing_b, color);
            }
        };

        // Ordinary edges retain Manhattan routing and distance-derived timing.
        // Draw selected-node output edges last so they remain legible wherever
        // Manhattan routes overlap. Each length label stays in its edge's layer.
        const auto draw_manhattan_edges = [&](bool highlighted) {
            for (const Edge &edge: graph_.edges()) {
                if (edge_is_highlighted(edge) != highlighted) {
                    continue;
                }

                const Node *source = graph_.find_node(edge.source_id);
                const Node *target = graph_.find_node(edge.target_id);
                if (!source || !target || is_relay(*source)) {
                    continue;
                }

                const bool counter = source->routing_mode == RoutingMode::Counter;
                const SDL_Color color = highlighted
                                            ? (counter ? kHighlightedCounterEdge : kHighlightedEdge)
                                            : (counter ? kCounterEdge : kEdge);
                const int target_margin = is_relay(*target)
                                              ? kRelayHalfSize + 4
                                              : kNodeRadius + 4;
                auto [points, direction] = orthogonal_edge_route(
                    grid_to_screen(source->x, source->y),
                    grid_to_screen(target->x, target->y),
                    target_margin);

                for (std::size_t index = 1; index < points.size(); ++index) {
                    draw_thick_line(points[index - 1], points[index], 3, color);
                }
                if (points.size() >= 2) {
                    draw_arrowhead(points.back(), direction, color);
                }

                const Point midpoint = polyline_midpoint(points);
                const std::string ticks = std::to_string(
                    graph_.edge_ticks(edge.source_id, edge.target_id));
                const CachedText text = (counter ? *counter_edge_cache_ : *edge_cache_).get(ticks);
                SDL_Rect destination{
                    midpoint.x - text.width / 2,
                    midpoint.y - text.height / 2,
                    text.width,
                    text.height,
                };
                SDL_RenderCopy(renderer_, text.texture, nullptr, &destination);
            }
        };

        draw_relay_edges(false);
        draw_manhattan_edges(false);
        draw_relay_edges(true);
        draw_manhattan_edges(true);
    }

    void SdlApp::draw_nodes(const TransportSnapshot &transport) {
        for (const Node &node: graph_.nodes()) {
            const Point center = grid_to_screen(node.x, node.y);

            if (is_relay(node)) {
                if (transport.current_node_id == node.id) {
                    draw_square_outline(center, kRelayHalfSize + 8, 3, kPlayhead);
                }
                if (graph_.start_node_id() == node.id) {
                    draw_square_outline(center, kRelayHalfSize + 5, 2, kStart);
                }

                set_color(renderer_, kRelayFill);
                SDL_Rect relay{
                    center.x - kRelayHalfSize,
                    center.y - kRelayHalfSize,
                    kRelayHalfSize * 2 + 1,
                    kRelayHalfSize * 2 + 1,
                };
                SDL_RenderFillRect(renderer_, &relay);
                draw_square_outline(center, kRelayHalfSize, 2, kRelayBorder);

                if (selected_id_ == node.id) {
                    draw_square_outline(center, kRelayHalfSize + 3, 2, kSelected);
                }
                if (connect_source_id_ == node.id || disconnect_source_id_ == node.id) {
                    draw_square_outline(center, kRelayHalfSize + 7, 1, kSelected);
                }
                continue;
            }

            if (transport.current_node_id == node.id) {
                draw_circle_outline(center, kNodeRadius + 8, 3, kPlayhead);
            }
            if (graph_.start_node_id() == node.id) {
                draw_circle_outline(center, kNodeRadius + 5, 2, kStart);
            }

            draw_filled_circle(center, kNodeRadius, node.silenced ? kRestFill : kNodeFill);
            if (!node.silenced) {
                draw_circle_outline(center, kNodeRadius, 2, kNodeBorder);
            }

            if (selected_id_ == node.id) {
                draw_circle_outline(center, kNodeRadius + 3, 2, kSelected);
            }
            if (connect_source_id_ == node.id || disconnect_source_id_ == node.id) {
                draw_circle_outline(center, kNodeRadius + 7, 1, kSelected);
            }

            const int label_x = center.x + kNodeRadius + 6;
            const CachedText primary = pitch_cache_->get(pitch_name(node.pitches.front()));
            SDL_Rect primary_rect{
                center.x - primary.width / 2,
                center.y - primary.height / 2,
                primary.width,
                primary.height,
            };
            SDL_RenderCopy(renderer_, primary.texture, nullptr, &primary_rect);

            if (node.pitches.size() > 1) {
                std::string label;
                for (std::size_t index = 1; index < node.pitches.size(); ++index) {
                    if (index > 1) {
                        label += "+";
                    }
                    label += pitch_name(node.pitches[index]);
                }

                const CachedText secondary = paraphonic_cache_->get(label);
                SDL_Rect secondary_rect{
                    label_x,
                    center.y - 3 - secondary.height,
                    secondary.width,
                    secondary.height,
                };
                SDL_RenderCopy(renderer_, secondary.texture, nullptr, &secondary_rect);
            }

            // Draw the rest outline and mark after the primary pitch so the label is visibly crossed out.
            if (node.silenced) {
                draw_circle_outline(center, kNodeRadius, 2, kRestBorder);
                const int slash = static_cast<int>(std::lround(kNodeRadius * 0.55));
                draw_thick_line(
                    {center.x - slash, center.y - slash},
                    {center.x + slash, center.y + slash},
                    3,
                    kRestBorder);
                draw_thick_line(
                    {center.x + slash, center.y - slash},
                    {center.x - slash, center.y + slash},
                    3,
                    kRestBorder);
            }

            if (node.velocity != default_velocity_) {
                const CachedText velocity = velocity_cache_->get(std::to_string(node.velocity));
                SDL_Rect velocity_rect{
                    label_x,
                    center.y + 3,
                    velocity.width,
                    velocity.height,
                };
                SDL_RenderCopy(renderer_, velocity.texture, nullptr, &velocity_rect);
            }
        }
    }

    void SdlApp::draw_panels(const TransportSnapshot &transport, double now) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        const int status_y = height - kStatusHeight;

        if (show_help_) {
            const int help_y = status_y - kHelpHeight;
            set_color(renderer_, kPanel);
            SDL_Rect panel{0, help_y, width, kHelpHeight};
            SDL_RenderFillRect(renderer_, &panel);
            set_color(renderer_, kPanelBorder);
            SDL_RenderDrawLine(renderer_, 0, help_y, width, help_y);
            draw_help(help_y);
        }

        set_color(renderer_, kPanel);
        SDL_Rect status_panel{0, status_y, width, kStatusHeight};
        SDL_RenderFillRect(renderer_, &status_panel);
        set_color(renderer_, kPanelBorder);
        SDL_RenderDrawLine(renderer_, 0, status_y, width, status_y);

        draw_text(small_font_, visible_status(now), kText, 10, status_y + 7, false);

        std::string mode = transport_state_name(transport.state);
        if (!transport.worker_alive) {
            mode = "Timing engine failed";
        } else if (!transport.worker_responsive) {
            mode = "Timing engine unresponsive";
        }

        std::string clock_state;
        if (transport.external_clock_enabled || transport.external_clock_active) {
            clock_state = "In";
        } else if (transport.midi_clock_active) {
            clock_state = "Out";
        } else if (transport.midi_clock_enabled) {
            clock_state = "Armed";
        } else {
            clock_state = "Off";
        }

        const std::string right =
            "State: " + mode +
            "   Tempo: " + std::to_string(static_cast<int>(std::lround(transport.bpm))) + " BPM" +
            "   Clock: " + clock_state +
            "   Release Gap: " + std::to_string(transport.release_gap_eighths) + "/8" +
            "   Grid: " + std::to_string(grid_scale_) + "x" +
            "   MIDI Ch: " + std::to_string(transport.output_channel + 1);
        draw_text(font_, right, kMutedText, width - 10, status_y + 7, true);
    }

    void SdlApp::draw_help(int top) {
        constexpr int kLeftPadding = 10;
        constexpr int kColumnGap = 18;
        constexpr int kTopPadding = 7;
        constexpr int kRowSpacing = 20;

        std::vector<int> column_widths(help_lines_.size());
        for (std::size_t column = 0; column < help_lines_.size(); ++column) {
            for (const StaticText &text: help_lines_[column]) {
                column_widths[column] = std::max(column_widths[column], text.width);
            }
        }

        int x = kLeftPadding;
        for (std::size_t column = 0; column < help_lines_.size(); ++column) {
            for (std::size_t row = 0; row < help_lines_[column].size(); ++row) {
                const StaticText &text = help_lines_[column][row];
                SDL_Rect destination{
                    x, top + kTopPadding + static_cast<int>(row) * kRowSpacing, text.width, text.height,
                };
                SDL_RenderCopy(renderer_, text.texture, nullptr, &destination);
            }
            x += column_widths[column] + kColumnGap;
        }
    }

    SdlApp::Point SdlApp::grid_to_screen(int x, int y) const {
        return {
            static_cast<int>(std::lround(camera_x_ + x * grid_pitch())),
            static_cast<int>(std::lround(camera_y_ + y * grid_pitch())),
        };
    }

    SdlApp::Point SdlApp::screen_to_grid(int x, int y) const {
        return {
            static_cast<int>(std::lround((x - camera_x_) / grid_pitch())),
            static_cast<int>(std::lround((y - camera_y_) / grid_pitch())),
        };
    }

    int SdlApp::grid_pitch() const noexcept {
        return kGridSize * grid_scale_;
    }

    int SdlApp::canvas_bottom() const noexcept {
        int height = 0;
        SDL_GetWindowSize(window_, nullptr, &height);
        return height - kStatusHeight - (show_help_ ? kHelpHeight : 0);
    }

    const Node *SdlApp::node_hit(int x, int y) const {
        constexpr int radius_squared = (kNodeRadius + 4) * (kNodeRadius + 4);

        // Reverse iteration gives the most recently inserted (visually topmost)
        // node priority if hit regions ever overlap.
        for (const Node &node: std::views::reverse(graph_.nodes())) {
            const Point point = grid_to_screen(node.x, node.y);
            const int dx = x - point.x;
            const int dy = y - point.y;

            if (is_relay(node)) {
                if (std::abs(dx) <= kRelayHalfSize + 4 &&
                    std::abs(dy) <= kRelayHalfSize + 4) {
                    return &node;
                }
            } else if (dx * dx + dy * dy <= radius_squared) {
                return &node;
            }
        }
        return nullptr;
    }

    std::pair<std::vector<SdlApp::Point>, SdlApp::Point>
    SdlApp::orthogonal_edge_route(Point start, Point target, int target_margin) {
        const int dx = target.x - start.x;
        const int dy = target.y - start.y;
        if (dx == 0 && dy == 0) {
            return {{start}, {0, 0}};
        }

        Point direction;
        Point bend = start;
        if (dx == 0) {
            direction = {0, dy > 0 ? 1 : -1};
        } else if (dy == 0) {
            direction = {dx > 0 ? 1 : -1, 0};
        } else if (std::abs(dx) >= std::abs(dy)) {
            // Travel along the dominant axis first so orthogonal routes are deterministic.
            direction = {0, dy > 0 ? 1 : -1};
            bend = {target.x, start.y};
        } else {
            direction = {dx > 0 ? 1 : -1, 0};
            bend = {start.x, target.y};
        }

        const Point end{target.x - direction.x * target_margin, target.y - direction.y * target_margin,};
        std::vector<Point> points{start};
        if (!(bend.x == start.x && bend.y == start.y) && !(bend.x == end.x && bend.y == end.y)) {
            points.push_back(bend);
        }
        if (!(points.back().x == end.x && points.back().y == end.y)) {
            points.push_back(end);
        }
        return {points, direction};
    }

    SdlApp::Point SdlApp::polyline_midpoint(const std::vector<Point> &points) {
        if (points.empty()) {
            return {};
        }

        int total = 0;
        for (std::size_t index = 1; index < points.size(); ++index) {
            total += std::abs(points[index].x - points[index - 1].x) + std::abs(points[index].y - points[index - 1].y);
        }

        const double halfway = total / 2.0;
        double travelled = 0.0;
        for (std::size_t index = 1; index < points.size(); ++index) {
            const Point first = points[index - 1];
            const Point second = points[index];
            const int length = std::abs(second.x - first.x) + std::abs(second.y - first.y);

            if (travelled + length >= halfway) {
                const double fraction = length == 0 ? 0.0 : (halfway - travelled) / length;
                return {
                    static_cast<int>(std::lround(first.x + (second.x - first.x) * fraction)),
                    static_cast<int>(std::lround(first.y + (second.y - first.y) * fraction)),
                };
            }
            travelled += length;
        }
        return points.back();
    }

    void SdlApp::draw_arrowhead(Point tip, Point direction, SDL_Color color) {
        const Point perpendicular{-direction.y, direction.x};
        const Point base{tip.x - direction.x * 9, tip.y - direction.y * 9,};
        draw_filled_triangle(tip, {base.x + perpendicular.x * 5, base.y + perpendicular.y * 5,},
                             {base.x - perpendicular.x * 5, base.y - perpendicular.y * 5,}, color);
    }

    void SdlApp::draw_thick_line(Point first, Point second, int thickness, SDL_Color color) {
        set_color(renderer_, color);

        if (first.x == second.x) {
            SDL_Rect rect{
                first.x - thickness / 2, std::min(first.y, second.y), thickness, std::abs(second.y - first.y) + 1,
            };
            SDL_RenderFillRect(renderer_, &rect);
        } else if (first.y == second.y) {
            SDL_Rect rect{
                std::min(first.x, second.x), first.y - thickness / 2, std::abs(second.x - first.x) + 1, thickness,
            };
            SDL_RenderFillRect(renderer_, &rect);
        } else {
            const double dx = static_cast<double>(second.x - first.x);
            const double dy = static_cast<double>(second.y - first.y);
            const double length = std::hypot(dx, dy);
            const double perpendicular_x = -dy / length;
            const double perpendicular_y = dx / length;

            for (int offset = -thickness / 2; offset <= thickness / 2; ++offset) {
                const int x_offset = static_cast<int>(std::lround(perpendicular_x * offset));
                const int y_offset = static_cast<int>(std::lround(perpendicular_y * offset));
                SDL_RenderDrawLine(
                    renderer_,
                    first.x + x_offset,
                    first.y + y_offset,
                    second.x + x_offset,
                    second.y + y_offset);
            }
        }
    }

    void SdlApp::draw_filled_circle(Point center, int radius, SDL_Color color) {
        set_color(renderer_, color);
        for (int y = -radius; y <= radius; ++y) {
            const int x = static_cast<int>(std::floor(std::sqrt(radius * radius - y * y)));
            SDL_RenderDrawLine(renderer_, center.x - x, center.y + y, center.x + x, center.y + y);
        }
    }

    void SdlApp::draw_circle_outline(Point center, int radius, int thickness, SDL_Color color) {
        set_color(renderer_, color);

        for (int layer = 0; layer < thickness; ++layer) {
            int x = radius - layer;
            int y = 0;
            int error = 1 - x;

            while (x >= y) {
                const int points[8][2] = {{x, y}, {y, x}, {-y, x}, {-x, y}, {-x, -y}, {-y, -x}, {y, -x}, {x, -y},};
                for (const auto &point: points) {
                    SDL_RenderDrawPoint(renderer_, center.x + point[0], center.y + point[1]);
                }

                ++y;
                if (error < 0) {
                    error += 2 * y + 1;
                } else {
                    --x;
                    error += 2 * (y - x + 1);
                }
            }
        }
    }

    void SdlApp::draw_square_outline(
        Point center,
        int half_size,
        int thickness,
        SDL_Color color) {
        set_color(renderer_, color);
        for (int layer = 0; layer < thickness; ++layer) {
            const int radius = half_size - layer;
            SDL_Rect rect{
                center.x - radius,
                center.y - radius,
                radius * 2 + 1,
                radius * 2 + 1,
            };
            SDL_RenderDrawRect(renderer_, &rect);
        }
    }

    void SdlApp::draw_filled_triangle(Point first, Point second, Point third, SDL_Color color) {
        const std::array<SDL_Vertex, 3> vertices{
            SDL_Vertex{
                .position = {static_cast<float>(first.x), static_cast<float>(first.y)},
                .color = color,
                .tex_coord = {},
            },
            SDL_Vertex{
                .position = {static_cast<float>(second.x), static_cast<float>(second.y)},
                .color = color,
                .tex_coord = {},
            },
            SDL_Vertex{
                .position = {static_cast<float>(third.x), static_cast<float>(third.y)},
                .color = color,
                .tex_coord = {},
            },
        };

        // A failed geometry submission intentionally leaves the arrowhead undrawn.
        (void) SDL_RenderGeometry(
            renderer_, nullptr, vertices.data(), static_cast<int>(vertices.size()), nullptr, 0);
    }

    void SdlApp::draw_cached(TextCache &cache, const std::string &text, SDL_Rect destination) {
        const CachedText cached = cache.get(text);
        destination.w = cached.width;
        destination.h = cached.height;
        SDL_RenderCopy(renderer_, cached.texture, nullptr, &destination);
    }

    SdlApp::StaticText SdlApp::make_text(TTF_Font *font, const std::string &text, SDL_Color color,
                                         std::optional<SDL_Color> background) {
        SDL_Surface *surface = background
                                   ? TTF_RenderUTF8_Shaded(font, text.c_str(), color, *background)
                                   : TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!surface) {
            throw std::runtime_error(std::string("TTF render: ") + TTF_GetError());
        }

        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
        const StaticText result{texture, surface->w, surface->h,};
        SDL_FreeSurface(surface);

        if (!texture) {
            throw std::runtime_error(std::string("SDL_CreateTextureFromSurface: ") + SDL_GetError());
        }
        return result;
    }

    void SdlApp::draw_text(TTF_Font *font, const std::string &text, SDL_Color color, int x, int y, bool right_aligned) {
        StaticText rendered = make_text(font, text, color);
        const int left = right_aligned ? std::max(10, x - rendered.width) : x;
        SDL_Rect destination{left, y, rendered.width, rendered.height,};
        SDL_RenderCopy(renderer_, rendered.texture, nullptr, &destination);
        SDL_DestroyTexture(rendered.texture);
    }

    std::filesystem::path SdlApp::locate_font(const std::optional<std::filesystem::path> &requested) {
        std::vector<std::filesystem::path> candidates;
        if (requested) {
            candidates.push_back(*requested);
        }
        if (const char *environment = std::getenv("SPATIAL_MIDI_FONT")) {
            candidates.emplace_back(environment);
        }

        // Fall back through common system sans-serif faces. No particular font
        // family is required.
        candidates.emplace_back("/usr/share/fonts/truetype/freefont/FreeSansBold.ttf");
        candidates.emplace_back("/usr/share/fonts/gnu-free/FreeSansBold.otf");
        candidates.emplace_back("/usr/share/fonts/gnu-free/FreeSansBold.ttf");
        candidates.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        candidates.emplace_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
        candidates.emplace_back("/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf");

        for (const auto &candidate: candidates) {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
                return candidate;
            }
        }
        if (const auto fallback = find_generic_system_font()) {
            return *fallback;
        }
        throw std::runtime_error("No usable TrueType/OpenType font found on this system");
    }
}
