#pragma once

#include "spatial_midi/core/graph.hpp"
#include "spatial_midi/core/midi_io.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace spatial_midi {
    [[nodiscard]] double sixteenth_seconds(double bpm);

    [[nodiscard]] double midi_clock_interval_seconds(double bpm);

    // The sequencer owns musical time and MIDI event ordering. It has no knowledge
    // of SDL and is called only by TransportWorker's dedicated thread.
    //
    // Internal timing stores absolute steady-clock deadlines in events_. External
    // MIDI Clock timing stores pulse positions in clock_events_. Release-gap Note Offs
    // that fall between external Clock pulses are temporarily mapped back to
    // steady-clock deadlines in armed_clock_releases_.
    class SequencerEngine {
    public:
        SequencerEngine(Graph &graph_model, MidiOutput &midi_output, double initial_bpm = kDefaultTempo,
                        int output_channel_index = 0,
                        int initial_release_gap_eighths = kDefaultReleaseGapEighths,
                        std::uint32_t random_seed = std::random_device{}());

        [[nodiscard]] bool playing() const noexcept;

        [[nodiscard]] bool external_clock_active() const noexcept;

        [[nodiscard]] bool midi_clock_active() const noexcept;

        [[nodiscard]] std::string clock_switch_pending() const;

        [[nodiscard]] std::size_t scheduled_event_count() const noexcept;

        [[nodiscard]] std::optional<double> next_real_deadline() const;

        [[nodiscard]] double external_clock_timeout() const noexcept;

        [[nodiscard]] std::optional<double> last_external_pulse_time() const noexcept;

        [[nodiscard]] TransportSnapshot snapshot() const;

        void set_tempo(double new_bpm, std::optional<double> now = std::nullopt);

        int set_release_gap_eighths(int eighths);

        int adjust_release_gap_eighths(int delta);

        bool toggle_midi_clock(std::optional<bool> enabled = std::nullopt, std::optional<double> now = std::nullopt);

        bool set_external_clock(bool enabled, std::optional<double> now = std::nullopt);

        bool force_internal_clock(std::optional<double> now = std::nullopt, bool clock_lost = false);

        void set_midi_backend(MidiOutput &midi_output);

        void reset_routing_counters(std::optional<int> node_id = std::nullopt);

        RoutingMode toggle_routing_mode(int node_id);

        Edge disconnect_edge(int source_id, int target_id);

        void start(std::optional<double> now = std::nullopt);

        bool pause(std::optional<double> now = std::nullopt);

        bool resume(std::optional<double> now = std::nullopt);

        int process(std::optional<double> now = std::nullopt, int max_events = 512);

        int process_external_message(std::uint8_t status, std::optional<double> now = std::nullopt);

        bool handle_node_deleted(int node_id);

        void stop();

        void emergency_stop() noexcept;

        void timing_overrun(double lateness);

        void mark_device_error() noexcept;

        Graph &graph;
        MidiOutput *midi;
        double bpm = kDefaultTempo;
        int output_channel = 0;
        int release_gap_eighths = kDefaultReleaseGapEighths;
        TransportState state = TransportState::Stopped;
        std::optional<int> current_node_id;
        std::set<int> active_pitches;
        bool midi_clock_enabled = false;
        bool external_clock_enabled = false;
        double max_event_lateness = 0.0;
        std::uint64_t missed_deadlines = 0;
        std::uint64_t input_gaps = 0;
        std::uint64_t overrun_count = 0;

    private:
        // Lower priority values are dispatched first at the same deadline. This
        // guarantees Note Off before Trigger, including a zero Release Gap (0/8).
        enum class EventKind {
            NoteOff,
            Finish,
            Trigger,
        };

        struct Event {
            double deadline = 0.0;
            int priority = 0;
            std::uint64_t sequence = 0;
            EventKind kind = EventKind::Trigger;
            int node_id = 0;
            std::vector<int> pitches;
            bool stop_after = false;
        };

        struct ArmedRelease {
            double real_deadline = 0.0;
            std::uint64_t sequence = 0;
            Event event;
        };

        static int event_priority(EventKind kind) noexcept;

        static bool event_later(const Event &lhs, const Event &rhs) noexcept;

        static bool release_later(const ArmedRelease &lhs, const ArmedRelease &rhs) noexcept;

        static void heap_push(std::vector<Event> &heap, Event event);

        static Event heap_pop(std::vector<Event> &heap);

        static void heap_rebuild(std::vector<Event> &heap);

        void release_push(ArmedRelease release);

        ArmedRelease release_pop();

        void push_event(double deadline, EventKind kind, int node_id, std::vector<int> pitches = {},
                        bool stop_after = false);

        void push_clock_event(double pulse_deadline, EventKind kind, int node_id, std::vector<int> pitches = {},
                              bool stop_after = false);

        void finish_playback();

        void clear_pause_state();

        void trigger_node(int node_id, double trigger_time);

        void trigger_node_external(int node_id, double trigger_pulse, double wire_time);

        std::optional<Edge> choose_outgoing_edge(int node_id, const std::vector<Edge> &outgoing);

        void sound_pitches(const std::vector<int> &pitches, int velocity, double deadline);

        void release_event_pitches(const Event &event, double deadline);

        void process_event(const Event &event, double wire_time, bool external);

        bool event_precedes_clock(const Event &event) const;

        void reset_external_clock_state();

        void switch_internal_to_external(double timestamp);

        void switch_external_to_internal(double timestamp);

        void release_active_after_clock_loss(double timestamp) noexcept;

        void record_deadline_lateness(double deadline, double now);

        void record_external_pulse(double source_timestamp);

        int process_armed_clock_releases(double now, int limit);

        int flush_clock_releases_at_pulse(double timestamp);

        int process_clock_events_at_pulse(double timestamp);

        void arm_subpulse_release(double pulse_time);

        double pulse_position_at(double timestamp) const;

        double time_at_pulse(std::int64_t pulse_index) const;

        void arm_clock_output_switch(bool enabled, double timestamp);

        void apply_clock_output_switch(double timestamp);

        void clear_clock_output_switch() noexcept;

        void start_clock_output(double timestamp, std::uint8_t transport_status, bool align_to_running_transport);

        void emit_clock_pulse(double deadline);

        void stop_clock_output(double deadline);

        bool external_clock_active_ = false;
        std::optional<std::string> external_switch_pending_;
        int external_alignment_pulses_ = 0;

        // Binary heaps ordered by deadline, priority, then insertion sequence.
        std::vector<Event> events_;
        std::vector<Event> clock_events_;
        std::vector<ArmedRelease> armed_clock_releases_;

        std::int64_t external_pulse_ = 0;
        std::deque<double> external_pulse_times_;
        double external_pulse_period_ = 0.0;
        std::uint64_t sequence_ = 0;

        // Epoch pair used to preserve Clock phase across live tempo changes.
        double tempo_epoch_time_ = 0.0;
        double tempo_epoch_pulse_ = 0.0;

        std::optional<std::int64_t> next_clock_pulse_index_;
        std::optional<double> next_clock_deadline_;
        bool clock_output_running_ = false;
        std::optional<bool> clock_output_switch_pending_;
        std::optional<std::int64_t> clock_output_switch_pulse_index_;
        std::optional<double> clock_output_switch_deadline_;

        std::unordered_map<int, std::size_t> counter_positions_;
        std::mt19937 random_;

        std::optional<double> paused_at_;
        TransportState state_before_pause_ = TransportState::Running;
        std::vector<int> paused_pitches_;
        int paused_velocity_ = 0;
        bool paused_clock_output_active_ = false;
    };
}
