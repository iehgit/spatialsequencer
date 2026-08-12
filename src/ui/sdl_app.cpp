#include "spatial_midi/ui/sdl_app.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace spatial_midi {
    namespace {
        constexpr double kPanSpeed = 480.0;
        constexpr double kResizeRedrawInterval = 1.0 / 30.0;
        constexpr double kResizeQuietSeconds = 0.15;
        constexpr double kFrameInterval = 1.0 / 120.0;
    }

    SdlApp::SdlApp(TransportWorker &worker, std::shared_ptr<MidiOutput> output, std::string midi_status,
                   OutputOpener output_opener, ClockInputOpener clock_input_opener, NoteInputOpener note_input_opener,
                   int midi_channel, int note_input_channel, int default_velocity, std::filesystem::path project_file,
                   std::optional<std::filesystem::path> font_path)
        : worker_(worker), graph_(worker.graph_snapshot()), output_(std::move(output)),
          output_opener_(std::move(output_opener)), clock_input_opener_(std::move(clock_input_opener)),
          note_input_opener_(std::move(note_input_opener)), midi_status_(std::move(midi_status)),
          midi_channel_(midi_channel), note_input_channel_(note_input_channel), default_velocity_(default_velocity),
          project_file_(std::move(project_file)) {
        if (midi_channel_ < 1 || midi_channel_ > 16) {
            throw std::invalid_argument("MIDI channel must be between 1 and 16");
        }
        if (note_input_channel_ < 1 || note_input_channel_ > 16) {
            throw std::invalid_argument("MIDI note input channel must be between 1 and 16");
        }
        if (default_velocity_ < 0 || default_velocity_ > 127) {
            throw std::invalid_argument("Default MIDI velocity must be between 0 and 127");
        }

        selected_id_ = graph_.start_node_id();
        initialize(font_path);
        start_note_input();
    }

    SdlApp::~SdlApp() {
        shutdown();
    }


    int SdlApp::run() {
        double previous = monotonic_seconds();

        while (running_) {
            const double frame_started = monotonic_seconds();
            handle_events();

            const double now = monotonic_seconds();
            const double frame = std::min(0.05, now - previous);
            previous = now;

            pan_with_keys(frame);
            consume_transport_failures();
            consume_note_input_failure();
            const TransportSnapshot transport = worker_.snapshot();
            redraw_if_needed(transport, now);

            const double remaining = kFrameInterval - (monotonic_seconds() - frame_started);
            if (remaining > 0.0) {
                std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
            }
        }

        try {
            worker_.close();
        } catch (const std::exception &error) {
            status("Could not stop timing engine: " + std::string(error.what()), 5.0);
        }
        return 0;
    }

    void SdlApp::handle_events() {
        SDL_Event event;
        std::optional<SDL_Event> latest_resize;

        while (SDL_PollEvent(&event)) {
            const bool is_resize = event.type == SDL_WINDOWEVENT && (
                                       event.window.event == SDL_WINDOWEVENT_RESIZED || event.window.event ==
                                       SDL_WINDOWEVENT_SIZE_CHANGED || event.window.event == SDL_WINDOWEVENT_MAXIMIZED);

            if (is_resize) {
                // Collapse a burst of resize notifications into one dirty mark.
                latest_resize = event;
            } else {
                handle_event(event);
            }
        }

        if (latest_resize) {
            const double now = monotonic_seconds();
            if (now >= resize_active_until_) {
                last_resize_redraw_at_.reset();
            }
            resize_active_until_ = now + kResizeQuietSeconds;
            mark_dirty();
        }
    }

    void SdlApp::handle_event(const SDL_Event &event) {
        if (event.type == note_input_event_type_) {
            const int pitch = event.user.code & 0xff;
            const int velocity = (event.user.code >> 8) & 0xff;
            handle_midi_note_entry(pitch, velocity);
            return;
        }

        switch (event.type) {
            case SDL_QUIT:
                running_ = false;
                break;

            case SDL_KEYDOWN:
                if (!event.key.repeat) {
                    handle_key(event.key);
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
                handle_mouse_down(event.button);
                break;

            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    dragging_node_id_.reset();
                } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                    panning_ = false;
                }
                break;

            case SDL_MOUSEMOTION:
                handle_mouse_motion(event.motion);
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_EXPOSED || event.window.event == SDL_WINDOWEVENT_SHOWN ||
                    event.window.event == SDL_WINDOWEVENT_RESTORED || event.window.event ==
                    SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                    mark_dirty();
                }
                break;

            default:
                break;
        }
    }

    void SdlApp::handle_key(const SDL_KeyboardEvent &event) {
        const SDL_Keycode key = event.keysym.sym;
        const SDL_Keymod mod = static_cast<SDL_Keymod>(event.keysym.mod);

        if (key == SDLK_h) {
            show_help_ = !show_help_;
            mark_dirty();
            return;
        }
        if (key == SDLK_F1) {
            save_project();
            return;
        }
        if (key == SDLK_F2) {
            load_project();
            return;
        }
        if (key == SDLK_F5) {
            reconnect_midi();
            return;
        }
        if (key == SDLK_HOME) {
            camera_x_ = 0.0;
            camera_y_ = 0.0;
            status("Camera moved to grid origin");
            return;
        }
        if (key == SDLK_z) {
            toggle_grid_spacing();
            return;
        }
        if (key == SDLK_ESCAPE) {
            const bool had_pending_edge = connect_source_id_.has_value() || disconnect_source_id_.has_value();
            connect_source_id_.reset();
            disconnect_source_id_.reset();
            status(had_pending_edge ? "Edge edit cancelled" : "Nothing to cancel");
            return;
        }

        if (key == SDLK_SPACE) {
            const auto transport = worker_.snapshot();
            try {
                if (transport.playing || transport.state == TransportState::Paused) {
                    worker_.stop();
                    status("Playback stopped");
                } else {
                    worker_.start();
                    status("Playback started");
                }
            } catch (const std::exception &error) {
                midi_failed(error.what());
            }
            return;
        }

        if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_PAUSE) {
            try {
                const auto transport = worker_.snapshot();
                if (transport.state == TransportState::Paused) {
                    worker_.resume();
                    status("Playback resumed");
                } else if (transport.playing) {
                    worker_.pause();
                    status("Playback paused");
                } else {
                    status("Playback is stopped");
                }
            } catch (const std::exception &error) {
                midi_failed(error.what());
            }
            return;
        }

        if (key == SDLK_k) {
            try {
                const auto before = worker_.snapshot();
                if (!before.midi_clock_enabled && midi_clock_input_enabled_) {
                    disable_midi_clock_input();
                }

                const bool enabled = worker_.toggle_midi_clock();
                if (before.playing) {
                    if (!enabled && !before.midi_clock_active) {
                        status("MIDI Clock output enable cancelled");
                    } else {
                        status(std::string("MIDI Clock output will ") + (enabled ? "enable" : "disable"));
                    }
                } else {
                    status(std::string("MIDI Clock output ") + (enabled ? "armed" : "disabled"));
                }
            } catch (const std::exception &error) {
                midi_failed(error.what());
            }
            return;
        }

        if (key == SDLK_i) {
            toggle_midi_clock_input();
            return;
        }

        if (key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET) {
            if (midi_clock_input_enabled_) {
                status("Tempo is controlled by MIDI Clock input");
                return;
            }

            const int direction = key == SDLK_LEFTBRACKET ? -1 : 1;
            const double tempo = tempo_step(worker_.snapshot().bpm, direction);
            worker_.set_tempo(tempo);
            status("Tempo: " + std::to_string(static_cast<int>(std::lround(tempo))) + " BPM");
            return;
        }

        if (key == SDLK_COMMA || key == SDLK_PERIOD) {
            const int gap = worker_.adjust_release_gap_eighths(key == SDLK_COMMA ? -1 : 1);
            status("Release Gap: " + std::to_string(gap) + "/8");
            return;
        }

        refresh_graph();
        Node *selected = selected_id_ ? graph_.find_node(*selected_id_) : nullptr;
        if (!selected) {
            const bool node_command = key == SDLK_c || key == SDLK_x || key == SDLK_u || key == SDLK_r || key == SDLK_m
                                      || key == SDLK_o || key == SDLK_p || key == SDLK_DELETE || key == SDLK_UP || key
                                      == SDLK_DOWN || key == SDLK_LEFT || key == SDLK_RIGHT;
            if (node_command) {
                status("Select a node first");
            }
            return;
        }

        const int id = selected->id;
        if (is_relay(*selected)) {
            if (key == SDLK_LEFT || key == SDLK_RIGHT) {
                status("Relay nodes do not have velocity");
                return;
            }
            if (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_p) {
                status("Relay nodes do not have pitches");
                return;
            }
            if (key == SDLK_m) {
                status("Relay nodes cannot be rests");
                return;
            }
        }

        try {
            if (key == SDLK_LEFT || key == SDLK_RIGHT) {
                int step = (mod & KMOD_SHIFT) ? 10 : 1;
                if (key == SDLK_LEFT) {
                    step = -step;
                }

                const int velocity = worker_.edit_graph([=](Graph &graph) {
                    return graph.adjust_velocity(id, step);
                });
                status("Node " + std::to_string(id) + " velocity: " + std::to_string(velocity));
            } else if (key == SDLK_UP || key == SDLK_DOWN) {
                int step = (mod & KMOD_SHIFT) ? 12 : 1;
                if (key == SDLK_DOWN) {
                    step = -step;
                }

                const bool last = (mod & KMOD_CTRL) != 0;
                const int pitch = worker_.edit_graph([=](Graph &graph) {
                    return last ? graph.transpose_last(id, step) : graph.transpose(id, step);
                });
                refresh_graph();
                const Node *after = graph_.find_node(id);

                if (last) {
                    status(
                        "Node " + std::to_string(id) + " last pitch (" +
                        std::to_string(after ? after->pitches.size() : 1) + "/6): " + pitch_name(pitch) + " (" +
                        std::to_string(pitch) + ")");
                } else {
                    status(
                        "Node " + std::to_string(id) + " first pitch: " + pitch_name(pitch) + " (" +
                        std::to_string(pitch) + ")");
                }
            } else if (key == SDLK_c) {
                connect_source_id_ = id;
                disconnect_source_id_.reset();
                status("Connect node " + std::to_string(id) + ": click a target node");
            } else if (key == SDLK_x) {
                disconnect_source_id_ = id;
                connect_source_id_.reset();
                status("Disconnect node " + std::to_string(id) + ": click a target node");
            } else if (key == SDLK_u) {
                const int removed = worker_.edit_graph([=](Graph &graph) {
                    return graph.remove_outgoing(id);
                });
                status(removed
                           ? "Removed " + std::to_string(removed) + " outgoing edge" + (removed == 1 ? "" : "s")
                           : "Node has no outgoing edges");
            } else if (key == SDLK_r) {
                worker_.edit_graph([=](Graph &graph) {
                    graph.set_start(id);
                });
                status("Start node set to " + std::to_string(id));
            } else if (key == SDLK_m) {
                const bool silenced = worker_.edit_graph([=](Graph &graph) {
                    return graph.toggle_silenced(id);
                });
                refresh_graph();
                const Node *after = graph_.find_node(id);
                const int pitch = after ? after->pitches.front() : 60;
                status("Node " + std::to_string(id) + ": " + (silenced
                                                                  ? "Rest"
                                                                  : pitch_name(pitch) + " (" + std::to_string(pitch) +
                                                                    ")"));
            } else if (key == SDLK_o) {
                const RoutingMode mode = worker_.toggle_routing_mode(id);
                refresh_graph();
                status("Node " + std::to_string(id) + " routing: " + (mode == RoutingMode::Counter
                                                                                 ? "Round-robin (creation order)"
                                                                                 : "Random"));
            } else if (key == SDLK_p) {
                if (mod & KMOD_SHIFT) {
                    const int removed = worker_.edit_graph([=](Graph &graph) {
                        return graph.remove_last_pitch(id);
                    });
                    refresh_graph();
                    const Node *after = graph_.find_node(id);
                    status(
                        "Node " + std::to_string(id) + " removed pitch: " + pitch_name(removed) + " (" +
                        std::to_string(removed) + "); " + std::to_string(after ? after->pitches.size() : 1) +
                        " remaining");
                } else {
                    const int pitch = worker_.edit_graph([=](Graph &graph) {
                        return graph.append_pitch(id);
                    });
                    refresh_graph();
                    const Node *after = graph_.find_node(id);
                    status(
                        "Node " + std::to_string(id) + " added pitch " +
                        std::to_string(after ? after->pitches.size() : 1) + "/6: " + pitch_name(pitch) + " (" +
                        std::to_string(pitch) + ")");
                }
            } else if (key == SDLK_DELETE) {
                const auto [was_playing, still_playing] = worker_.delete_node(id);
                selected_id_.reset();
                if (connect_source_id_ == id) {
                    connect_source_id_.reset();
                }
                if (disconnect_source_id_ == id) {
                    disconnect_source_id_.reset();
                }
                refresh_graph();

                if (was_playing && still_playing) {
                    status("Deleted node " + std::to_string(id) + "; playback continues");
                } else if (was_playing) {
                    status("Deleted next node " + std::to_string(id) + "; stopping after current note");
                } else {
                    status("Deleted node " + std::to_string(id));
                }
            }

            refresh_graph();
            mark_dirty();
        } catch (const std::exception &error) {
            status("Could not apply edit: " + std::string(error.what()));
        }
    }

    void SdlApp::handle_mouse_down(const SDL_MouseButtonEvent &event) {
        if (event.button == SDL_BUTTON_MIDDLE) {
            panning_ = true;
            return;
        }
        if (event.button != SDL_BUTTON_LEFT || event.y >= canvas_bottom()) {
            return;
        }

        refresh_graph();
        const Node *hit = node_hit(event.x, event.y);

        try {
            if (disconnect_source_id_) {
                if (!hit) {
                    status("Disconnect mode: click a target node");
                    return;
                }

                const int source = *disconnect_source_id_;
                worker_.disconnect_edge(source, hit->id);
                disconnect_source_id_.reset();
                refresh_graph();
                status("Removed edge " + std::to_string(source) + " -> " + std::to_string(hit->id));
                return;
            }

            if (connect_source_id_) {
                if (!hit) {
                    status("Connect mode: click another node");
                    return;
                }

                const int source = *connect_source_id_;
                const int target = hit->id;
                worker_.edit_graph([=](Graph &graph) {
                    graph.connect(source, target);
                });
                selected_id_ = target;
                connect_source_id_.reset();
                refresh_graph();
                status("Added edge " + std::to_string(source) + " -> " + std::to_string(target));
                return;
            }

            if (hit) {
                selected_id_ = hit->id;
                dragging_node_id_ = hit->id;
                mark_dirty();
                return;
            }

            const Point grid = screen_to_grid(event.x, event.y);
            const bool create_relay = (SDL_GetModState() & KMOD_SHIFT) != 0;
            const Node created = worker_.edit_graph([
                create_relay,
                grid,
                default_velocity = default_velocity_
            ](Graph &graph) {
                return create_relay
                           ? Node(graph.add_relay_node(grid.x, grid.y))
                           : Node(graph.add_node(grid.x, grid.y, 60, default_velocity));
            });
            selected_id_ = created.id;
            dragging_node_id_ = created.id;
            refresh_graph();
            if (is_relay(created)) {
                status("Created relay node " + std::to_string(created.id));
            } else {
                status(
                    "Created node " + std::to_string(created.id) + ": " + pitch_name(created.pitches.front()) + " (" +
                    std::to_string(created.pitches.front()) + ")");
            }
        } catch (const std::exception &error) {
            status("Could not update project: " + std::string(error.what()));
        }
    }

    void SdlApp::handle_mouse_motion(const SDL_MouseMotionEvent &event) {
        if (panning_ && (event.state & SDL_BUTTON_MMASK)) {
            camera_x_ += event.xrel;
            camera_y_ += event.yrel;
            mark_dirty();
            return;
        }
        if (!dragging_node_id_ || !(event.state & SDL_BUTTON_LMASK)) {
            return;
        }

        const Point grid = screen_to_grid(event.x, event.y);
        refresh_graph();
        const Node *node = graph_.find_node(*dragging_node_id_);
        if (!node || (node->x == grid.x && node->y == grid.y)) {
            return;
        }

        try {
            const int id = node->id;
            worker_.edit_graph([=](Graph &graph) {
                graph.move_node(id, grid.x, grid.y);
            });
            refresh_graph();
            mark_dirty();
        } catch (const std::exception &error) {
            status("Could not move node: " + std::string(error.what()));
        }
    }

    void SdlApp::pan_with_keys(double frame_seconds) {
        const Uint8 *keys = SDL_GetKeyboardState(nullptr);
        const int horizontal = static_cast<int>(keys[SDL_SCANCODE_A]) - static_cast<int>(keys[SDL_SCANCODE_D]);
        const int vertical = static_cast<int>(keys[SDL_SCANCODE_W]) - static_cast<int>(keys[SDL_SCANCODE_S]);

        if (!horizontal && !vertical) {
            return;
        }

        camera_x_ += horizontal * kPanSpeed * frame_seconds;
        camera_y_ += vertical * kPanSpeed * frame_seconds;
        mark_dirty();
    }

    void SdlApp::toggle_grid_spacing() {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        (void) height;

        // Keep the canvas centre anchored in world coordinates while zooming.
        const int old_pitch = grid_pitch();
        const double anchor_x = width / 2.0;
        const double anchor_y = canvas_bottom() / 2.0;
        grid_scale_ = grid_scale_ == 1 ? 2 : 1;
        const double ratio = static_cast<double>(grid_pitch()) / old_pitch;
        camera_x_ = anchor_x + (camera_x_ - anchor_x) * ratio;
        camera_y_ = anchor_y + (camera_y_ - anchor_y) * ratio;
        status("Grid spacing: " + std::to_string(grid_scale_) + "x");
    }

    void SdlApp::save_project() {
        try {
            refresh_graph();
            const TransportSnapshot transport = worker_.snapshot();
            graph_.save_json(
                project_file_,
                ProjectSettings{transport.bpm, transport.release_gap_eighths});
            status("Saved project to " + project_file_.string());
        } catch (const std::exception &error) {
            status("Could not save project: " + std::string(error.what()), 5.0);
        }
    }

    void SdlApp::load_project() {
        try {
            ProjectSettings settings;
            Graph loaded = Graph::load_json(project_file_, &settings);
            worker_.replace_project(loaded, settings);
            graph_ = std::move(loaded);
            selected_id_ = graph_.start_node_id();
            connect_source_id_.reset();
            disconnect_source_id_.reset();
            dragging_node_id_.reset();
            mark_dirty();
            status("Loaded project from " + project_file_.string());
        } catch (const std::exception &error) {
            status("Could not load project: " + std::string(error.what()), 5.0);
        }
    }

    void SdlApp::toggle_midi_clock_input() {
        if (midi_clock_input_enabled_) {
            disable_midi_clock_input();
            const auto transport = worker_.snapshot();
            status(transport.playing && transport.external_clock_active
                       ? "MIDI Clock input will disable"
                       : "MIDI Clock input disabled");
            return;
        }

        try {
            if (!clock_input_) {
                const ClockInputOpenResult result = clock_input_opener_();
                clock_input_ = result.backend;
                clock_input_connection_status_ = result.status;
                if (!clock_input_) {
                    status("Could not enable MIDI Clock input: " + result.status, 5.0);
                    return;
                }
                worker_.set_midi_clock_input(clock_input_);
            }

            if (worker_.snapshot().midi_clock_enabled) {
                worker_.toggle_midi_clock(false);
            }
            worker_.set_external_clock(true);
            midi_clock_input_enabled_ = true;
            midi_clock_input_status_ = clock_input_connection_status_;

            status(clock_input_connection_status_ +
                   (worker_.snapshot().playing ? "" : "; waiting for MIDI Clock/Start"));
        } catch (const std::exception &error) {
            clock_input_.reset();
            clock_input_connection_status_.clear();
            status("Could not enable MIDI Clock input: " + std::string(error.what()), 5.0);
        }
    }

    void SdlApp::disable_midi_clock_input() {
        if (midi_clock_input_enabled_) {
            worker_.set_external_clock(false);
        }
        midi_clock_input_enabled_ = false;
        midi_clock_input_status_ = "MIDI Clock input off";
    }

    void SdlApp::start_note_input() {
        try {
            const NoteInputOpenResult result = note_input_opener_(output_);
            if (!result.backend) {
                if (!result.status.empty()) {
                    status("MIDI note input unavailable: " + result.status, 5.0);
                }
                return;
            }

            auto worker = make_note_input_worker(result.backend);
            note_input_ = result.backend;
            note_input_worker_ = std::move(worker);
        } catch (const std::exception &error) {
            note_input_worker_.reset();
            note_input_.reset();
            status("MIDI note input unavailable: " + std::string(error.what()), 5.0);
        }
    }

    std::unique_ptr<MidiNoteInputWorker> SdlApp::make_note_input_worker(const std::shared_ptr<MidiNoteInput> &input) {
        return std::make_unique<MidiNoteInputWorker>(
            input, note_input_channel_ - 1,
            [this](const MidiNoteMessage &note) {
                SDL_Event event{};
                event.type = note_input_event_type_;
                event.user.code = (note.pitch & 0xff) | ((note.velocity & 0xff) << 8);
                if (SDL_PushEvent(&event) < 0) {
                    throw std::runtime_error(std::string("SDL_PushEvent: ") + SDL_GetError());
                }
            });
    }

    std::string SdlApp::reconnect_note_input(const std::shared_ptr<MidiOutput> &source_output) {
        if (note_input_worker_ && note_input_worker_->pop_failure()) {
            note_input_worker_.reset();
            note_input_.reset();
            SDL_FlushEvent(note_input_event_type_);
        }

        const bool had_active_input = note_input_worker_ && note_input_;
        try {
            const NoteInputOpenResult result = note_input_opener_(source_output);
            if (!result.backend) {
                const std::string detail = result.status.empty() ? "no source available" : result.status;
                return had_active_input
                           ? "Note In: unchanged (" + detail + ')'
                           : "Note In: unavailable (" + detail + ')';
            }

            auto replacement_input = result.backend;
            auto replacement_worker = make_note_input_worker(replacement_input);

            note_input_worker_.reset();
            note_input_.reset();
            SDL_FlushEvent(note_input_event_type_);
            note_input_ = std::move(replacement_input);
            note_input_worker_ = std::move(replacement_worker);
            return "Note In: OK";
        } catch (const std::exception &error) {
            return had_active_input
                       ? "Note In: unchanged (" + std::string(error.what()) + ')'
                       : "Note In: unavailable (" + std::string(error.what()) + ')';
        }
    }

    void SdlApp::consume_note_input_failure() {
        if (!note_input_worker_) {
            return;
        }

        const auto failure = note_input_worker_->pop_failure();
        if (!failure) {
            return;
        }

        note_input_worker_.reset();
        note_input_.reset();
        SDL_FlushEvent(note_input_event_type_);
        status("MIDI note input unavailable: " + *failure, 5.0);
    }

    void SdlApp::handle_midi_note_entry(int pitch, int velocity) {
        if (worker_.snapshot().state != TransportState::Stopped || !selected_id_) {
            return;
        }

        refresh_graph();
        const Node *node = graph_.find_node(*selected_id_);
        if (node == nullptr || node->type != NodeType::Musical) {
            return;
        }

        if (!worker_.set_node_from_midi(*selected_id_, pitch, velocity)) {
            return;
        }

        refresh_graph();
        status(
            "Node " + std::to_string(*selected_id_) + " first pitch: " + pitch_name(pitch) + " (" +
            std::to_string(pitch) + "), velocity: " + std::to_string(velocity));
    }

    void SdlApp::consume_transport_failures() {
        for (const TransportFailure &failure: worker_.pop_failures()) {
            if (failure.source == "midi_clock_input") {
                clock_input_.reset();
                clock_input_connection_status_.clear();
                midi_clock_input_enabled_ = false;
                status(
                    worker_.snapshot().playing
                        ? "MIDI Clock input lost; using internal clock: " + failure.message
                        : "MIDI Clock input lost: " + failure.message,
                    5.0);
            } else if (failure.source == "clock_lost") {
                midi_clock_input_enabled_ = false;
                status("MIDI Clock input timed out; using internal clock", 5.0);
            } else if (failure.source == "midi_output") {
                midi_failed(failure.message);
            } else if (failure.source == "timing_overrun") {
                status("Timing overrun; playback stopped: " + failure.message, 7.5);
            } else if (failure.source == "worker") {
                status("Timing engine failed: " + failure.message, 7.5);
            } else if (failure.source == "transport") {
                status("Playback stopped: " + failure.message, 5.0);
            } else {
                status(failure.message, 5.0);
            }
        }
    }

    void SdlApp::reconnect_midi() {
        std::string output_result;
        bool stopped_playback = false;
        try {
            OutputOpenResult result = output_opener_();
            if (!result.backend || !result.connected) {
                output_result = "Out: failed (" + result.status + ')';
            } else {
                if (worker_.snapshot().playing) {
                    // A backend cannot be replaced while notes may still be active
                    // on the old destination. Reconnecting during playback therefore
                    // performs the same all-notes-off cleanup as a device failure.
                    worker_.emergency_stop();
                    stopped_playback = true;
                }

                worker_.set_midi_backend(result.backend);
                output_ = std::move(result.backend);
                midi_status_ = result.status;
                output_result = "Out: OK";
            }
        } catch (const std::exception &error) {
            output_result = "Out: failed (" + std::string(error.what()) + ')';
        }

        const std::string note_input_result = reconnect_note_input(output_);
        status(
            "MIDI reconnect: " + output_result + "; " + note_input_result +
            (stopped_playback ? "; playback stopped" : ""),
            5.0);
    }

    void SdlApp::midi_failed(const std::string &message) {
        try {
            worker_.emergency_stop();
        } catch (...) {
        }

        output_ = std::make_shared<NullMidiOutput>();
        try {
            worker_.set_midi_backend(output_);
        } catch (...) {
        }

        midi_status_ = "No MIDI output";
        status("MIDI output unavailable: " + message, 5.0);
    }

    void SdlApp::refresh_graph() {
        graph_ = worker_.graph_snapshot();
    }

    void SdlApp::status(std::string message, double seconds) {
        status_message_ = std::move(message);
        status_until_ = monotonic_seconds() + seconds;
        ++status_revision_;
        mark_dirty();
    }

    double SdlApp::tempo_step(double bpm, int direction) noexcept {
        constexpr double step = 5.0;

        // External Clock can leave the displayed tempo between integer steps.
        // Move to the next 5-BPM boundary in the requested direction; when the
        // tempo is already on a boundary, move by one complete step.
        const double scaled = bpm / step;
        const double snapped = direction < 0
                                   ? std::ceil(scaled - 1e-9) - 1.0
                                   : std::floor(scaled + 1e-9) + 1.0;
        return std::clamp(snapped * step, static_cast<double>(kMinTempo),
                          static_cast<double>(kMaxTempo));
    }

    std::string SdlApp::visible_status(double now) const {
        return now < status_until_ ? status_message_ : midi_status_;
    }

    SdlApp::RenderState SdlApp::render_state(const TransportSnapshot &transport, double now) const {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_, &width, &height);

        return {
            .window_width = width, .window_height = height, .grid_scale = grid_scale_,
            .camera_x = static_cast<int>(std::lround(camera_x_)), .camera_y = static_cast<int>(std::lround(camera_y_)),
            .selected_id = selected_id_, .connect_source_id = connect_source_id_,
            .disconnect_source_id = disconnect_source_id_, .show_help = show_help_,
            .temporary_status_visible = now < status_until_, .status_revision = status_revision_,
            .current_node_id = transport.current_node_id, .rounded_bpm = static_cast<int>(std::lround(transport.bpm)),
            .midi_clock_enabled = transport.midi_clock_enabled, .midi_clock_active = transport.midi_clock_active,
            .external_clock_active = transport.external_clock_active, .transport_state = transport.state,
            .release_gap_eighths = transport.release_gap_eighths, .output_channel = transport.output_channel,
            .worker_alive = transport.worker_alive, .worker_responsive = transport.worker_responsive,
        };
    }

    bool SdlApp::redraw_if_needed(const TransportSnapshot &transport, double now) {
        const RenderState current_state = render_state(transport, now);
        if (!last_render_state_ || current_state != *last_render_state_) {
            dirty_ = true;
        }
        if (!dirty_) {
            return false;
        }

        // Resize events can arrive much faster than the renderer should redraw.
        // Keep the window responsive while limiting expensive text and graph work.
        const bool resizing = now < resize_active_until_;
        if (resizing && last_resize_redraw_at_ && now - *last_resize_redraw_at_ < kResizeRedrawInterval) {
            return false;
        }

        refresh_graph();
        draw(transport, now);
        SDL_RenderPresent(renderer_);
        dirty_ = false;
        last_render_state_ = current_state;
        last_resize_redraw_at_ = resizing ? std::optional<double>{now} : std::nullopt;
        return true;
    }

}
