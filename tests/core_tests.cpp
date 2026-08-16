#include "spatial_midi/core/graph.hpp"
#include "spatial_midi/core/midi_io.hpp"
#include "spatial_midi/core/midi_note_input_worker.hpp"
#include "spatial_midi/core/sequencer_engine.hpp"
#include "spatial_midi/core/transport_worker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
    using namespace spatial_midi;
    using namespace std::chrono_literals;

    void check(bool condition, std::string_view expression,
               const std::source_location &where = std::source_location::current()) {
        if (!condition) {
            throw std::runtime_error(
                std::string(where.file_name()) + ':' + std::to_string(where.line()) +
                ": CHECK failed: " + std::string(expression));
        }
    }

#define CHECK(expression) check(static_cast<bool>(expression), #expression)

    template<class Exception, class Function>
    void check_throws(Function &&function,
                      const std::source_location &where = std::source_location::current()) {
        try {
            std::forward<Function>(function)();
        } catch (const Exception &) {
            return;
        }
        throw std::runtime_error(
            std::string(where.file_name()) + ':' + std::to_string(where.line()) +
            ": expected exception was not thrown");
    }

    TimePoint at(double seconds) {
        return TimePoint{to_nanoseconds(Seconds{seconds})};
    }

    TimePoint after(TimePoint origin, Seconds duration) {
        return origin + to_nanoseconds(duration);
    }

    bool same_time(TimePoint first, TimePoint second, Nanoseconds tolerance = 2ns) {
        return first > second ? first - second <= tolerance : second - first <= tolerance;
    }

    struct MidiEvent {
        enum class Kind { NotesOn, NotesOff, AllNotesOff, Realtime, ClearScheduled };

        Kind kind{};
        std::vector<int> pitches;
        int velocity = 0;
        int channel = 0;
        std::uint8_t status = 0;
        TimePoint deadline{};
    };

    class RecordingMidi final : public MidiOutput {
    public:
        [[nodiscard]] std::string description() const override { return "recording MIDI"; }

        void notes_on(std::span<const int> pitches, int velocity, int channel, TimePoint deadline) override {
            events.push_back({MidiEvent::Kind::NotesOn, {pitches.begin(), pitches.end()}, velocity, channel, 0, deadline});
        }

        void notes_off(std::span<const int> pitches, int velocity, int channel, TimePoint deadline) override {
            events.push_back({MidiEvent::Kind::NotesOff, {pitches.begin(), pitches.end()}, velocity, channel, 0, deadline});
        }

        void all_notes_off(int channel) override {
            events.push_back({MidiEvent::Kind::AllNotesOff, {}, 0, channel, 0, {}});
        }

        void send_realtime(std::uint8_t status, TimePoint deadline) override {
            events.push_back({MidiEvent::Kind::Realtime, {}, 0, 0, status, deadline});
        }

        void clear_scheduled() override {
            events.push_back({MidiEvent::Kind::ClearScheduled, {}, 0, 0, 0, {}});
        }

        std::vector<MidiEvent> events;
    };

    std::vector<const MidiEvent *> events_of(const RecordingMidi &midi, MidiEvent::Kind kind) {
        std::vector<const MidiEvent *> result;
        for (const auto &event: midi.events) {
            if (event.kind == kind) {
                result.push_back(&event);
            }
        }
        return result;
    }

    std::vector<int> triggered_first_pitches(const RecordingMidi &midi) {
        std::vector<int> result;
        for (const auto *event: events_of(midi, MidiEvent::Kind::NotesOn)) {
            if (!event->pitches.empty()) {
                result.push_back(event->pitches.front());
            }
        }
        return result;
    }

    std::vector<std::uint8_t> realtime_statuses(const RecordingMidi &midi) {
        std::vector<std::uint8_t> result;
        for (const auto *event: events_of(midi, MidiEvent::Kind::Realtime)) {
            result.push_back(event->status);
        }
        return result;
    }

    std::size_t find_event(const RecordingMidi &midi, MidiEvent::Kind kind, TimePoint deadline,
                           std::uint8_t status = 0) {
        for (std::size_t i = 0; i < midi.events.size(); ++i) {
            const auto &event = midi.events[i];
            if (event.kind != kind || !same_time(event.deadline, deadline)) {
                continue;
            }
            if (kind == MidiEvent::Kind::Realtime && event.status != status) {
                continue;
            }
            return i;
        }
        return midi.events.size();
    }


    std::string replace_once(std::string text, std::string_view from, std::string_view to) {
        const std::size_t position = text.find(from);
        CHECK(position != std::string::npos);
        text.replace(position, from.size(), to);
        return text;
    }

    template<class Predicate>
    bool wait_until(std::chrono::milliseconds timeout, Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return predicate();
    }

    class ScriptedNoteInput final : public MidiNoteInput {
    public:
        explicit ScriptedNoteInput(std::vector<MidiNoteMessage> messages)
            : messages_(std::move(messages)) {}

        [[nodiscard]] std::string description() const override { return "scripted note input"; }

        std::optional<MidiNoteMessage> read_note(std::chrono::milliseconds timeout) override {
            std::lock_guard lock(mutex_);
            if (next_ < messages_.size()) {
                return messages_[next_++];
            }
            std::this_thread::sleep_for(timeout);
            return std::nullopt;
        }

    private:
        std::mutex mutex_;
        std::vector<MidiNoteMessage> messages_;
        std::size_t next_ = 0;
    };

    class BrokenNoteInput final : public MidiNoteInput {
    public:
        [[nodiscard]] std::string description() const override { return "broken note input"; }

        std::optional<MidiNoteMessage> read_note(std::chrono::milliseconds) override {
            throw std::runtime_error("input disconnected");
        }
    };

    void project_round_trip_preserves_the_composition() {
        Graph graph;
        const int chord = graph.add_node(3, -2, 60, 91, true, {60, 64, 67}, RoutingMode::Counter).id;
        const int relay = graph.add_relay_node(7, -2).id;
        const int lead = graph.add_node(10, 1, 74, 105).id;
        graph.connect(chord, relay);
        graph.connect(relay, lead);
        graph.set_start(chord);

        const ProjectSettings saved{137.5, 3};
        ProjectSettings loaded_settings;
        const Graph loaded = Graph::from_json(graph.to_json(saved), &loaded_settings);

        CHECK(loaded_settings.bpm == saved.bpm);
        CHECK(loaded_settings.release_gap_eighths == saved.release_gap_eighths);
        CHECK(loaded.start_node_id() == chord);
        CHECK(loaded.nodes().size() == 3);
        CHECK(loaded.edges() == graph.edges());

        const Node *loaded_chord = loaded.find_node(chord);
        const Node *loaded_relay = loaded.find_node(relay);
        const Node *loaded_lead = loaded.find_node(lead);
        CHECK(loaded_chord != nullptr);
        CHECK(loaded_relay != nullptr);
        CHECK(loaded_lead != nullptr);
        CHECK(loaded_chord->x == 3 && loaded_chord->y == -2);
        CHECK(loaded_chord->pitches == std::vector<int>({60, 64, 67}));
        CHECK(loaded_chord->velocity == 91);
        CHECK(loaded_chord->silenced);
        CHECK(loaded_chord->routing_mode == RoutingMode::Counter);
        CHECK(is_relay(*loaded_relay));
        CHECK(loaded_relay->pitches.empty());
        CHECK(loaded_lead->pitches == std::vector<int>({74}));
        CHECK(loaded.edge_ticks(chord, relay) == 4);
        CHECK(loaded.edge_ticks(relay, lead) == 0);
    }

    void graph_edits_obey_grid_and_midi_rules() {
        Graph graph;
        const int note = graph.add_node(0, 0, 120, 120).id;
        const int other = graph.add_node(2, 3, 61).id;
        const int relay = graph.add_relay_node(5, 3).id;

        check_throws<GraphError>([&] { (void) graph.add_node(0, 0, 62); });
        check_throws<GraphError>([&] { graph.move_node(other, 0, 0); });

        CHECK(graph.transpose(note, 20) == 127);
        CHECK(graph.adjust_velocity(note, 20) == 127);
        const std::size_t pitches_before = graph.find_node(note)->pitches.size();
        const int added_pitch = graph.append_pitch(note);
        CHECK(graph.find_node(note)->pitches.size() == pitches_before + 1);
        CHECK(added_pitch >= kMinMidiPitch && added_pitch <= kMaxMidiPitch);
        while (graph.find_node(note)->pitches.size() < static_cast<std::size_t>(kMaxPitchSlots)) {
            (void) graph.append_pitch(note);
        }
        check_throws<GraphError>([&] { (void) graph.append_pitch(note); });
        while (graph.find_node(note)->pitches.size() > static_cast<std::size_t>(kMinPitchSlots)) {
            (void) graph.remove_last_pitch(note);
        }
        check_throws<GraphError>([&] { (void) graph.remove_last_pitch(note); });
        check_throws<GraphError>([&] { (void) graph.set_pitch(relay, 72); });

        graph.connect(note, other);
        graph.connect(other, relay);
        CHECK(graph.edge_ticks(note, other) == 5);
        CHECK(graph.edge_ticks(other, relay) == 3);
        CHECK((graph.connect(relay, note) == Edge{relay, note}));
        CHECK(graph.edge_ticks(relay, note) == 0);
        check_throws<GraphError>([&] { (void) graph.connect(note, note); });
    }

    void internal_playback_follows_grid_time_and_release_gap() {
        Graph graph;
        const int chord = graph.add_node(0, 0, 60, 93, false, {60, 64, 67}).id;
        const int next = graph.add_node(4, 0, 72, 81).id;
        graph.connect(chord, next);
        graph.set_start(chord);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 2, 1, 1);
        const TimePoint start = at(10.0);
        const Seconds tick = sixteenth_duration(120.0);
        const TimePoint next_trigger = after(start, 4.0 * tick);
        const TimePoint chord_release = after(start, (4.0 - 1.0 / 8.0) * tick);

        engine.start(start);
        const auto note_ons_after_start = events_of(midi, MidiEvent::Kind::NotesOn);
        CHECK(note_ons_after_start.size() == 1);
        CHECK(note_ons_after_start[0]->pitches == std::vector<int>({60, 64, 67}));
        CHECK(note_ons_after_start[0]->velocity == 93);
        CHECK(note_ons_after_start[0]->channel == 2);
        CHECK(same_time(note_ons_after_start[0]->deadline, start));

        CHECK(engine.process(chord_release - 1ns) == 0);
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).empty());
        engine.process(chord_release);
        const auto note_offs = events_of(midi, MidiEvent::Kind::NotesOff);
        CHECK(note_offs.size() == 1);
        CHECK(note_offs[0]->pitches == std::vector<int>({60, 64, 67}));
        CHECK(same_time(note_offs[0]->deadline, chord_release));

        engine.process(next_trigger);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 72}));
        CHECK(same_time(events_of(midi, MidiEvent::Kind::NotesOn).back()->deadline, next_trigger));

        const TimePoint terminal_release = after(next_trigger, tick);
        engine.process(terminal_release);
        CHECK(!engine.playing());
        CHECK(same_time(events_of(midi, MidiEvent::Kind::NotesOff).back()->deadline, terminal_release));
    }

    void live_release_gap_changes_affect_subsequently_triggered_notes() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(4, 0, 62).id;
        const int third = graph.add_node(8, 0, 64).id;
        graph.connect(first, second);
        graph.connect(second, third);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 1, 2);
        const TimePoint start = at(0.0);
        const Seconds tick = sixteenth_duration(120.0);
        engine.start(start);

        CHECK(engine.set_release_gap_eighths(4) == 4);

        const TimePoint release_if_retimed = after(start, 3.5 * tick);
        engine.process(release_if_retimed);
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).empty());

        const TimePoint first_release = after(start, (4.0 - 1.0 / 8.0) * tick);
        engine.process(first_release);
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).size() == 1);
        CHECK(same_time(events_of(midi, MidiEvent::Kind::NotesOff).back()->deadline, first_release));

        const TimePoint second_trigger = after(start, 4.0 * tick);
        engine.process(second_trigger);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 62}));

        const TimePoint second_release = after(second_trigger, 3.5 * tick);
        engine.process(second_release);
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).size() == 2);
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).back()->pitches == std::vector<int>({62}));
        CHECK(same_time(events_of(midi, MidiEvent::Kind::NotesOff).back()->deadline, second_release));
    }

    void rests_and_relays_route_without_making_notes() {
        Graph graph;
        const int source = graph.add_node(0, 0, 60).id;
        const int rest = graph.add_node(2, 0, 62, 100, true).id;
        const int relay = graph.add_relay_node(4, 0).id;
        const int target = graph.add_node(30, 20, 67).id;
        graph.connect(source, rest);
        graph.connect(rest, relay);
        graph.connect(relay, target);
        graph.set_start(source);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 2);
        const TimePoint start = at(1.0);
        const Seconds tick = sixteenth_duration(120.0);
        engine.start(start);

        engine.process(after(start, 2.0 * tick));
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60}));

        const TimePoint relay_arrival = after(start, 4.0 * tick);
        engine.process(relay_arrival);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 67}));
        CHECK(same_time(events_of(midi, MidiEvent::Kind::NotesOn).back()->deadline, relay_arrival));
        CHECK(engine.snapshot().current_node_id == target);
    }

    void round_robin_branches_repeat_in_edge_order() {
        Graph graph;
        const int branch = graph.add_node(0, 0, 60, 100, false, {}, RoutingMode::Counter).id;
        const int east = graph.add_node(1, 0, 62).id;
        const int west = graph.add_node(-1, 0, 64).id;
        graph.connect(branch, east);
        graph.connect(branch, west);
        graph.connect(east, branch);
        graph.connect(west, branch);
        graph.set_start(branch);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 3);
        const TimePoint start = at(0.0);
        const Seconds tick = sixteenth_duration(120.0);
        engine.start(start);
        for (int step = 1; step <= 4; ++step) {
            engine.process(after(start, static_cast<double>(step) * tick));
        }

        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 62, 60, 64, 60}));
    }

    void zero_release_gap_still_releases_before_the_next_note() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(1, 0, 60).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 4);
        const TimePoint start = at(2.0);
        const TimePoint boundary = after(start, sixteenth_duration(120.0));
        engine.start(start);
        engine.process(boundary);

        const std::size_t off_index = find_event(midi, MidiEvent::Kind::NotesOff, boundary);
        const std::size_t on_index = find_event(midi, MidiEvent::Kind::NotesOn, boundary);
        CHECK(off_index < midi.events.size());
        CHECK(on_index < midi.events.size());
        CHECK(off_index < on_index);
    }

    void midi_clock_output_uses_standard_transport_and_pulse_rate() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(1, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 5);
        CHECK(engine.toggle_midi_clock(true, at(5.0)));
        const TimePoint start = at(5.0);
        const TimePoint boundary = after(start, sixteenth_duration(120.0));
        engine.start(start);
        engine.process(boundary);

        const auto statuses = realtime_statuses(midi);
        CHECK(!statuses.empty());
        CHECK(statuses.front() == kMidiStart);
        CHECK(static_cast<int>(std::ranges::count(statuses, kMidiTimingClock)) == 7);

        const std::size_t off_index = find_event(midi, MidiEvent::Kind::NotesOff, boundary);
        const std::size_t on_index = find_event(midi, MidiEvent::Kind::NotesOn, boundary);
        const std::size_t clock_index = find_event(midi, MidiEvent::Kind::Realtime, boundary, kMidiTimingClock);
        CHECK(off_index < on_index);
        CHECK(on_index < clock_index);
    }

    void external_clock_advances_one_step_every_six_pulses() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(1, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 6);
        CHECK(engine.set_external_clock(true, at(0.0)));
        CHECK(engine.snapshot().state == TransportState::WaitingForClock);
        CHECK(engine.process_external_message(kMidiStart, at(1.0)) == 1);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60}));

        for (int pulse = 1; pulse <= 5; ++pulse) {
            engine.process_external_message(kMidiTimingClock, at(1.0 + pulse / 48.0));
        }
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60}));

        engine.process_external_message(kMidiTimingClock, at(1.0 + 6.0 / 48.0));
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 62}));
        CHECK(engine.snapshot().external_clock_active);

        CHECK(engine.process_external_message(kMidiStop, at(2.0)) == 1);
        CHECK(!engine.playing());
        CHECK(!events_of(midi, MidiEvent::Kind::AllNotesOff).empty());
    }

    void pause_and_resume_release_then_restore_the_current_note() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60, 77).id;
        const int second = graph.add_node(4, 0, 65).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 1, 0, 7);
        const Seconds tick = sixteenth_duration(120.0);
        const TimePoint start = at(3.0);
        const TimePoint paused_at = after(start, tick);
        const TimePoint resumed_at = at(10.0);
        engine.start(start);
        CHECK(engine.pause(paused_at));

        CHECK(engine.snapshot().state == TransportState::Paused);
        CHECK(!events_of(midi, MidiEvent::Kind::ClearScheduled).empty());
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).back()->pitches == std::vector<int>({60}));
        CHECK(!events_of(midi, MidiEvent::Kind::AllNotesOff).empty());

        CHECK(engine.resume(resumed_at));
        const auto note_ons = events_of(midi, MidiEvent::Kind::NotesOn);
        CHECK(note_ons.size() == 2);
        CHECK(note_ons.back()->pitches == std::vector<int>({60}));
        CHECK(note_ons.back()->velocity == 77);
        CHECK(same_time(note_ons.back()->deadline, resumed_at));

        const TimePoint shifted_boundary = after(resumed_at, 3.0 * tick);
        engine.process(shifted_boundary - 1ns);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 60}));
        engine.process(shifted_boundary);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 60, 65}));
    }


    void random_routing_can_reach_each_outgoing_branch() {
        Graph graph;
        const int branch = graph.add_node(0, 0, 60, 100, false, {}, RoutingMode::Random).id;
        const int east = graph.add_node(1, 0, 62).id;
        const int west = graph.add_node(-1, 0, 64).id;
        graph.connect(branch, east);
        graph.connect(branch, west);
        graph.connect(east, branch);
        graph.connect(west, branch);
        graph.set_start(branch);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 42);
        const TimePoint start = at(0.0);
        const Seconds tick = sixteenth_duration(120.0);
        engine.start(start);
        for (int step = 1; step <= 80; ++step) {
            engine.process(after(start, static_cast<double>(step) * tick));
        }

        const auto pitches = triggered_first_pitches(midi);
        CHECK(std::ranges::all_of(pitches, [](int pitch) { return pitch == 60 || pitch == 62 || pitch == 64; }));
        CHECK(std::ranges::find(pitches, 62) != pitches.end());
        CHECK(std::ranges::find(pitches, 64) != pitches.end());
    }

    void stop_releases_notes_clears_transport_and_starts_routing_fresh() {
        Graph graph;
        const int branch = graph.add_node(0, 0, 60, 100, false, {}, RoutingMode::Counter).id;
        const int east = graph.add_node(1, 0, 62).id;
        const int west = graph.add_node(-1, 0, 64).id;
        graph.connect(branch, east);
        graph.connect(branch, west);
        graph.connect(east, branch);
        graph.connect(west, branch);
        graph.set_start(branch);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 43);
        const Seconds tick = sixteenth_duration(120.0);
        const TimePoint first_start = at(1.0);
        engine.start(first_start);
        engine.process(after(first_start, tick));
        engine.process(after(first_start, 2.0 * tick));
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 62, 60}));

        engine.stop();
        CHECK(engine.snapshot().state == TransportState::Stopped);
        CHECK(!engine.snapshot().current_node_id.has_value());
        CHECK(!events_of(midi, MidiEvent::Kind::ClearScheduled).empty());
        CHECK(!events_of(midi, MidiEvent::Kind::AllNotesOff).empty());
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).back()->pitches == std::vector<int>({60}));

        const std::size_t notes_before_restart = events_of(midi, MidiEvent::Kind::NotesOn).size();
        const TimePoint second_start = at(10.0);
        engine.start(second_start);
        engine.process(after(second_start, tick));
        const auto notes_after_restart = events_of(midi, MidiEvent::Kind::NotesOn);
        CHECK(notes_after_restart.size() == notes_before_restart + 2);
        CHECK(notes_after_restart[notes_before_restart]->pitches == std::vector<int>({60}));
        CHECK(notes_after_restart[notes_before_restart + 1]->pitches == std::vector<int>({62}));
    }

    void live_tempo_changes_preserve_musical_phase() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(4, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 44);
        engine.start(at(0.0));

        engine.set_tempo(60.0, at(0.25));
        CHECK(engine.snapshot().bpm == 60.0);
        engine.process(at(0.75) - 1ns);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60}));
        engine.process(at(0.75));
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 62}));
        CHECK(same_time(events_of(midi, MidiEvent::Kind::NotesOn).back()->deadline, at(0.75)));
    }

    void live_midi_clock_output_toggles_on_sixteenth_boundaries() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(8, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 45);
        engine.start(at(0.0));

        CHECK(engine.toggle_midi_clock(true, at(0.05)));
        CHECK(engine.snapshot().midi_clock_output_switch_pending == std::optional<bool>{true});
        engine.process(at(0.125) - 1ns);
        CHECK(realtime_statuses(midi).empty());
        engine.process(at(0.125));
        CHECK(find_event(midi, MidiEvent::Kind::Realtime, at(0.125), kMidiStart) < midi.events.size());
        CHECK(find_event(midi, MidiEvent::Kind::Realtime, at(0.125), kMidiTimingClock) < midi.events.size());

        CHECK(!engine.toggle_midi_clock(false, at(0.16)));
        engine.process(at(0.25) - 1ns);
        CHECK(std::ranges::count(realtime_statuses(midi), kMidiStop) == 0);
        engine.process(at(0.25));
        CHECK(find_event(midi, MidiEvent::Kind::Realtime, at(0.25), kMidiStop) < midi.events.size());
    }

    void pause_and_resume_use_midi_stop_and_continue_when_clock_output_is_active() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(8, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 46);
        CHECK(engine.toggle_midi_clock(true, at(2.0)));
        engine.start(at(2.0));
        CHECK(engine.pause(at(2.05)));
        CHECK(realtime_statuses(midi).back() == kMidiStop);

        CHECK(engine.resume(at(3.0)));
        const auto statuses = realtime_statuses(midi);
        CHECK(statuses.back() == kMidiContinue);
        CHECK(std::ranges::count(statuses, kMidiStart) == 1);
        CHECK(std::ranges::count(statuses, kMidiStop) == 1);
        CHECK(std::ranges::count(statuses, kMidiContinue) == 1);
    }

    void playback_hands_off_between_internal_and_external_clock_without_restarting() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(8, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 47);
        engine.start(at(0.0));
        CHECK(engine.set_external_clock(true, at(0.05)));
        CHECK(engine.snapshot().external_clock_switch_pending == std::optional<ExternalClockSwitch>{ExternalClockSwitch::ToExternal});

        for (int pulse = 1; pulse <= 5; ++pulse) {
            engine.process_external_message(kMidiTimingClock, at(0.04 + pulse * 0.02));
        }
        CHECK(!engine.snapshot().external_clock_active);
        engine.process_external_message(kMidiTimingClock, at(0.16));
        CHECK(engine.snapshot().external_clock_active);
        CHECK(!engine.snapshot().external_clock_switch_pending.has_value());
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60}));

        CHECK(!engine.set_external_clock(false, at(0.17)));
        CHECK(engine.snapshot().external_clock_switch_pending == std::optional<ExternalClockSwitch>{ExternalClockSwitch::ToInternal});
        for (int pulse = 1; pulse <= 5; ++pulse) {
            engine.process_external_message(kMidiTimingClock, at(0.16 + pulse * 0.02));
        }
        CHECK(engine.snapshot().external_clock_active);
        engine.process_external_message(kMidiTimingClock, at(0.28));
        CHECK(!engine.snapshot().external_clock_active);
        CHECK(!engine.snapshot().external_clock_switch_pending.has_value());
        CHECK(engine.snapshot().state == TransportState::Running);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60}));
    }

    void clock_loss_releases_active_notes_and_continues_on_internal_time() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(2, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 48);
        CHECK(engine.set_external_clock(true, at(0.0)));
        CHECK(engine.process_external_message(kMidiStart, at(1.0)) == 1);
        for (int pulse = 1; pulse <= 6; ++pulse) {
            engine.process_external_message(kMidiTimingClock, at(1.0 + pulse * 0.02));
        }

        const TimePoint loss_time = at(1.13);
        CHECK(engine.force_internal_clock(loss_time, true));
        CHECK(engine.snapshot().state == TransportState::ClockLost);
        CHECK(!engine.snapshot().external_clock_active);
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).back()->pitches == std::vector<int>({60}));
        CHECK(same_time(events_of(midi, MidiEvent::Kind::NotesOff).back()->deadline, loss_time));
        CHECK(!events_of(midi, MidiEvent::Kind::AllNotesOff).empty());

        const double fallback_bpm = engine.snapshot().bpm;
        const TimePoint fallback_target = loss_time + to_nanoseconds(6.0 * midi_clock_interval(fallback_bpm));
        engine.process(fallback_target - 1ns);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60}));
        engine.process(fallback_target);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 62}));
    }

    void startup_project_loads_existing_file_or_uses_default() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / ("spatial-midi-startup-" + std::to_string(unique) + ".json");

        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } cleanup{path};

        Graph saved;
        const int first = saved.add_node(1, 2, 74, 88).id;
        const int second = saved.add_node(5, 2, 79, 91).id;
        saved.connect(first, second);
        saved.set_start(first);
        saved.save_json(path, ProjectSettings{96.0, 3});

        ProjectSettings loaded_settings;
        const Graph loaded = load_project_or_default(path, 111, &loaded_settings);
        CHECK(loaded.nodes().size() == 2);
        CHECK(loaded.start_node_id() == first);
        CHECK(loaded.find_node(first) != nullptr);
        CHECK(loaded.find_node(first)->pitches == std::vector<int>({74}));
        CHECK(loaded_settings.bpm == 96.0);
        CHECK(loaded_settings.release_gap_eighths == 3);

        std::filesystem::remove(path);
        loaded_settings = ProjectSettings{240.0, 4};
        const Graph fallback = load_project_or_default(path, 111, &loaded_settings);
        CHECK(fallback.nodes().size() == 4);
        CHECK(fallback.start_node_id().has_value());
        for (const Node &node: fallback.nodes()) {
            CHECK(node.velocity == 111);
        }
        CHECK(loaded_settings.bpm == kDefaultTempo);
        CHECK(loaded_settings.release_gap_eighths == kDefaultReleaseGapEighths);
    }

    void invalid_projects_are_rejected_during_load() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(1, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);
        const std::string valid = graph.to_json({120.0, 1});

        const std::vector<std::string> invalid_projects{
            "{",
            replace_once(valid, "\"version\": 1", "\"version\": 999"),
            replace_once(valid, "\"bpm\": 120", "\"bpm\": 301"),
            replace_once(valid, "\"release_gap_eighths\": 1", "\"release_gap_eighths\": 5"),
            replace_once(valid, "\"target_id\": 2", "\"target_id\": 999"),
        };

        for (const std::string &project: invalid_projects) {
            check_throws<GraphError>([&] { (void) Graph::from_json(project); });
        }
    }

    void deleting_the_upcoming_node_lets_the_current_note_finish_then_stops() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(4, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 49);
        const TimePoint start = at(4.0);
        const TimePoint boundary = after(start, 4.0 * sixteenth_duration(120.0));
        engine.start(start);

        graph.delete_node(second);
        (void) engine.handle_node_deleted(second);
        CHECK(engine.playing());
        engine.process(boundary - 1ns);
        CHECK(engine.playing());
        engine.process(boundary);

        CHECK(!engine.playing());
        CHECK(engine.snapshot().state == TransportState::Stopped);
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60}));
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).back()->pitches == std::vector<int>({60}));
    }

    void external_clock_release_gap_can_release_between_clock_pulses() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(2, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);

        RecordingMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 2, 50);
        CHECK(engine.set_external_clock(true, at(0.0)));
        CHECK(engine.process_external_message(kMidiStart, at(1.0)) == 1);
        for (int pulse = 1; pulse <= 10; ++pulse) {
            engine.process_external_message(kMidiTimingClock, at(1.0 + pulse * 0.02));
        }

        const TimePoint release = at(1.21);
        engine.process(release - 1ns);
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).empty());
        engine.process(release);
        CHECK(events_of(midi, MidiEvent::Kind::NotesOff).size() == 1);
        CHECK(same_time(events_of(midi, MidiEvent::Kind::NotesOff).front()->deadline, release));

        engine.process_external_message(kMidiTimingClock, at(1.22));
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60}));
        engine.process_external_message(kMidiTimingClock, at(1.24));
        CHECK(triggered_first_pitches(midi) == std::vector<int>({60, 62}));
    }

    void note_input_forwards_only_note_ons_on_the_selected_channel() {
        auto input = std::make_shared<ScriptedNoteInput>(std::vector<MidiNoteMessage>{
            {60, 100, 0},
            {61, 0, 1},
            {62, 90, 1},
            {63, 80, 2},
        });
        std::mutex received_mutex;
        std::vector<MidiNoteMessage> received;

        MidiNoteInputWorker worker(input, 1, [&](const MidiNoteMessage &message) {
            std::lock_guard lock(received_mutex);
            received.push_back(message);
        });

        CHECK(wait_until(250ms, [&] {
            std::lock_guard lock(received_mutex);
            return received.size() == 1;
        }));
        worker.close();

        std::lock_guard lock(received_mutex);
        CHECK(received.size() == 1);
        CHECK(received.front().pitch == 62);
        CHECK(received.front().velocity == 90);
        CHECK(received.front().channel == 1);
    }

    void note_input_reports_device_failure_without_crashing_the_caller() {
        auto input = std::make_shared<BrokenNoteInput>();
        MidiNoteInputWorker worker(input, 0, [](const MidiNoteMessage &) {});

        CHECK(wait_until(250ms, [&] { return worker.pop_failure().has_value(); }));
        worker.close();
    }

    void transport_worker_applies_note_entry_only_while_stopped() {
        Graph graph;
        const int node = graph.add_node(0, 0, 60, 70, true, {60, 67}).id;
        const int next = graph.add_node(8, 0, 65).id;
        graph.connect(node, next);
        graph.set_start(node);

        auto output = std::make_shared<NullMidiOutput>();
        TransportWorker worker(std::move(graph), output, 120.0, 0);

        CHECK(worker.set_node_from_midi(node, 71, 96));
        const Graph stopped_graph = worker.graph_snapshot();
        const Node *edited = stopped_graph.find_node(node);
        CHECK(edited != nullptr);
        CHECK(edited->pitches == std::vector<int>({71, 67}));
        CHECK(edited->velocity == 96);
        CHECK(edited->silenced);

        worker.start();
        CHECK(!worker.set_node_from_midi(node, 73, 110));
        worker.stop();
        const Graph final_graph = worker.graph_snapshot();
        const Node *unchanged = final_graph.find_node(node);
        CHECK(unchanged != nullptr);
        CHECK(unchanged->pitches.front() == 71);
        CHECK(unchanged->velocity == 96);
        worker.close();
    }

    struct TestCase {
        const char *name;
        void (*run)();
    };
}

