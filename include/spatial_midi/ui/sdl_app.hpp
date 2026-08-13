#pragma once

#include "spatial_midi/core/midi_note_input_worker.hpp"
#include "spatial_midi/core/transport_worker.hpp"
#include "spatial_midi/ui/text_cache.hpp"

#include <SDL.h>
#include <SDL_ttf.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace spatial_midi {
    struct OutputOpenResult {
        std::shared_ptr<MidiOutput> backend;
        std::string status;
        bool connected = false;
    };

    struct ClockInputOpenResult {
        std::shared_ptr<MidiClockInput> backend;
        std::string status;
    };

    struct NoteInputOpenResult {
        std::shared_ptr<MidiNoteInput> backend;
        std::string status;
    };

    class SdlApp {
    public:
        using OutputOpener = std::function<OutputOpenResult()>;
        using ClockInputOpener = std::function<ClockInputOpenResult()>;
        using NoteInputOpener = std::function<NoteInputOpenResult(const std::shared_ptr<MidiOutput> &)>;

        SdlApp(TransportWorker &worker, std::shared_ptr<MidiOutput> output, std::string midi_status,
               OutputOpener output_opener, ClockInputOpener clock_input_opener, NoteInputOpener note_input_opener,
               int midi_channel, int note_input_channel, int default_velocity,
               std::filesystem::path project_file = "graph.json",
               std::optional<std::filesystem::path> font_path = std::nullopt);

        ~SdlApp();

        SdlApp(const SdlApp &) = delete;

        SdlApp &operator=(const SdlApp &) = delete;

        int run();

    private:
        struct Point {
            int x = 0;
            int y = 0;
        };

        struct StaticText {
            SDL_Texture *texture = nullptr;
            int width = 0;
            int height = 0;
        };

        // A typed snapshot of every value that can affect the rendered frame.
        // Keeping it local to SdlApp avoids coupling transport code to UI details.
        struct RenderState {
            int window_width = 0;
            int window_height = 0;
            int grid_scale = 1;
            int camera_x = 0;
            int camera_y = 0;
            std::optional<int> selected_id;
            std::optional<int> connect_source_id;
            std::optional<int> disconnect_source_id;
            bool show_help = false;
            bool temporary_status_visible = false;
            std::uint64_t status_revision = 0;
            std::optional<int> current_node_id;
            int rounded_bpm = 0;
            bool midi_clock_enabled = false;
            bool midi_clock_active = false;
            bool external_clock_enabled = false;
            bool external_clock_active = false;
            TransportState transport_state = TransportState::Stopped;
            int release_gap_eighths = kDefaultReleaseGapEighths;
            int output_channel = 0;
            bool worker_alive = false;
            bool worker_responsive = false;

            friend bool operator==(const RenderState &, const RenderState &) = default;
        };

        void initialize(const std::optional<std::filesystem::path> &font_path);

        void shutdown() noexcept;

        void set_window_icon();

        void cache_static_help_text();

        void destroy_static_text() noexcept;

        void handle_events();

        void handle_event(const SDL_Event &event);

        void handle_key(const SDL_KeyboardEvent &event);

        void handle_mouse_down(const SDL_MouseButtonEvent &event);

        void handle_mouse_motion(const SDL_MouseMotionEvent &event);

        void pan_with_keys(double frame_seconds);

        void toggle_grid_spacing();

        void save_project();

        void load_project();

        void toggle_midi_clock_input();

        void disable_midi_clock_input();

        void consume_transport_failures();

        void consume_note_input_failure();

        void start_note_input();

        [[nodiscard]] std::unique_ptr<MidiNoteInputWorker> make_note_input_worker(
            const std::shared_ptr<MidiNoteInput> &input);

        [[nodiscard]] std::string reconnect_note_input(const std::shared_ptr<MidiOutput> &source_output);

        void handle_midi_note_entry(int pitch, int velocity);

        void reconnect_midi();

        void midi_failed(const std::string &message);

        void refresh_graph();

        void status(std::string message, double seconds = 2.5);

        [[nodiscard]] std::string visible_status(double now) const;

        void mark_dirty() noexcept { dirty_ = true; }

        [[nodiscard]] RenderState render_state(const TransportSnapshot &transport, double now) const;

        bool redraw_if_needed(const TransportSnapshot &transport, double now);

        [[nodiscard]] static double tempo_step(double bpm, int direction) noexcept;

        void draw(const TransportSnapshot &transport, double now);

        void draw_grid();

        void draw_edges();

        void draw_nodes(const TransportSnapshot &transport);

        void draw_panels(const TransportSnapshot &transport, double now);

        void draw_help(int top);

        [[nodiscard]] Point grid_to_screen(int x, int y) const;

        [[nodiscard]] Point screen_to_grid(int x, int y) const;

        [[nodiscard]] int grid_pitch() const noexcept;

        [[nodiscard]] int canvas_bottom() const noexcept;

        [[nodiscard]] const Node *node_hit(int x, int y) const;

        [[nodiscard]] static std::pair<std::vector<Point>, Point> orthogonal_edge_route(
            Point start, Point target, int target_margin);

        [[nodiscard]] static Point polyline_midpoint(const std::vector<Point> &points);

        void draw_arrowhead(Point tip, Point direction, SDL_Color color);

        void draw_thick_line(Point first, Point second, int thickness, SDL_Color color);

        void draw_filled_circle(Point center, int radius, SDL_Color color);

        void draw_circle_outline(Point center, int radius, int thickness, SDL_Color color);

        void draw_square_outline(Point center, int half_size, int thickness, SDL_Color color);

        void draw_filled_triangle(Point first, Point second, Point third, SDL_Color color);

        StaticText make_text(TTF_Font *font, const std::string &text, SDL_Color color,
                             std::optional<SDL_Color> background = std::nullopt);

        void draw_text(TTF_Font *font, const std::string &text, SDL_Color color, int x, int y,
                       bool right_aligned = false);

        [[nodiscard]] static std::filesystem::path locate_font(const std::optional<std::filesystem::path> &requested);

        TransportWorker &worker_;
        Graph graph_;
        std::shared_ptr<MidiOutput> output_;
        std::shared_ptr<MidiClockInput> clock_input_;
        std::shared_ptr<MidiNoteInput> note_input_;
        std::unique_ptr<MidiNoteInputWorker> note_input_worker_;
        OutputOpener output_opener_;
        ClockInputOpener clock_input_opener_;
        NoteInputOpener note_input_opener_;
        std::string midi_status_;
        std::string midi_clock_input_status_ = "MIDI Clock input off";
        std::string clock_input_connection_status_;
        int midi_channel_ = 1;
        int note_input_channel_ = 1;
        int default_velocity_ = kDefaultVelocity;
        std::filesystem::path project_file_;
        bool midi_clock_input_enabled_ = false;
        Uint32 note_input_event_type_ = 0;

        SDL_Window *window_ = nullptr;
        SDL_Renderer *renderer_ = nullptr;
        TTF_Font *font_ = nullptr;
        TTF_Font *small_font_ = nullptr;
        std::unique_ptr<TextCache> pitch_cache_;
        std::unique_ptr<TextCache> velocity_cache_;
        std::unique_ptr<TextCache> paraphonic_cache_;
        std::unique_ptr<TextCache> edge_cache_;
        std::unique_ptr<TextCache> counter_edge_cache_;
        std::vector<std::vector<StaticText> > help_lines_;

        double camera_x_ = 96.0;
        double camera_y_ = 96.0;
        int grid_scale_ = 1;
        std::optional<int> selected_id_;
        std::optional<int> connect_source_id_;
        std::optional<int> disconnect_source_id_;
        std::optional<int> dragging_node_id_;
        bool panning_ = false;
        bool show_help_ = true;
        bool running_ = true;
        bool dirty_ = true;
        std::string status_message_;
        double status_until_ = 0.0;
        std::uint64_t status_revision_ = 0;
        std::optional<RenderState> last_render_state_;
        double resize_active_until_ = 0.0;
        std::optional<double> last_resize_redraw_at_;
    };
}
