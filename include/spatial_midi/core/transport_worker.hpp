#pragma once

#include "spatial_midi/core/sequencer_engine.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace spatial_midi {
    struct TransportFailure {
        std::string source;
        std::string message;
    };

    // Serializes graph edits, transport commands, MIDI Clock input, and MIDI output on a
    // single worker thread. SDL only reads published snapshots and submits commands;
    // it never calls the sequencer or MIDI backends directly.
    class TransportWorker {
    public:
        TransportWorker(Graph graph, std::shared_ptr<MidiOutput> output, double bpm = kDefaultTempo,
                        int output_channel = 0);

        ~TransportWorker();

        TransportWorker(const TransportWorker &) = delete;

        TransportWorker &operator=(const TransportWorker &) = delete;

        [[nodiscard]] TransportSnapshot snapshot() const;

        [[nodiscard]] Graph graph_snapshot() const;

        [[nodiscard]] std::vector<TransportFailure> pop_failures();

        void start(std::optional<TimePoint> now = std::nullopt);

        bool pause(std::optional<TimePoint> now = std::nullopt);

        bool resume(std::optional<TimePoint> now = std::nullopt);

        void stop();

        void emergency_stop();

        void set_tempo(double bpm, std::optional<TimePoint> now = std::nullopt);

        int set_release_gap_eighths(int eighths);

        int adjust_release_gap_eighths(int delta);

        bool toggle_midi_clock(std::optional<bool> enabled = std::nullopt, std::optional<TimePoint> now = std::nullopt);

        // Returns the engine's resulting intended state immediately; compare it
        // with `enabled` to determine whether the request was accepted.
        [[nodiscard]] bool set_external_clock(bool enabled, std::optional<TimePoint> now = std::nullopt);

        void set_midi_backend(std::shared_ptr<MidiOutput> output);

        void set_midi_clock_input(std::shared_ptr<MidiClockInput> input);

        RoutingMode toggle_routing_mode(int node_id);

        Edge disconnect_edge(int source_id, int target_id);

        std::pair<bool, bool> delete_node(int node_id);

        void replace_project(Graph graph, const ProjectSettings &settings);

        bool set_node_from_midi(int node_id, int pitch, int velocity);

        template<class Function>
        auto edit_graph(Function &&function) -> std::invoke_result_t<Function, Graph &> {
            using Result = std::invoke_result_t<Function, Graph &>;
            auto shared_function = std::make_shared<std::decay_t<Function> >(std::forward<Function>(function));

            return submit([this, shared_function]() mutable -> Result {
                const auto before_edges = graph_.edges();
                std::vector<std::pair<int, RoutingMode> > before_modes;
                before_modes.reserve(graph_.nodes().size());

                for (const Node &node: graph_.nodes()) {
                    before_modes.emplace_back(node.id, node.routing_mode);
                }

                if constexpr (std::is_void_v<Result>) {
                    std::invoke(*shared_function, graph_);
                    reconcile_graph_edit(before_edges, before_modes);
                } else {
                    Result result = std::invoke(*shared_function, graph_);
                    reconcile_graph_edit(before_edges, before_modes);
                    return result;
                }
            });
        }

        void close();

    private:
        // Commands submitted from SDL are packaged so exceptions and return values
        // cross the thread boundary safely. Calls originating on the worker execute
        // inline to avoid a self-deadlock.
        template<class Function>
        auto submit(Function &&function) -> std::invoke_result_t<Function> {
            using Result = std::invoke_result_t<Function>;

            if (std::this_thread::get_id() == worker_.get_id()) {
                return std::invoke(std::forward<Function>(function));
            }

            auto task = std::make_shared<std::packaged_task<Result()> >(std::forward<Function>(function));
            auto future = task->get_future();

            {
                std::lock_guard lock(condition_mutex_);
                if (shutdown_) {
                    throw std::runtime_error("Transport worker is closed");
                }
                commands_.emplace_back([task] { (*task)(); });
            }

            condition_.notify_one();
            if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
                throw std::runtime_error("Transport worker command timed out");
            }

            if constexpr (std::is_void_v<Result>) {
                future.get();
            } else {
                return future.get();
            }
        }

        void run(std::stop_token stop_token);

        void service_transport(TimePoint now);

        void poll_midi_clock_input(TimePoint now);

        void check_clock_watchdog(TimePoint now);

        Nanoseconds next_wait_timeout(TimePoint now) const;

        void publish_snapshot(bool graph_changed = false);

        void record_failure(std::string source, std::string message);

        void reconcile_graph_edit(const std::vector<Edge> &before_edges,
                                  const std::vector<std::pair<int, RoutingMode> > &before_modes);

        // snapshot_mutex_ protects only values published to the UI. The worker does
        // not hold it while touching MIDI or running the engine.
        mutable std::mutex snapshot_mutex_;
        TransportSnapshot cached_snapshot_;
        Graph cached_graph_;
        TimePoint last_heartbeat_{};
        std::deque<TransportFailure> failures_;

        // condition_mutex_ protects the command queue and shutdown flag.
        mutable std::mutex condition_mutex_;
        std::condition_variable_any condition_;
        std::deque<std::function<void()> > commands_;
        bool shutdown_ = false;
        bool closed_ = false;

        // Everything below is worker-thread-owned after construction.
        Graph graph_;
        std::shared_ptr<MidiOutput> output_;
        std::shared_ptr<MidiClockInput> clock_input_;
        SequencerEngine engine_;
        bool clock_input_enabled_ = false;
        TimePoint next_clock_input_poll_{};
        std::optional<TimePoint> clock_watch_started_;
        std::optional<TimePoint> last_clock_arrival_;
        std::jthread worker_;
    };
}