int main() {
    const std::vector<TestCase> tests{
        {"project round trip preserves the composition", project_round_trip_preserves_the_composition},
        {"graph edits obey grid and MIDI rules", graph_edits_obey_grid_and_midi_rules},
        {"internal playback follows grid time and Release Gap", internal_playback_follows_grid_time_and_release_gap},
        {"live Release Gap changes affect later notes", live_release_gap_changes_affect_subsequently_triggered_notes},
        {"rests and relays route without making notes", rests_and_relays_route_without_making_notes},
        {"round-robin branches repeat in edge order", round_robin_branches_repeat_in_edge_order},
        {"random routing can reach each outgoing branch", random_routing_can_reach_each_outgoing_branch},
        {"Stop cleans transport and starts routing fresh", stop_releases_notes_clears_transport_and_starts_routing_fresh},
        {"live tempo changes preserve musical phase", live_tempo_changes_preserve_musical_phase},
        {"zero Release Gap releases before the next note", zero_release_gap_still_releases_before_the_next_note},
        {"MIDI Clock output uses transport and 24 PPQN", midi_clock_output_uses_standard_transport_and_pulse_rate},
        {"live MIDI Clock output toggles on sixteenth boundaries", live_midi_clock_output_toggles_on_sixteenth_boundaries},
        {"external Clock advances one step every six pulses", external_clock_advances_one_step_every_six_pulses},
        {"playback hands off between internal and external Clock", playback_hands_off_between_internal_and_external_clock_without_restarting},
        {"Clock loss releases notes and falls back to internal time", clock_loss_releases_active_notes_and_continues_on_internal_time},
        {"external Clock Release Gap can fall between pulses", external_clock_release_gap_can_release_between_clock_pulses},
        {"pause and resume release then restore the current note", pause_and_resume_release_then_restore_the_current_note},
        {"Clock output pause and resume use Stop and Continue", pause_and_resume_use_midi_stop_and_continue_when_clock_output_is_active},
        {"startup loads the project file when present", startup_project_loads_existing_file_or_uses_default},
        {"invalid projects are rejected during load", invalid_projects_are_rejected_during_load},
        {"deleting an upcoming node finishes the current note then stops", deleting_the_upcoming_node_lets_the_current_note_finish_then_stops},
        {"note input filters channel and zero velocity", note_input_forwards_only_note_ons_on_the_selected_channel},
        {"note input reports device failure", note_input_reports_device_failure_without_crashing_the_caller},
        {"transport worker gates note entry by transport state", transport_worker_applies_note_entry_only_while_stopped},
    };

    int failures = 0;
    for (const TestCase &test: tests) {
        try {
            test.run();
            std::cout << "PASS  " << test.name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "FAIL  " << test.name << "\n      " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " of " << tests.size() << " tests failed.\n";
        return 1;
    }

    std::cout << "All " << tests.size() << " behavior tests passed.\n";
    return 0;
}
