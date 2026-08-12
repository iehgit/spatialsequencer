#include "spatial_midi/core/transport_worker.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>

namespace spatial_midi {
    namespace {
        std::shared_ptr<MidiOutput> require_output(std::shared_ptr<MidiOutput> output) {
            if (!output) {
                throw std::invalid_argument("TransportWorker requires a MIDI output");
            }
            return output;
        }

        constexpr double kInputPoll = 0.002;
        constexpr double kIdleInputPoll = 0.020;
        constexpr double kHeartbeat = 1.0;
        constexpr double kUnresponsive = 5.0;
        constexpr double kTimingOverrun = 5.0;

        // Events remain in the engine until their exact deadlines so tempo changes,
        // pause/resume, clock handoffs, and graph edits can still rescale or cancel them.
        // ALSA receives absolute timestamps at dispatch, preserving same-deadline ordering.
        constexpr double kSchedulingLead = 0.0;
    }

    TransportWorker::TransportWorker(Graph graph, std::shared_ptr<MidiOutput> output, double bpm, int output_channel)
        : graph_(std::move(graph)),
          output_(require_output(std::move(output))),
          engine_(graph_, *output_, bpm, output_channel) {
        const double now = monotonic_seconds();
        next_clock_input_poll_ = now;
        last_heartbeat_ = now;
        cached_snapshot_ = engine_.snapshot();
        cached_graph_ = graph_;
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
    }

    TransportWorker::~TransportWorker() {
        try {
            close();
        } catch (...) {
            // Destructors must not throw. Any normal shutdown error is already
            // exposed through the worker's failure queue while the object is alive.
        }
    }

    TransportSnapshot TransportWorker::snapshot() const {
        std::lock_guard lock(snapshot_mutex_);

        TransportSnapshot result = cached_snapshot_;
        const bool alive = worker_.joinable() && !closed_;
        result.worker_alive = alive;
        result.worker_responsive =
                alive && monotonic_seconds() - last_heartbeat_ <= kUnresponsive;
        return result;
    }

    Graph TransportWorker::graph_snapshot() const {
        std::lock_guard lock(snapshot_mutex_);
        return cached_graph_;
    }

    std::vector<TransportFailure> TransportWorker::pop_failures() {
        std::lock_guard lock(snapshot_mutex_);
        std::vector<TransportFailure> result(failures_.begin(), failures_.end());
        failures_.clear();
        return result;
    }

    void TransportWorker::start(std::optional<double> now) {
        submit([this, now] {
            engine_.start(now);
            if (engine_.external_clock_active()) {
                clock_watch_started_ = monotonic_seconds();
            }
        });
    }

    bool TransportWorker::pause(std::optional<double> now) {
        return submit([this, now] {
            const bool paused = engine_.pause(now);
            if (paused) {
                clock_watch_started_.reset();
                last_clock_arrival_.reset();
            }
            return paused;
        });
    }

    bool TransportWorker::resume(std::optional<double> now) {
        return submit([this, now] {
            const bool resumed = engine_.resume(now);
            if (resumed && engine_.external_clock_active()) {
                clock_watch_started_ = monotonic_seconds();
                last_clock_arrival_.reset();
            }
            return resumed;
        });
    }

    void TransportWorker::stop() {
        submit([this] { engine_.stop(); });
    }

    void TransportWorker::emergency_stop() {
        submit([this] { engine_.emergency_stop(); });
    }

    void TransportWorker::set_tempo(double bpm, std::optional<double> now) {
        submit([this, bpm, now] { engine_.set_tempo(bpm, now); });
    }

    int TransportWorker::set_release_gap_eighths(int eighths) {
        return submit([this, eighths] {
            return engine_.set_release_gap_eighths(eighths);
        });
    }

    int TransportWorker::adjust_release_gap_eighths(int delta) {
        return submit([this, delta] {
            return engine_.adjust_release_gap_eighths(delta);
        });
    }

    bool TransportWorker::toggle_midi_clock(std::optional<bool> enabled, std::optional<double> now) {
        return submit([this, enabled, now] {
            return engine_.toggle_midi_clock(enabled, now);
        });
    }

    bool TransportWorker::set_external_clock(bool enabled, std::optional<double> now) {
        return submit([this, enabled, now] {
            const bool result = engine_.set_external_clock(enabled, now);
            clock_input_enabled_ = result || engine_.external_clock_active();

            const double timestamp = monotonic_seconds();
            next_clock_input_poll_ = timestamp;
            clock_watch_started_ =
                    clock_input_enabled_ ? std::optional<double>{timestamp} : std::nullopt;
            last_clock_arrival_.reset();
            return result;
        });
    }

