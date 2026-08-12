#include "spatial_midi/core/sequencer_engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace spatial_midi {
    namespace {
        constexpr double kDeadlineEpsilon = 1e-9;
        constexpr double kMissedDeadlineSeconds = 0.010;
        constexpr std::size_t kExternalEstimateIntervals = 48;
        constexpr std::size_t kExternalEstimateMinPulses = 6;
        constexpr double kExternalResetGap = 0.5;
        constexpr double kExternalTimeoutMin = 2.0;
        constexpr int kMidiClockPulsesPerQuarter = kMidiClockPulsesPerSixteenth * 4;
        constexpr int kExternalTimeoutPulses = kMidiClockPulsesPerQuarter;
    }

    double sixteenth_seconds(double bpm) {
        if (!std::isfinite(bpm) || bpm < kMinTempo || bpm > kMaxTempo) {
            throw std::invalid_argument("Tempo must be between 20 and 300 BPM");
        }
        return 60.0 / bpm / 4.0;
    }

    double midi_clock_interval_seconds(double bpm) {
        (void) sixteenth_seconds(bpm);
        return 60.0 / bpm / kMidiClockPulsesPerQuarter;
    }

    SequencerEngine::SequencerEngine(
        Graph &graph_model,
        MidiOutput &midi_output,
        double initial_bpm,
        int output_channel_index,
        int initial_release_gap_eighths,
        std::uint32_t random_seed)
        : graph(graph_model),
          midi(&midi_output),
          bpm(initial_bpm),
          output_channel(output_channel_index),
          release_gap_eighths(initial_release_gap_eighths),
          external_pulse_period_(midi_clock_interval_seconds(initial_bpm)),
          random_(random_seed) {
        (void) sixteenth_seconds(bpm);

        if (output_channel < 0 || output_channel > 15) {
            throw std::invalid_argument(
                "MIDI output channel index must be between 0 and 15");
        }
        if (initial_release_gap_eighths < kMinReleaseGapEighths ||
            initial_release_gap_eighths > kMaxReleaseGapEighths) {
            throw std::invalid_argument(
                "Release Gap must be between 0/8 and 4/8");
        }
    }

    bool SequencerEngine::playing() const noexcept {
        return state == TransportState::Running ||
               state == TransportState::ClockLost;
    }

    bool SequencerEngine::external_clock_active() const noexcept {
        return external_clock_active_ &&
               (playing() || state == TransportState::Paused);
    }

    bool SequencerEngine::midi_clock_active() const noexcept {
        return playing() && clock_output_running_;
    }

    std::size_t SequencerEngine::scheduled_event_count() const noexcept {
        return events_.size() + clock_events_.size() +
               armed_clock_releases_.size();
    }

    std::optional<double> SequencerEngine::next_real_deadline() const {
        if (!playing()) {
            return std::nullopt;
        }

        // Pulse-domain events cannot be predicted until the next external Clock
        // message. Only Release-gap Note Offs already armed in real time have deadlines.
        if (external_clock_active()) {
            if (armed_clock_releases_.empty()) {
                return std::nullopt;
            }
            return armed_clock_releases_.front().real_deadline;
        }

        std::optional<double> result;
        const auto consider = [&result](std::optional<double> value) {
            if (value && (!result || *value < *result)) {
                result = value;
            }
        };

        if (!events_.empty()) {
            consider(events_.front().deadline);
        }
        if (clock_output_running_) {
            consider(next_clock_deadline_);
        }
        consider(clock_output_switch_deadline_);
        return result;
    }

    double SequencerEngine::external_clock_timeout() const noexcept {
        return std::max(
            kExternalTimeoutMin,
            kExternalTimeoutPulses * external_pulse_period_);
    }

    std::optional<double> SequencerEngine::last_external_pulse_time() const noexcept {
        if (external_pulse_times_.empty()) {
            return std::nullopt;
        }
        return external_pulse_times_.back();
    }

    TransportSnapshot SequencerEngine::snapshot() const {
        return TransportSnapshot{
            .playing = playing(),
            .current_node_id = current_node_id,
            .bpm = bpm,
            .midi_clock_enabled = midi_clock_enabled,
            .midi_clock_active = midi_clock_active(),
            .midi_clock_output_switch_pending = clock_output_switch_pending_,
            .external_clock_enabled = external_clock_enabled,
            .output_channel = output_channel,
            .release_gap_eighths = release_gap_eighths,
            .external_clock_active = external_clock_active(),
            .external_clock_switch_pending = external_switch_pending_,
            .state = state,
            .max_event_lateness_ms = max_event_lateness * 1000.0,
            .missed_deadlines = missed_deadlines,
            .input_gaps = input_gaps,
            .overrun_count = overrun_count,
        };
    }

    int SequencerEngine::event_priority(EventKind kind) noexcept {
        return kind == EventKind::Trigger ? 2 : 1;
    }

    bool SequencerEngine::event_later(const Event &lhs, const Event &rhs) noexcept {
        if (lhs.deadline != rhs.deadline) {
            return lhs.deadline > rhs.deadline;
        }
        if (lhs.priority != rhs.priority) {
            return lhs.priority > rhs.priority;
        }
        return lhs.sequence > rhs.sequence;
    }

    bool SequencerEngine::release_later(const ArmedRelease &lhs, const ArmedRelease &rhs) noexcept {
        if (lhs.real_deadline != rhs.real_deadline) {
            return lhs.real_deadline > rhs.real_deadline;
        }
        return lhs.sequence > rhs.sequence;
    }

    void SequencerEngine::heap_push(std::vector<Event> &heap, Event event) {
        heap.push_back(std::move(event));
        std::ranges::push_heap(heap, event_later);
    }

    SequencerEngine::Event SequencerEngine::heap_pop(std::vector<Event> &heap) {
        std::ranges::pop_heap(heap, event_later);
        Event event = std::move(heap.back());
        heap.pop_back();
        return event;
    }

    void SequencerEngine::heap_rebuild(std::vector<Event> &heap) {
        std::ranges::make_heap(heap, event_later);
    }

    void SequencerEngine::release_push(ArmedRelease release) {
        armed_clock_releases_.push_back(std::move(release));
        std::ranges::push_heap(armed_clock_releases_, release_later);
    }

    SequencerEngine::ArmedRelease SequencerEngine::release_pop() {
        std::ranges::pop_heap(armed_clock_releases_, release_later);
        ArmedRelease release = std::move(armed_clock_releases_.back());
        armed_clock_releases_.pop_back();
        return release;
    }

    void SequencerEngine::push_event(
        double deadline,
        EventKind kind,
        int node_id,
        std::vector<int> pitches,
        bool stop_after) {
        heap_push(
            events_,
            Event{
                deadline,
                event_priority(kind),
                ++sequence_,
                kind,
                node_id,
                std::move(pitches),
                stop_after,
            });
    }

    void SequencerEngine::push_clock_event(
        double pulse_deadline,
        EventKind kind,
        int node_id,
        std::vector<int> pitches,
        bool stop_after) {
        heap_push(
            clock_events_,
            Event{
                pulse_deadline,
                event_priority(kind),
                ++sequence_,
                kind,
                node_id,
                std::move(pitches),
                stop_after,
            });
    }

    void SequencerEngine::set_tempo(double new_bpm, std::optional<double> now) {
        (void) sixteenth_seconds(new_bpm);

        const double change_time = now.value_or(monotonic_seconds());
        const double phase_time =
                state == TransportState::Paused && paused_at_
                    ? *paused_at_
                    : change_time;
        const double pulse_position = pulse_position_at(phase_time);

        // Scale remaining absolute deadlines around the tempo-change instant to
        // preserve the current step's fractional progress.
        if ((playing() || state == TransportState::Paused) && !events_.empty()) {
            const double scale = bpm / new_bpm;
            for (Event &event: events_) {
                if (event.deadline > phase_time) {
                    event.deadline =
                            phase_time + (event.deadline - phase_time) * scale;
                }
            }
            heap_rebuild(events_);
        }

        tempo_epoch_time_ = phase_time;
        tempo_epoch_pulse_ = pulse_position;
        bpm = new_bpm;

        if (external_pulse_times_.empty()) {
            external_pulse_period_ = midi_clock_interval_seconds(new_bpm);
        }
        if (clock_output_running_ && next_clock_pulse_index_) {
            next_clock_deadline_ = time_at_pulse(*next_clock_pulse_index_);
        }
        if (clock_output_switch_pulse_index_) {
            clock_output_switch_deadline_ =
                    time_at_pulse(*clock_output_switch_pulse_index_);
        }
    }

    int SequencerEngine::set_release_gap_eighths(int eighths) {
        release_gap_eighths = std::clamp(
            eighths,
            kMinReleaseGapEighths,
            kMaxReleaseGapEighths);
        return release_gap_eighths;
    }

    int SequencerEngine::adjust_release_gap_eighths(int delta) {
        return set_release_gap_eighths(release_gap_eighths + delta);
    }

    bool SequencerEngine::toggle_midi_clock(std::optional<bool> enabled, std::optional<double> now) {
        const bool new_state = enabled.value_or(!midi_clock_enabled);
        if (new_state == midi_clock_enabled) {
            return new_state;
        }
        if (new_state && external_clock_enabled) {
            throw std::runtime_error(
                "MIDI Clock input and output are mutually exclusive");
        }

        midi_clock_enabled = new_state;
        if (state == TransportState::Paused) {
            clear_clock_output_switch();
            if (!new_state) {
                paused_clock_output_active_ = false;
                stop_clock_output(now.value_or(monotonic_seconds()));
            }
            return new_state;
        }

        if (!playing()) {
            clear_clock_output_switch();
            if (!new_state) {
                stop_clock_output(now.value_or(monotonic_seconds()));
            }
            return new_state;
        }

        const double timestamp = now.value_or(monotonic_seconds());
        if (new_state) {
            arm_clock_output_switch(true, timestamp);
        } else if (clock_output_running_) {
            arm_clock_output_switch(false, timestamp);
        } else {
            clear_clock_output_switch();
        }
        return midi_clock_enabled;
    }

    bool SequencerEngine::set_external_clock(bool enabled, std::optional<double> now) {
        if (enabled == external_clock_enabled) {
            return enabled;
        }

        const double timestamp = now.value_or(monotonic_seconds());
        if (enabled && midi_clock_enabled) {
            (void) toggle_midi_clock(false, timestamp);
        }

        external_clock_enabled = enabled;
        if (enabled) {
            external_alignment_pulses_ = 0;
            external_pulse_times_.clear();

            if (state == TransportState::Paused || playing()) {
                external_switch_pending_ = ExternalClockSwitch::ToExternal;
            } else {
                external_switch_pending_.reset();
                external_clock_active_ = true;
                reset_external_clock_state();
                state = TransportState::WaitingForClock;
            }
        } else if (state == TransportState::Paused && external_clock_active_) {
            external_switch_pending_ = ExternalClockSwitch::ToInternal;
        } else if (external_clock_active() && playing()) {
            external_switch_pending_ = ExternalClockSwitch::ToInternal;
        } else {
            external_switch_pending_.reset();
            external_clock_active_ = false;
            reset_external_clock_state();

            if (state == TransportState::WaitingForClock) {
                state = TransportState::Stopped;
            } else if (state == TransportState::ClockLost) {
                state = TransportState::Running;
            }
        }
        return enabled;
    }

    bool SequencerEngine::force_internal_clock(std::optional<double> now, bool clock_lost) {
        const double timestamp = now.value_or(monotonic_seconds());
        const bool was_playing = playing();
        const bool was_paused = state == TransportState::Paused;

        if (external_clock_active() && (was_playing || was_paused)) {
            const double handoff_time =
                    was_paused && paused_at_ ? *paused_at_ : timestamp;
            switch_external_to_internal(handoff_time);
        }

        external_clock_enabled = false;
        external_clock_active_ = false;
        external_switch_pending_.reset();
        external_alignment_pulses_ = 0;

        if (was_paused) {
            state = TransportState::Paused;
            state_before_pause_ = clock_lost
                                      ? TransportState::ClockLost
                                      : TransportState::Running;
        } else if (was_playing) {
            if (clock_lost) {
                release_active_after_clock_loss(timestamp);
            }
            state = clock_lost
                        ? TransportState::ClockLost
                        : TransportState::Running;
        } else if (state == TransportState::WaitingForClock) {
            state = TransportState::Stopped;
        }

        // Callers use the return value to distinguish a running handoff from
        // disabling external Clock while transport was stopped or paused.
        return was_playing;
    }

    void SequencerEngine::set_midi_backend(MidiOutput &midi_output) {
        if (playing() || !active_pitches.empty()) {
            throw std::runtime_error(
                "Stop playback before replacing the MIDI backend");
        }
        midi = &midi_output;
    }

    void SequencerEngine::reset_routing_counters(std::optional<int> node_id) {
        if (node_id) {
            counter_positions_.erase(*node_id);
        } else {
            counter_positions_.clear();
        }
    }

    RoutingMode SequencerEngine::toggle_routing_mode(int node_id) {
        const RoutingMode mode = graph.toggle_routing_mode(node_id);
        reset_routing_counters(node_id);
        return mode;
    }

    Edge SequencerEngine::disconnect_edge(int source_id, int target_id) {
        Edge edge = graph.disconnect(source_id, target_id);
        reset_routing_counters(source_id);
        return edge;
    }

    void SequencerEngine::start(std::optional<double> now) {
        const auto start_id = graph.start_node_id();
        if (!start_id || graph.find_node(*start_id) == nullptr) {
            throw GraphError("Choose a start node before playback");
        }

        stop();
        const double trigger_time = now.value_or(monotonic_seconds());
        tempo_epoch_time_ = trigger_time;
        tempo_epoch_pulse_ = 0.0;
        state = TransportState::Running;
        external_clock_active_ = external_clock_enabled;
        external_switch_pending_.reset();
        clear_clock_output_switch();

        if (midi_clock_enabled) {
            start_clock_output(trigger_time, kMidiStart, false);
        }

        if (external_clock_active()) {
            reset_external_clock_state();
            trigger_node_external(*start_id, 0.0, trigger_time);
        } else {
            trigger_node(*start_id, trigger_time);
            if (clock_output_running_) {
                emit_clock_pulse(trigger_time);
            }
        }
    }

    bool SequencerEngine::pause(std::optional<double> now) {
        if (!playing()) {
            return false;
        }

        const double timestamp = now.value_or(monotonic_seconds());
        (void) process(timestamp, 512);
        if (!playing()) {
            return false;
        }

        midi->clear_scheduled();
        paused_at_ = timestamp;
        state_before_pause_ = state;
        paused_pitches_.assign(active_pitches.begin(), active_pitches.end());

        if (const Node *node =
                current_node_id ? graph.find_node(*current_node_id) : nullptr) {
            paused_velocity_ = node->velocity;
        } else {
            paused_velocity_ = 0;
        }

        paused_clock_output_active_ = clock_output_running_;
        state = TransportState::Paused;

        if (clock_output_running_) {
            clock_output_running_ = false;
            midi->send_realtime(kMidiStop, timestamp);
        }
        if (!active_pitches.empty()) {
            midi->notes_off(paused_pitches_, 0, output_channel, timestamp);
        }

        active_pitches.clear();
        midi->all_notes_off(output_channel);
        return true;
    }

    bool SequencerEngine::resume(std::optional<double> now) {
        if (state != TransportState::Paused || !paused_at_) {
            return false;
        }

        const double timestamp = now.value_or(monotonic_seconds());
        const double paused_duration = std::max(0.0, timestamp - *paused_at_);

        for (Event &event: events_) {
            event.deadline += paused_duration;
        }
        heap_rebuild(events_);

        for (ArmedRelease &release: armed_clock_releases_) {
            release.real_deadline += paused_duration;
        }
        std::ranges::make_heap(armed_clock_releases_, release_later);

        tempo_epoch_time_ += paused_duration;
        if (next_clock_pulse_index_) {
            next_clock_deadline_ = time_at_pulse(*next_clock_pulse_index_);
        }
        if (clock_output_switch_pulse_index_) {
            clock_output_switch_deadline_ =
                    time_at_pulse(*clock_output_switch_pulse_index_);
        }

        const auto pitches = paused_pitches_;
        const int velocity = paused_velocity_;
        const bool clock_was_active = paused_clock_output_active_;
        state = state_before_pause_;

        if (clock_was_active && midi_clock_enabled) {
            midi->send_realtime(kMidiContinue, timestamp);
            clock_output_running_ = true;
        } else if (
            midi_clock_enabled &&
            !external_clock_active_ &&
            !clock_output_switch_pending_) {
            arm_clock_output_switch(true, timestamp);
        }

        if (!pitches.empty() &&
            current_node_id &&
            graph.find_node(*current_node_id)) {
            sound_pitches(pitches, velocity, timestamp);
        }

        clear_pause_state();
        return true;
    }

    int SequencerEngine::process(std::optional<double> now, int max_events) {
        const double current_time = now.value_or(monotonic_seconds());
        if (max_events <= 0) {
            return 0;
        }
        if (external_clock_active()) {
            return process_armed_clock_releases(current_time, max_events);
        }

        int processed = 0;
        while (playing() && processed < max_events) {
            constexpr double no_deadline = std::numeric_limits<double>::infinity();
            const Event *event = events_.empty() ? nullptr : &events_.front();

            // Keep the three candidate deadlines explicit so equal-time precedence
            // remains visible and auditable below.
            const bool has_transport_deadline = event != nullptr;
            const bool has_clock_deadline =
                    clock_output_running_ && next_clock_deadline_.has_value();
            const bool has_switch_deadline =
                    clock_output_switch_deadline_.has_value();

            const double transport_deadline = has_transport_deadline
                                                  ? event->deadline
                                                  : no_deadline;
            const double clock_deadline = has_clock_deadline
                                              ? *next_clock_deadline_
                                              : no_deadline;
            const double switch_deadline = has_switch_deadline
                                               ? *clock_output_switch_deadline_
                                               : no_deadline;

            const double due = std::min({
                transport_deadline,
                clock_deadline,
                switch_deadline,
            });
            if (!std::isfinite(due) || due > current_time) {
                break;
            }

            // At an equal deadline, non-terminal Note Offs and valid Triggers
            // precede MIDI Clock. Terminal or invalid transport events yield to the
            // final Clock pulse before playback cleanup stops Clock output.
            const bool transport_precedes =
                    event &&
                    (!has_clock_deadline ||
                     event->deadline < clock_deadline - kDeadlineEpsilon ||
                     (std::abs(event->deadline - clock_deadline) <=
                      kDeadlineEpsilon &&
                      event_precedes_clock(*event))) &&
                    (!has_switch_deadline ||
                     event->deadline <= switch_deadline + kDeadlineEpsilon);

            if (transport_precedes) {
                Event popped = heap_pop(events_);
                record_deadline_lateness(popped.deadline, current_time);
                process_event(popped, popped.deadline, false);
                ++processed;
                continue;
            }

            if (has_clock_deadline &&
                (!has_switch_deadline ||
                 clock_deadline <= switch_deadline + kDeadlineEpsilon) &&
                (!has_transport_deadline ||
                 clock_deadline <= transport_deadline + kDeadlineEpsilon)) {
                record_deadline_lateness(clock_deadline, current_time);
                emit_clock_pulse(clock_deadline);
                ++processed;
                continue;
            }

            if (has_switch_deadline &&
                (!has_transport_deadline ||
                 switch_deadline <= transport_deadline + kDeadlineEpsilon)) {
                record_deadline_lateness(switch_deadline, current_time);
                apply_clock_output_switch(switch_deadline);
                ++processed;
                continue;
            }

            Event popped = heap_pop(events_);
            record_deadline_lateness(popped.deadline, current_time);
            process_event(popped, popped.deadline, false);
            ++processed;
        }

        return processed;
    }

    bool SequencerEngine::event_precedes_clock(const Event &event) const {
        if (event.kind == EventKind::NoteOff) {
            return !event.stop_after;
        }
        if (event.kind == EventKind::Trigger) {
            return graph.find_node(event.node_id) != nullptr;
        }
        return false;
    }

    void SequencerEngine::process_event(const Event &event, double wire_time, bool external) {
        if (event.kind == EventKind::NoteOff) {
            release_event_pitches(event, wire_time);
        } else if (event.kind == EventKind::Trigger) {
            if (!graph.find_node(event.node_id)) {
                finish_playback();
            } else if (external) {
                trigger_node_external(event.node_id, event.deadline, wire_time);
            } else {
                trigger_node(event.node_id, event.deadline);
            }
        } else {
            finish_playback();
        }
    }

    int SequencerEngine::process_external_message(std::uint8_t status, std::optional<double> now) {
        if (!external_clock_enabled && !external_clock_active()) {
            return 0;
        }

        const double timestamp = now.value_or(monotonic_seconds());
        if (status == kMidiContinue) {
            return 0;
        }

        if (status == kMidiStart) {
            if (playing() && external_switch_pending_ == ExternalClockSwitch::ToExternal) {
                record_external_pulse(timestamp);
                switch_internal_to_external(timestamp);
            } else if (!playing()) {
                start(timestamp);
            }
            return 1;
        }

        if (status == kMidiStop) {
            if (external_clock_active() &&
                (playing() || !active_pitches.empty())) {
                stop();
                return 1;
            }
            return 0;
        }

        if (status != kMidiTimingClock) {
            return 0;
        }

        record_external_pulse(timestamp);
        if (!playing()) {
            return 0;
        }

        if (external_switch_pending_ == ExternalClockSwitch::ToExternal) {
            ++external_alignment_pulses_;
            if (external_alignment_pulses_ >= kMidiClockPulsesPerSixteenth) {
                switch_internal_to_external(timestamp);
                return 1;
            }
            return 0;
        }

        if (!external_clock_active()) {
            return 0;
        }

        ++external_pulse_;
        int processed =
                flush_clock_releases_at_pulse(timestamp) +
                process_clock_events_at_pulse(timestamp);

        if (external_switch_pending_ == ExternalClockSwitch::ToInternal &&
            external_pulse_ % kMidiClockPulsesPerSixteenth == 0) {
            switch_external_to_internal(timestamp);
            external_switch_pending_.reset();
            return processed + 1;
        }

        arm_subpulse_release(timestamp);
        return processed;
    }

    bool SequencerEngine::handle_node_deleted(int node_id) {
        if (current_node_id == node_id) {
            current_node_id.reset();
        }
        if (!playing()) {
            return false;
        }

        const auto handle =
                [this, node_id](std::vector<Event> &heap, bool clock_domain) {
            std::vector<double> removed;
            std::vector<Event> retained;

            for (Event &event: heap) {
                if (event.kind == EventKind::Trigger &&
                    event.node_id == node_id) {
                    removed.push_back(event.deadline);
                } else {
                    retained.push_back(std::move(event));
                }
            }

            if (removed.empty()) {
                return true;
            }

            heap = std::move(retained);
            heap_rebuild(heap);

            // Let the current note finish naturally, then stop instead of
            // traversing into a node that no longer exists.
            bool found_note_off = false;
            for (Event &event: heap) {
                if (event.kind == EventKind::NoteOff) {
                    event.stop_after = true;
                    found_note_off = true;
                }
            }
            if (clock_domain) {
                for (ArmedRelease &release: armed_clock_releases_) {
                    if (release.event.kind == EventKind::NoteOff) {
                        release.event.stop_after = true;
                        found_note_off = true;
                    }
                }
            }

            if (!found_note_off) {
                const double deadline =
                        std::ranges::min(removed);
                const int current = current_node_id.value_or(node_id);
                if (clock_domain) {
                    push_clock_event(deadline, EventKind::Finish, current);
                } else {
                    push_event(deadline, EventKind::Finish, current);
                }
            }
            return false;
        };

        return external_clock_active()
                   ? handle(clock_events_, true)
                   : handle(events_, false);
    }

    void SequencerEngine::stop() {
        events_.clear();
        clock_events_.clear();
        armed_clock_releases_.clear();
        reset_routing_counters();

        state = external_clock_enabled
                    ? TransportState::WaitingForClock
                    : TransportState::Stopped;
        current_node_id.reset();
        clear_pause_state();
        external_switch_pending_.reset();
        clear_clock_output_switch();

        const double now = monotonic_seconds();
        midi->clear_scheduled();
        stop_clock_output(now);

        if (!active_pitches.empty()) {
            std::vector<int> pitches(
                active_pitches.begin(),
                active_pitches.end());
            midi->notes_off(pitches, 0, output_channel, now);
        }
        active_pitches.clear();
        midi->all_notes_off(output_channel);
    }

    void SequencerEngine::emergency_stop() noexcept {
        try {
            stop();
        } catch (...) {
            // Best effort during teardown or device failure.
        }
    }

    void SequencerEngine::timing_overrun(double lateness) {
        ++overrun_count;
        max_event_lateness = std::max(max_event_lateness, lateness);
        ++missed_deadlines;
        try {
            stop();
        } catch (...) {
        }
        state = TransportState::TimingOverrun;
    }

    void SequencerEngine::mark_device_error() noexcept {
        emergency_stop();
        state = TransportState::DeviceError;
    }

    void SequencerEngine::finish_playback() {
        events_.clear();
        clock_events_.clear();
        armed_clock_releases_.clear();
        reset_routing_counters();

        state = external_clock_enabled
                    ? TransportState::WaitingForClock
                    : TransportState::Stopped;
        current_node_id.reset();
        external_switch_pending_.reset();
        clear_clock_output_switch();
        stop_clock_output(monotonic_seconds());
    }

    void SequencerEngine::clear_pause_state() {
        paused_at_.reset();
        state_before_pause_ = TransportState::Running;
        paused_pitches_.clear();
        paused_velocity_ = 0;
        paused_clock_output_active_ = false;
    }

    void SequencerEngine::trigger_node(int node_id, double trigger_time) {
        const Node *node = graph.find_node(node_id);
        if (!node) {
            finish_playback();
            return;
        }

        const auto outgoing = graph.outgoing_edges(node_id);
        const auto edge = choose_outgoing_edge(node_id, outgoing);

        if (is_relay(*node)) {
            // A relay is transparent in musical time. Its incoming edge has
            // already consumed time; its selected outgoing edge contributes none.
            current_node_id.reset();
            if (!edge) {
                finish_playback();
            } else {
                trigger_node(edge->target_id, trigger_time);
            }
            return;
        }

        current_node_id = node_id;
        const double tick = sixteenth_seconds(bpm);
        const std::vector<int> pitches = node->silenced
                                             ? std::vector<int>{}
                                             : node->pitches;

        sound_pitches(pitches, node->velocity, trigger_time);
        if (!edge) {
            if (pitches.empty()) {
                push_event(
                    trigger_time + tick,
                    EventKind::Finish,
                    node_id);
            } else {
                push_event(
                    trigger_time + tick,
                    EventKind::NoteOff,
                    node_id,
                    pitches,
                    true);
            }
            return;
        }

        const int ticks = graph.edge_ticks(edge->source_id, edge->target_id);
        if (!pitches.empty()) {
            // Release Gap is measured as eighths of the final sixteenth. A zero
            // gap places Note Off on the next Trigger deadline, where Note Off wins
            // by event priority.
            const double release_deadline = trigger_time +
                                            (ticks - release_gap_eighths / 8.0) * tick;
            push_event(
                release_deadline,
                EventKind::NoteOff,
                node_id,
                pitches);
        }

        push_event(
            trigger_time + ticks * tick,
            EventKind::Trigger,
            edge->target_id);
    }

    void SequencerEngine::trigger_node_external(int node_id, double trigger_pulse, double wire_time) {
        const Node *node = graph.find_node(node_id);
        if (!node) {
            finish_playback();
            return;
        }

        const auto outgoing = graph.outgoing_edges(node_id);
        const auto edge = choose_outgoing_edge(node_id, outgoing);

        if (is_relay(*node)) {
            // Relay traversal also stays at the same external Clock pulse.
            current_node_id.reset();
            if (!edge) {
                finish_playback();
            } else {
                trigger_node_external(edge->target_id, trigger_pulse, wire_time);
            }
            return;
        }

        current_node_id = node_id;
        const std::vector<int> pitches = node->silenced
                                             ? std::vector<int>{}
                                             : node->pitches;

        sound_pitches(pitches, node->velocity, wire_time);
        if (!edge) {
            const double deadline =
                    trigger_pulse + kMidiClockPulsesPerSixteenth;
            if (pitches.empty()) {
                push_clock_event(deadline, EventKind::Finish, node_id);
            } else {
                push_clock_event(
                    deadline,
                    EventKind::NoteOff,
                    node_id,
                    pitches,
                    true);
            }
            return;
        }

        const int ticks = graph.edge_ticks(edge->source_id, edge->target_id);
        const double target =
                trigger_pulse + ticks * kMidiClockPulsesPerSixteenth;
        if (!pitches.empty()) {
            // External Clock uses the same Release Gap, expressed here in Clock
            // pulses; sub-pulse Note Offs are later interpolated into real time.
            const double release = target -
                                   (release_gap_eighths / 8.0) *
                                   kMidiClockPulsesPerSixteenth;
            push_clock_event(release, EventKind::NoteOff, node_id, pitches);
        }
        push_clock_event(target, EventKind::Trigger, edge->target_id);
    }

    std::optional<Edge> SequencerEngine::choose_outgoing_edge(int node_id, const std::vector<Edge> &outgoing) {
        if (outgoing.empty()) {
            return std::nullopt;
        }
        if (outgoing.size() == 1) {
            return outgoing.front();
        }

        const Node *node = graph.find_node(node_id);
        if (node && node->routing_mode == RoutingMode::Counter) {
            // Outgoing edges stay in graph insertion order, which defines the
            // round-robin routing sequence.
            std::size_t &position = counter_positions_[node_id];
            const Edge edge = outgoing[position % outgoing.size()];
            ++position;
            return edge;
        }

        std::uniform_int_distribution<std::size_t> distribution(
            0,
            outgoing.size() - 1);
        return outgoing[distribution(random_)];
    }

    void SequencerEngine::sound_pitches(const std::vector<int> &pitches, int velocity, double deadline) {
        if (pitches.empty()) {
            return;
        }

        midi->notes_on(pitches, velocity, output_channel, deadline);
        active_pitches.insert(pitches.begin(), pitches.end());
    }

    void SequencerEngine::release_event_pitches(const Event &event, double deadline) {
        if (!event.pitches.empty()) {
            midi->notes_off(event.pitches, 0, output_channel, deadline);
            for (int pitch: event.pitches) {
                active_pitches.erase(pitch);
            }
        }

        if (current_node_id == event.node_id) {
            current_node_id.reset();
        }
        if (event.stop_after) {
            finish_playback();
        }
    }

    void SequencerEngine::reset_external_clock_state() {
        clock_events_.clear();
        armed_clock_releases_.clear();
        external_pulse_ = 0;
        external_pulse_times_.clear();
        external_pulse_period_ = midi_clock_interval_seconds(bpm);
    }

    void SequencerEngine::switch_internal_to_external(double timestamp) {
        (void) process(timestamp, 512);

        // Convert remaining absolute-time events into pulse offsets measured from
        // this handoff. Their relative musical phase is preserved.
        std::vector<Event> converted;
        converted.reserve(events_.size());
        for (const Event &event: events_) {
            Event copy = event;
            copy.deadline =
                    std::max(0.0, event.deadline - timestamp) /
                    external_pulse_period_;
            converted.push_back(std::move(copy));
        }

        events_.clear();
        clock_events_ = std::move(converted);
        heap_rebuild(clock_events_);
        armed_clock_releases_.clear();
        external_pulse_ = 0;
        external_clock_active_ = true;
        external_switch_pending_.reset();
        external_alignment_pulses_ = 0;
        state = TransportState::Running;

        midi_clock_enabled = false;
        clear_clock_output_switch();
        stop_clock_output(timestamp);
        (void) process_clock_events_at_pulse(timestamp);
        arm_subpulse_release(timestamp);
    }

    void SequencerEngine::switch_external_to_internal(double timestamp) {
        (void) process_armed_clock_releases(timestamp, 512);

        std::vector<Event> converted;
        converted.reserve(clock_events_.size() + armed_clock_releases_.size());
        for (const Event &event: clock_events_) {
            Event copy = event;
            copy.deadline = timestamp +
                            std::max(
                                0.0,
                                event.deadline - static_cast<double>(external_pulse_)) *
                            external_pulse_period_;
            converted.push_back(std::move(copy));
        }
        for (const ArmedRelease &release: armed_clock_releases_) {
            Event copy = release.event;
            copy.deadline = std::max(timestamp, release.real_deadline);
            converted.push_back(std::move(copy));
        }

        clock_events_.clear();
        armed_clock_releases_.clear();
        events_ = std::move(converted);
        heap_rebuild(events_);

        tempo_epoch_time_ = timestamp;
        tempo_epoch_pulse_ = 0.0;
        external_clock_active_ = false;
        external_switch_pending_.reset();
        external_alignment_pulses_ = 0;
        state = TransportState::Running;

        if (midi_clock_enabled) {
            arm_clock_output_switch(true, timestamp);
        }
    }

    void SequencerEngine::release_active_after_clock_loss(double timestamp) noexcept {
        try {
            if (!active_pitches.empty()) {
                std::vector<int> pitches(
                    active_pitches.begin(),
                    active_pitches.end());
                midi->notes_off(pitches, 0, output_channel, timestamp);
            }
        } catch (...) {
        }

        active_pitches.clear();
        try {
            midi->all_notes_off(output_channel);
        } catch (...) {
        }

        current_node_id.reset();
        for (Event &event: events_) {
            if (event.kind == EventKind::NoteOff) {
                event.pitches.clear();
            }
        }
    }

    void SequencerEngine::record_deadline_lateness(double deadline, double now) {
        const double lateness = std::max(0.0, now - deadline);
        max_event_lateness = std::max(max_event_lateness, lateness);
        if (lateness > kMissedDeadlineSeconds) {
            ++missed_deadlines;
        }
    }

    void SequencerEngine::record_external_pulse(double source_timestamp) {
        if (!external_pulse_times_.empty()) {
            const double previous = external_pulse_times_.back();
            if (source_timestamp < previous || source_timestamp - previous > kExternalResetGap) {
                if (source_timestamp >= previous) {
                    ++input_gaps;
                }
                external_pulse_times_.clear();
            }
        }

        external_pulse_times_.push_back(source_timestamp);
        while (external_pulse_times_.size() >
               kExternalEstimateIntervals + 1) {
            external_pulse_times_.pop_front();
        }

        if (external_pulse_times_.size() < kExternalEstimateMinPulses) {
            return;
        }

        const double elapsed =
                external_pulse_times_.back() - external_pulse_times_.front();
        const std::size_t intervals = external_pulse_times_.size() - 1;
        if (elapsed <= 0.0) {
            return;
        }

        const double period = elapsed / static_cast<double>(intervals);
        const double measured_bpm = 60.0 / (period * kMidiClockPulsesPerQuarter);
        if (measured_bpm >= kMinTempo && measured_bpm <= kMaxTempo) {
            external_pulse_period_ = period;
            bpm = measured_bpm;
        }
    }

    int SequencerEngine::process_armed_clock_releases(double now, int limit) {
        int processed = 0;
        while (playing() &&
               !armed_clock_releases_.empty() &&
               armed_clock_releases_.front().real_deadline <= now &&
               processed < limit) {
            ArmedRelease release = release_pop();
            record_deadline_lateness(release.real_deadline, now);
            process_event(release.event, release.real_deadline, true);
            ++processed;
        }
        return processed;
    }

    int SequencerEngine::flush_clock_releases_at_pulse(double timestamp) {
        int processed = process_armed_clock_releases(timestamp, 512);
        std::vector<ArmedRelease> retained;

        while (!armed_clock_releases_.empty()) {
            ArmedRelease release = release_pop();
            if (release.event.deadline <= static_cast<double>(external_pulse_)) {
                process_event(release.event, timestamp, true);
                ++processed;
            } else {
                retained.push_back(std::move(release));
            }
        }

        for (ArmedRelease &release: retained) {
            release_push(std::move(release));
        }
        return processed;
    }

    int SequencerEngine::process_clock_events_at_pulse(double timestamp) {
        int processed = 0;
        while (playing() &&
               !clock_events_.empty() &&
               clock_events_.front().deadline <=
               static_cast<double>(external_pulse_)) {
            Event event = heap_pop(clock_events_);
            process_event(event, timestamp, true);
            ++processed;
        }
        return processed;
    }

    void SequencerEngine::arm_subpulse_release(double pulse_time) {
        // Node triggers stay aligned to incoming Clock pulses. Only Release-gap
        // Note Offs may be interpolated between pulses using the measured period.
        while (playing() && !clock_events_.empty()) {
            const Event &event = clock_events_.front();
            if (event.kind != EventKind::NoteOff) {
                return;
            }

            const double offset =
                    event.deadline - static_cast<double>(external_pulse_);
            if (!(offset > 0.0 && offset < 1.0)) {
                return;
            }

            Event popped = heap_pop(clock_events_);
            release_push(ArmedRelease{
                pulse_time + offset * external_pulse_period_,
                ++sequence_,
                std::move(popped),
            });
        }
    }

    double SequencerEngine::pulse_position_at(double timestamp) const {
        return tempo_epoch_pulse_ +
               (timestamp - tempo_epoch_time_) /
               midi_clock_interval_seconds(bpm);
    }

    double SequencerEngine::time_at_pulse(std::int64_t pulse_index) const {
        return tempo_epoch_time_ +
               (static_cast<double>(pulse_index) - tempo_epoch_pulse_) *
               midi_clock_interval_seconds(bpm);
    }

    void SequencerEngine::arm_clock_output_switch(bool enabled, double timestamp) {
        const double position = pulse_position_at(timestamp);
        const auto boundary = static_cast<std::int64_t>(
            (std::floor(
                 (position + kDeadlineEpsilon) /
                 kMidiClockPulsesPerSixteenth) +
             1) *
            kMidiClockPulsesPerSixteenth);

        clock_output_switch_pending_ = enabled;
        clock_output_switch_pulse_index_ = boundary;
        clock_output_switch_deadline_ = time_at_pulse(boundary);
    }

    void SequencerEngine::apply_clock_output_switch(double timestamp) {
        const auto enabled = clock_output_switch_pending_;
        const auto boundary = clock_output_switch_pulse_index_;
        clear_clock_output_switch();

        if (enabled == true && midi_clock_enabled && boundary) {
            midi->send_realtime(kMidiStart, timestamp);
            next_clock_pulse_index_ = *boundary;
            next_clock_deadline_ = time_at_pulse(*boundary);
            clock_output_running_ = true;
            emit_clock_pulse(timestamp);
        } else if (enabled == false && !midi_clock_enabled) {
            stop_clock_output(timestamp);
        }
    }

    void SequencerEngine::clear_clock_output_switch() noexcept {
        clock_output_switch_pending_.reset();
        clock_output_switch_pulse_index_.reset();
        clock_output_switch_deadline_.reset();
    }

    void SequencerEngine::start_clock_output(
        double timestamp,
        std::uint8_t transport_status,
        bool align_to_running_transport) {
        midi->send_realtime(transport_status, timestamp);

        const double position = pulse_position_at(timestamp);
        const std::int64_t next = align_to_running_transport
                                      ? std::max<std::int64_t>(
                                          0,
                                          static_cast<std::int64_t>(
                                              std::ceil(position - kDeadlineEpsilon)))
                                      : 0;

        next_clock_pulse_index_ = next;
        next_clock_deadline_ = time_at_pulse(next);
        clock_output_running_ = true;
    }

    void SequencerEngine::emit_clock_pulse(double deadline) {
        if (!clock_output_running_ || !next_clock_pulse_index_) {
            return;
        }

        midi->send_realtime(kMidiTimingClock, deadline);
        ++*next_clock_pulse_index_;
        next_clock_deadline_ = time_at_pulse(*next_clock_pulse_index_);
    }

    void SequencerEngine::stop_clock_output(double deadline) {
        const bool was_running = clock_output_running_;
        clock_output_running_ = false;
        next_clock_pulse_index_.reset();
        next_clock_deadline_.reset();

        if (was_running) {
            midi->send_realtime(kMidiStop, deadline);
        }
    }
}