    void TransportWorker::set_midi_backend(std::shared_ptr<MidiOutput> output) {
        if (!output) {
            throw std::invalid_argument("MIDI output cannot be null");
        }

        submit([this, output = std::move(output)] {
            engine_.set_midi_backend(*output);
            output_ = output;
        });
    }

    void TransportWorker::set_midi_clock_input(std::shared_ptr<MidiClockInput> input) {
        submit([this, input = std::move(input)] {
            clock_input_ = input;
            next_clock_input_poll_ = monotonic_seconds();
        });
    }

    RoutingMode TransportWorker::toggle_routing_mode(int node_id) {
        return submit([this, node_id] {
            const RoutingMode mode = engine_.toggle_routing_mode(node_id);
            publish_snapshot(true);
            return mode;
        });
    }

    Edge TransportWorker::disconnect_edge(int source_id, int target_id) {
        return submit([this, source_id, target_id] {
            const Edge edge = engine_.disconnect_edge(source_id, target_id);
            publish_snapshot(true);
            return edge;
        });
    }

    std::pair<bool, bool> TransportWorker::delete_node(int node_id) {
        return submit([this, node_id] {
            const bool was_playing = engine_.playing();
            graph_.delete_node(node_id);
            const bool still_playing = engine_.handle_node_deleted(node_id);
            engine_.reset_routing_counters();
            publish_snapshot(true);
            return std::pair{was_playing, still_playing};
        });
    }

    bool TransportWorker::set_node_from_midi(int node_id, int pitch, int velocity) {
        return submit([this, node_id, pitch, velocity] {
            if (engine_.snapshot().state != TransportState::Stopped) {
                return false;
            }

            const Node *node = graph_.find_node(node_id);
            if (node == nullptr || node->type != NodeType::Musical) {
                return false;
            }

            graph_.set_primary_pitch_and_velocity(node_id, pitch, velocity);
            publish_snapshot(true);
            return true;
        });
    }

    void TransportWorker::replace_project(Graph graph, const ProjectSettings &settings) {
        submit([this, graph = std::move(graph), settings]() mutable {
            engine_.stop();
            graph_ = std::move(graph);
            engine_.reset_routing_counters();
            engine_.set_tempo(settings.bpm);
            engine_.set_release_gap_eighths(settings.release_gap_eighths);
            publish_snapshot(true);
        });
    }

    void TransportWorker::close() {
        {
            std::lock_guard lock(condition_mutex_);
            if (shutdown_) {
                return;
            }
            shutdown_ = true;
        }

        condition_.notify_all();
        worker_.request_stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void TransportWorker::run(std::stop_token stop_token) {
        try {
            while (!stop_token.stop_requested()) {
                // Drain every queued UI command before servicing time-sensitive
                // transport work. Commands are short and all execute on this thread.
                for (;;) {
                    std::function<void()> command;
                    {
                        std::lock_guard lock(condition_mutex_);
                        if (commands_.empty()) {
                            break;
                        }
                        command = std::move(commands_.front());
                        commands_.pop_front();
                    }

                    command();
                    publish_snapshot(false);
                }

                {
                    std::lock_guard lock(condition_mutex_);
                    if (shutdown_) {
                        break;
                    }
                }

                const double now = monotonic_seconds();
                service_transport(now);
                if (clock_input_ && now >= next_clock_input_poll_) {
                    poll_midi_clock_input(now);
                }
                check_clock_watchdog(monotonic_seconds());
                publish_snapshot(false);

                std::unique_lock lock(condition_mutex_);
                if (!commands_.empty() || shutdown_) {
                    continue;
                }

                condition_.wait_for(
                    lock,
                    stop_token,
                    std::chrono::duration<double>(
                        next_wait_timeout(monotonic_seconds())),
                    [this] { return shutdown_ || !commands_.empty(); });
            }
        } catch (const std::exception &error) {
            record_failure("worker", error.what());
        } catch (...) {
            record_failure("worker", "Unknown failure");
        }

        engine_.emergency_stop();
        publish_snapshot(false);

        std::lock_guard lock(snapshot_mutex_);
        closed_ = true;
    }

    void TransportWorker::service_transport(double now) {
        try {
            if (const auto deadline = engine_.next_real_deadline();
                deadline && now - *deadline > kTimingOverrun) {
                const double lateness = now - *deadline;
                engine_.timing_overrun(lateness);
                record_failure(
                    "timing_overrun",
                    "Transport stopped after a " + std::to_string(lateness) +
                    " s timing overrun");
                return;
            }

            (void) engine_.process(now + kSchedulingLead, 512);
        } catch (const GraphError &error) {
            engine_.emergency_stop();
            record_failure("transport", error.what());
        } catch (const std::exception &error) {
            engine_.mark_device_error();
            record_failure("midi_output", error.what());
        }
    }

    void TransportWorker::poll_midi_clock_input(double now) {
        const double interval = clock_input_enabled_ ? kInputPoll : kIdleInputPoll;
        next_clock_input_poll_ = now + interval;

        try {
            const auto messages = clock_input_->poll_realtime();
            if (!clock_input_enabled_) {
                return;
            }

            for (const MidiRealtimeMessage &message: messages) {
                (void) engine_.process_external_message(
                    message.status,
                    message.timestamp);

                // Kernel/device timestamps can be a fraction ahead of this poll's
                // sampled `now` because the clocks are calibrated independently.
                // Clamp only the watchdog reference; the engine receives the exact
                // timestamp so Clock interval estimation remains meaningful.
                const double watchdog_time = std::min(message.timestamp, now);
                if (message.status == kMidiTimingClock) {
                    last_clock_arrival_ = watchdog_time;
                }
                if (message.status == kMidiStart) {
                    clock_watch_started_ = watchdog_time;
                }
            }

            clock_input_enabled_ =
                    engine_.external_clock_enabled || engine_.external_clock_active();
            if (!clock_input_enabled_) {
                clock_watch_started_.reset();
                last_clock_arrival_.reset();
            }
        } catch (const std::exception &error) {
            const bool was_timing =
                    clock_input_enabled_ || engine_.external_clock_active();
            clock_input_.reset();
            clock_input_enabled_ = false;
            clock_watch_started_.reset();
            last_clock_arrival_.reset();

            if (was_timing) {
                engine_.force_internal_clock(now, true);
            }
            record_failure("midi_clock_input", error.what());
        }
    }

    void TransportWorker::check_clock_watchdog(double now) {
        if (!clock_input_enabled_ || !engine_.playing()) {
            return;
        }

        const auto reference =
                last_clock_arrival_ ? last_clock_arrival_ : clock_watch_started_;
        if (!reference) {
            clock_watch_started_ = now;
            return;
        }

        const double silence = now - *reference;
        if (silence <= engine_.external_clock_timeout()) {
            return;
        }

        ++engine_.input_gaps;
        engine_.force_internal_clock(now, true);
        clock_input_enabled_ = false;
        clock_watch_started_.reset();
        last_clock_arrival_.reset();
        record_failure(
            "clock_lost",
            "MIDI Clock silent for " + std::to_string(silence) + " s");
    }

    double TransportWorker::next_wait_timeout(double now) const {
        double deadline = now + kHeartbeat;

        if (const auto transport = engine_.next_real_deadline()) {
            deadline = std::min(deadline, *transport - kSchedulingLead);
        }
        if (clock_input_) {
            deadline = std::min(deadline, next_clock_input_poll_);
        }
        if (clock_input_enabled_ && engine_.playing()) {
            const auto reference =
                    last_clock_arrival_ ? last_clock_arrival_ : clock_watch_started_;
            if (reference) {
                deadline = std::min(
                    deadline,
                    *reference + engine_.external_clock_timeout());
            }
        }

        return std::clamp(deadline - now, 0.0, kHeartbeat);
    }

    void TransportWorker::publish_snapshot(bool graph_changed) {
        const TransportSnapshot snapshot = engine_.snapshot();

        std::lock_guard lock(snapshot_mutex_);
        cached_snapshot_ = snapshot;
        if (graph_changed) {
            cached_graph_ = graph_;
        }
        last_heartbeat_ = monotonic_seconds();
    }

    void TransportWorker::record_failure(std::string source, std::string message) {
        std::lock_guard lock(snapshot_mutex_);
        failures_.push_back(
            TransportFailure{std::move(source), std::move(message)});
    }

    void TransportWorker::reconcile_graph_edit(
        const std::vector<Edge> &before_edges,
        const std::vector<std::pair<int, RoutingMode> > &before_modes) {
        if (graph_.edges() != before_edges) {
            // Round-robin routing depends on outgoing-edge insertion order, so any edge
            // edit invalidates every saved counter position.
            engine_.reset_routing_counters();
        } else {
            for (const auto &[node_id, mode]: before_modes) {
                const Node *node = graph_.find_node(node_id);
                if (!node || node->routing_mode != mode) {
                    engine_.reset_routing_counters(node_id);
                }
            }
        }

        publish_snapshot(true);
    }
}
