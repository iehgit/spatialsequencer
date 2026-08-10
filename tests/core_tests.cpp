#include "spatial_midi/core/graph.hpp"
#include "spatial_midi/core/midi_io.hpp"
#include "spatial_midi/core/midi_note_input_worker.hpp"
#include "spatial_midi/core/sequencer_engine.hpp"
#include "spatial_midi/core/transport_worker.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <filesystem>
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

    constexpr double kEpsilon = 1e-9;

    // CHECK remains active in Release builds, where NDEBUG disables assert().
    void check_test_condition(bool condition, std::string_view expression,
                              const std::source_location &location = std::source_location::current()) {
        if (condition) {
            return;
        }

        throw std::runtime_error(
            std::string(location.file_name()) + ':' + std::to_string(location.line()) + ": CHECK failed: " +
            std::string(expression));
    }

#define CHECK(expression) \
    check_test_condition(static_cast<bool>(expression), #expression)

    bool near(double lhs, double rhs, double tolerance = 1e-8) {
        return std::abs(lhs - rhs) <= tolerance;
    }

    struct RecordedMidi {
        enum class Kind { On, Off, AllOff, Realtime, Clear };

        Kind kind{};
        std::vector<int> pitches;
        int velocity = 0;
        int channel = 0;
        std::uint8_t status = 0;
        double deadline = 0.0;
    };

    class FakeNoteInput final : public MidiNoteInput {
    public:
        explicit FakeNoteInput(std::vector<MidiNoteMessage> messages)
            : messages_(std::move(messages)) {
        }

        [[nodiscard]] std::string description() const override {
            return "Fake note input";
        }

        std::optional<MidiNoteMessage> read_note(std::chrono::milliseconds timeout) override {
            if (next_ < messages_.size()) {
                return messages_[next_++];
            }
            std::this_thread::sleep_for(timeout);
            return std::nullopt;
        }

    private:
        std::vector<MidiNoteMessage> messages_;
        std::size_t next_ = 0;
    };

    class FailingNoteInput final : public MidiNoteInput {
    public:
        [[nodiscard]] std::string description() const override {
            return "Failing note input";
        }

        std::optional<MidiNoteMessage> read_note(std::chrono::milliseconds) override {
            throw std::runtime_error("simulated note input failure");
        }
    };

    class FakeMidi final : public MidiOutput {
    public:
        [[nodiscard]] std::string description() const override {
            return "Fake MIDI";
        }

        void notes_on(std::span<const int> pitches, int velocity, int channel, double deadline) override {
            events.push_back({
                RecordedMidi::Kind::On, {pitches.begin(), pitches.end()}, velocity, channel, 0, deadline,
            });
        }

        void notes_off(std::span<const int> pitches, int velocity, int channel, double deadline) override {
            events.push_back({
                RecordedMidi::Kind::Off, {pitches.begin(), pitches.end()}, velocity, channel, 0, deadline,
            });
        }

        void all_notes_off(int channel) override {
            events.push_back({RecordedMidi::Kind::AllOff, {}, 0, channel, 0, 0.0});
        }

        void send_realtime(std::uint8_t status, double deadline) override {
            events.push_back({RecordedMidi::Kind::Realtime, {}, 0, 0, status, deadline});
        }

        void clear_scheduled() override {
            events.push_back({RecordedMidi::Kind::Clear, {}, 0, 0, 0, 0.0});
        }

        std::vector<RecordedMidi> events;
    };

    std::vector<int> note_on_pitches(const FakeMidi &midi) {
        std::vector<int> result;
        for (const auto &event: midi.events) {
            if (event.kind == RecordedMidi::Kind::On) {
                result.insert(result.end(), event.pitches.begin(), event.pitches.end());
            }
        }
        return result;
    }

    std::vector<RecordedMidi> events_at(const FakeMidi &midi, double deadline, double tolerance = 1e-8);

    const RecordedMidi *first_event(const FakeMidi &midi, RecordedMidi::Kind kind, int pitch = -1) {
        const auto it = std::ranges::find_if(midi.events, [kind, pitch](const RecordedMidi &event) {
            return event.kind == kind &&
                   (pitch < 0 || std::ranges::find(event.pitches, pitch) != event.pitches.end());
        });
        return it == midi.events.end() ? nullptr : &*it;
    }

    void expect_graph_error(const auto &operation) {
        bool threw = false;
        try {
            operation();
        } catch (const GraphError &) {
            threw = true;
        }
        CHECK(threw);
    }

    void test_graph_model_and_json() {
        CHECK(manhattan_ticks(Node{1, 1, 2, {60}}, Node{2, 6, 2, {61}}) == 5);
        CHECK(manhattan_ticks(Node{1, -1, 1, {60}}, Node{2, 3, 4, {61}}) == 7);

        Graph graph;
        const int first_id = graph.add_node(1, 2, 60, 100, false, {60, 64, 67, 72, 76, 79}, RoutingMode::Counter).id;
        const int second_id = graph.add_node(4, 2, 62, 88, true).id;
        graph.connect(first_id, second_id);
        graph.connect(second_id, first_id);
        graph.set_start(second_id);
        CHECK(graph.edge_ticks(first_id) == 3);
        CHECK((graph.outgoing_edges(first_id).front() == Edge{first_id, second_id}));

        CHECK(graph.set_pitch(first_id, -50) == 0);
        CHECK(graph.transpose(first_id, 500) == 127);
        CHECK(graph.set_velocity(first_id, 500) == 127);
        CHECK(graph.adjust_velocity(first_id, -500) == 0);
        CHECK(graph.toggle_silenced(first_id));
        CHECK(graph.toggle_routing_mode(first_id) == RoutingMode::Random);
        CHECK(graph.toggle_routing_mode(first_id) == RoutingMode::Counter);

        Graph slots;
        Node &slot_node = slots.add_node(0, 0, 116);
        CHECK(slots.append_pitch(slot_node.id) == 104);
        while (slot_node.pitches.size() < 6) (void) slots.append_pitch(slot_node.id);
        expect_graph_error([&] { (void) slots.append_pitch(slot_node.id); });
        while (slot_node.pitches.size() > 1) (void) slots.remove_last_pitch(slot_node.id);
        expect_graph_error([&] { (void) slots.remove_last_pitch(slot_node.id); });

        expect_graph_error([&] { graph.add_node(1, 2); });
        expect_graph_error([&] { graph.connect(first_id, first_id); });
        expect_graph_error([&] { graph.connect(first_id, second_id); });

        const int relay_id = graph.add_relay_node(8, 2, RoutingMode::Counter).id;
        const int relay_target_id = graph.add_node(40, 9, 65).id;
        graph.connect(first_id, relay_id);
        graph.connect(relay_id, relay_target_id);
        CHECK(is_relay(*graph.find_node(relay_id)));
        CHECK(graph.find_node(relay_id)->pitches.empty());
        CHECK(graph.edge_ticks(first_id, relay_id) == 7);
        CHECK(graph.edge_ticks(relay_id, relay_target_id) == 0);
        expect_graph_error([&] { (void) graph.transpose(relay_id, 1); });
        expect_graph_error([&] { (void) graph.adjust_velocity(relay_id, 1); });
        expect_graph_error([&] { (void) graph.toggle_silenced(relay_id); });

        const int second_relay_id = graph.add_relay_node(9, 3).id;
        expect_graph_error([&] { graph.connect(relay_id, second_relay_id); });

        const ProjectSettings saved_settings{137.5, 3};
        const std::string encoded = graph.to_json(saved_settings);
        ProjectSettings decoded_settings;
        const Graph decoded = Graph::from_json(encoded, &decoded_settings);
        CHECK(decoded.to_json(decoded_settings) == encoded);
        CHECK(near(decoded_settings.bpm, 137.5));
        CHECK(decoded_settings.release_gap_eighths == 3);
        CHECK(decoded.next_node_id() == 6);
        CHECK(decoded.find_node(first_id)->pitches.size() == 6);
        CHECK(decoded.find_node(second_id)->silenced);
        CHECK(decoded.find_node(first_id)->routing_mode == RoutingMode::Counter);
        CHECK(is_relay(*decoded.find_node(relay_id)));
        CHECK(decoded.find_node(relay_id)->routing_mode == RoutingMode::Counter);
        CHECK(decoded.edge_ticks(relay_id, relay_target_id) == 0);
        CHECK(encoded.find("\"type\": \"relay\"") != std::string::npos);

        expect_graph_error([] {
            constexpr auto invalid_version = R"({
            "format": "spatial-midi-project",
            "version": 2,
            "start_node_id": null,
            "nodes": [],
            "edges": []
        })";
            (void) Graph::from_json(invalid_version);
        });
        expect_graph_error([] {
            constexpr auto invalid_bpm = R"({
            "format": "spatial-midi-project",
            "version": 1,
            "bpm": 400,
            "start_node_id": null,
            "nodes": [],
            "edges": []
        })";
            (void) Graph::from_json(invalid_bpm);
        });
        expect_graph_error([] {
            constexpr auto invalid_release_gap = R"({
            "format": "spatial-midi-project",
            "version": 1,
            "release_gap_eighths": 5,
            "start_node_id": null,
            "nodes": [],
            "edges": []
        })";
            (void) Graph::from_json(invalid_release_gap);
        });
        expect_graph_error([] {
            constexpr auto obsolete_pitch_field = R"({
            "format": "spatial-midi-project",
            "version": 1,
            "start_node_id": 1,
            "nodes": [{"id": 1, "x": 0, "y": 0, "pitch": 60}],
            "edges": []
        })";
            (void) Graph::from_json(obsolete_pitch_field);
        });
        expect_graph_error([] {
            constexpr auto empty_pitches = R"({
            "format": "spatial-midi-project",
            "version": 1,
            "start_node_id": 1,
            "nodes": [{"id": 1, "x": 0, "y": 0, "pitches": []}],
            "edges": []
        })";
            (void) Graph::from_json(empty_pitches);
        });
        expect_graph_error([] {
            constexpr auto relay_with_pitch = R"({
            "format": "spatial-midi-project",
            "version": 1,
            "start_node_id": 1,
            "nodes": [{"id": 1, "x": 0, "y": 0, "type": "relay", "pitches": [60]}],
            "edges": []
        })";
            (void) Graph::from_json(relay_with_pitch);
        });
        expect_graph_error([] {
            constexpr auto relay_chain = R"({
            "format": "spatial-midi-project",
            "version": 1,
            "start_node_id": 1,
            "nodes": [
                {"id": 1, "x": 0, "y": 0, "type": "relay"},
                {"id": 2, "x": 1, "y": 0, "type": "relay"}
            ],
            "edges": [{"source_id": 1, "target_id": 2}]
        })";
            (void) Graph::from_json(relay_chain);
        });

        const auto path = std::filesystem::temp_directory_path() / "spatial-midi-cpp-roundtrip-test.json";
        graph.save_json(path, saved_settings);
        ProjectSettings loaded_settings;
        const Graph loaded = Graph::load_json(path, &loaded_settings);
        CHECK(loaded.to_json(loaded_settings) == encoded);
        CHECK(near(loaded_settings.bpm, saved_settings.bpm));
        CHECK(loaded_settings.release_gap_eighths == saved_settings.release_gap_eighths);
        std::filesystem::remove(path);

        constexpr auto project_without_settings = R"({
            "format": "spatial-midi-project",
            "version": 1,
            "start_node_id": 1,
            "nodes": [{
                "id": 1,
                "x": 0,
                "y": 0,
                "pitches": [60],
                "routing_mode": "round_robin"
            }],
            "edges": []
        })";
        ProjectSettings defaulted_settings{200.0, 4};
        const Graph defaulted = Graph::from_json(project_without_settings, &defaulted_settings);
        CHECK(near(defaulted_settings.bpm, kDefaultTempo));
        CHECK(defaulted_settings.release_gap_eighths == kDefaultReleaseGapEighths);
        CHECK(defaulted.nodes().front().routing_mode == RoutingMode::Counter);
        CHECK(encoded.find("\"routing_mode\": \"round_robin\"") != std::string::npos);


        const Graph defaults = create_default_graph();
        const Graph quiet_defaults = create_default_graph(73);
        CHECK(defaults.nodes().size() == 4);
        CHECK(defaults.start_node_id() == defaults.nodes().front().id);
        const int expected[] = {69, 72, 69, 71};
        for (std::size_t index = 0; index < defaults.nodes().size(); ++index) {
            CHECK(defaults.nodes()[index].pitches.front() == expected[index]);
            CHECK(defaults.nodes()[index].velocity == kDefaultVelocity);
            CHECK(quiet_defaults.nodes()[index].velocity == 73);
            CHECK(defaults.edge_ticks(defaults.nodes()[index].id) == 4);
        }
        expect_graph_error([] { (void) create_default_graph(-1); });
        expect_graph_error([] { (void) create_default_graph(128); });
    }

    Graph make_chain() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(2, 0, 62).id;
        const int third = graph.add_node(2, 1, 64).id;
        graph.connect(first, second);
        graph.connect(second, third);
        graph.set_start(first);
        return graph;
    }

    void test_transparent_relay_nodes() {
        Graph graph;
        const int source = graph.add_node(0, 0, 60).id;
        const int relay = graph.add_relay_node(2, 0).id;
        const int target = graph.add_node(40, 20, 67).id;
        graph.connect(source, relay);
        graph.connect(relay, target);
        graph.set_start(source);

        FakeMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 0, 41);
        engine.start(0.0);
        const double arrival = 2.0 * sixteenth_seconds(120.0);
        CHECK(note_on_pitches(midi) == std::vector<int>({60}));
        engine.process(arrival);
        CHECK(note_on_pitches(midi) == std::vector<int>({60, 67}));

        const auto at_arrival = events_at(midi, arrival);
        CHECK(at_arrival.size() >= 2);
        CHECK(at_arrival[0].kind == RecordedMidi::Kind::Off);
        CHECK(at_arrival[0].pitches == std::vector<int>({60}));
        CHECK(at_arrival[1].kind == RecordedMidi::Kind::On);
        CHECK(at_arrival[1].pitches == std::vector<int>({67}));
        CHECK(engine.current_node_id == target);

        Graph relay_start;
        const int start_relay = relay_start.add_relay_node(0, 0).id;
        const int start_target = relay_start.add_node(100, 100, 72).id;
        relay_start.connect(start_relay, start_target);
        relay_start.set_start(start_relay);
        FakeMidi start_midi;
        SequencerEngine start_engine(relay_start, start_midi, 120.0, 0, 1, 42);
        start_engine.start(5.0);
        const RecordedMidi *start_on = first_event(start_midi, RecordedMidi::Kind::On, 72);
        CHECK(start_on && near(start_on->deadline, 5.0));
        CHECK(start_engine.current_node_id == start_target);

        Graph terminal_relay_graph;
        const int terminal_relay = terminal_relay_graph.add_relay_node(0, 0).id;
        terminal_relay_graph.set_start(terminal_relay);
        FakeMidi terminal_midi;
        SequencerEngine terminal_engine(terminal_relay_graph, terminal_midi, 120.0, 0, 1, 45);
        terminal_engine.start(6.0);
        CHECK(!terminal_engine.playing());
        CHECK(first_event(terminal_midi, RecordedMidi::Kind::On) == nullptr);

        Graph counter_graph;
        const int counter_relay = counter_graph.add_relay_node(0, 0, RoutingMode::Counter).id;
        const int first = counter_graph.add_node(1, 0, 60).id;
        const int second = counter_graph.add_node(-1, 0, 62).id;
        counter_graph.connect(counter_relay, first);
        counter_graph.connect(counter_relay, second);
        counter_graph.connect(first, counter_relay);
        counter_graph.connect(second, counter_relay);
        counter_graph.set_start(counter_relay);

        FakeMidi counter_midi;
        SequencerEngine counter_engine(counter_graph, counter_midi, 120.0, 0, 0, 43);
        counter_engine.start(0.0);
        const double tick = sixteenth_seconds(120.0);
        counter_engine.process(tick);
        counter_engine.process(2.0 * tick);
        CHECK(note_on_pitches(counter_midi) == std::vector<int>({60, 62, 60}));

        Graph external_graph;
        const int external_relay = external_graph.add_relay_node(0, 0).id;
        const int external_first = external_graph.add_node(1, 0, 65).id;
        const int external_second = external_graph.add_node(50, 50, 69).id;
        external_graph.connect(external_relay, external_first);
        external_graph.connect(external_first, external_relay);
        external_graph.connect(external_relay, external_second);
        external_graph.set_start(external_relay);
        external_graph.set_routing_mode(external_relay, RoutingMode::Counter);

        FakeMidi external_midi;
        SequencerEngine external_engine(external_graph, external_midi, 120.0, 0, 0, 44);
        external_engine.set_external_clock(true, 0.0);
        external_engine.process_external_message(kMidiStart, 0.0);
        CHECK(note_on_pitches(external_midi) == std::vector<int>({65}));
        for (int pulse = 1; pulse <= kMidiClockPulsesPerSixteenth; ++pulse) {
            external_engine.process_external_message(kMidiTimingClock, pulse / 48.0);
        }
        CHECK(note_on_pitches(external_midi) == std::vector<int>({65, 69}));
    }

    void test_internal_transport() {
        CHECK(near(5.0 * sixteenth_seconds(120.0), 0.625));
        CHECK(near(midi_clock_interval_seconds(120.0), 1.0 / 48.0));

        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(5, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);
        FakeMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 1, 1);
        engine.start(10.0);
        const double release = 10.0 + 4.875 * sixteenth_seconds(120.0);
        CHECK(engine.process(release - kEpsilon) == 0);
        CHECK(first_event(midi, RecordedMidi::Kind::Off, 60) == nullptr);
        CHECK(engine.process(release) == 1);
        const auto *off = first_event(midi, RecordedMidi::Kind::Off, 60);
        CHECK(off && near(off->deadline, release));
        engine.process(10.0 + 5.0 * sixteenth_seconds(120.0));
        CHECK(note_on_pitches(midi) == std::vector<int>({60, 62}));

        Graph chain = make_chain();
        FakeMidi chain_midi;
        SequencerEngine chain_engine(chain, chain_midi, 120.0, 0, 1, 2);
        chain_engine.start(20.0);
        chain_engine.process(21.0);
        CHECK(note_on_pitches(chain_midi) == std::vector<int>({60, 62, 64}));
        CHECK(!chain_engine.playing());

        Graph tempo_graph = make_chain();
        FakeMidi tempo_midi;
        SequencerEngine tempo_engine(tempo_graph, tempo_midi, 120.0, 0, 1, 3);
        tempo_engine.start(0.0);
        tempo_engine.set_tempo(60.0, 0.125);
        tempo_engine.process(0.250);
        CHECK(note_on_pitches(tempo_midi) == std::vector<int>({60}));
        tempo_engine.process(0.375);
        CHECK(note_on_pitches(tempo_midi) == std::vector<int>({60, 62}));

        Graph chord_graph;
        const int chord = chord_graph.add_node(0, 0, 60, 91, false, {60, 64, 67, 72, 76, 79}).id;
        const int target = chord_graph.add_node(1, 0, 62).id;
        chord_graph.connect(chord, target);
        chord_graph.set_start(chord);
        FakeMidi chord_midi;
        SequencerEngine chord_engine(chord_graph, chord_midi, 120.0, 9);
        chord_engine.start(0.0);
        const auto *on = first_event(chord_midi, RecordedMidi::Kind::On);
        CHECK(on && on->pitches.size() == 6 && on->channel == 9 && on->velocity == 91);
        chord_graph.set_last_pitch(chord, 100);
        chord_engine.process(0.109375);
        const auto *chord_off = first_event(chord_midi, RecordedMidi::Kind::Off);
        CHECK(chord_off && chord_off->pitches.back() == 79);

        Graph rest_graph;
        const int rest = rest_graph.add_node(0, 0, 60, 100, true).id;
        rest_graph.set_start(rest);
        FakeMidi rest_midi;
        SequencerEngine rest_engine(rest_graph, rest_midi);
        rest_engine.start(0.0);
        rest_engine.process(sixteenth_seconds(120.0));
        CHECK(note_on_pitches(rest_midi).empty());
        CHECK(!rest_engine.playing());
    }

    void test_routing_pause_clock_and_deletion() {
        Graph graph;
        const int source = graph.add_node(0, 0, 60, 100, false, {}, RoutingMode::Counter).id;
        const int a = graph.add_node(1, 0, 62).id;
        const int b = graph.add_node(0, 1, 67).id;
        graph.connect(source, a);
        graph.connect(source, b);
        graph.connect(a, source);
        graph.connect(b, source);
        graph.set_start(source);
        FakeMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 1, 99);
        engine.start(0.0);
        engine.process(0.375);
        CHECK(note_on_pitches(midi) == std::vector<int>({60, 62, 60, 67}));

        engine.stop();
        midi.events.clear();
        engine.start(1.0);
        CHECK(engine.pause(1.04));
        CHECK(engine.state == TransportState::Paused);
        CHECK(engine.resume(2.04));
        CHECK(engine.playing());
        engine.process(2.125);
        CHECK(note_on_pitches(midi).front() == 60);

        Graph clock_graph;
        const int first = clock_graph.add_node(0, 0, 60).id;
        const int second = clock_graph.add_node(1, 0, 62).id;
        clock_graph.connect(first, second);
        clock_graph.set_start(first);
        FakeMidi clock_midi;
        SequencerEngine clock_engine(clock_graph, clock_midi, 120.0);
        CHECK(clock_engine.toggle_midi_clock(true, 3.0));
        clock_engine.start(3.0);
        CHECK(clock_midi.events.size() >= 2);
        CHECK(clock_midi.events[clock_midi.events.size() - 2].kind == RecordedMidi::Kind::On);
        CHECK(clock_midi.events.back().kind == RecordedMidi::Kind::Realtime);
        CHECK(clock_midi.events.back().status == kMidiTimingClock);

        Graph external_graph;
        const int external_first = external_graph.add_node(0, 0, 60).id;
        const int external_second = external_graph.add_node(1, 0, 62).id;
        external_graph.connect(external_first, external_second);
        external_graph.set_start(external_first);
        FakeMidi external_midi;
        SequencerEngine external_engine(external_graph, external_midi, 120.0);
        external_engine.set_external_clock(true, 0.0);
        external_engine.process_external_message(kMidiStart, 0.0);
        for (int pulse = 1; pulse <= 6; ++pulse) {
            external_engine.process_external_message(kMidiTimingClock, pulse / 48.0);
        }
        CHECK(note_on_pitches(external_midi) == std::vector<int>({60, 62}));

        Graph delete_graph;
        const int d1 = delete_graph.add_node(0, 0, 60).id;
        const int d2 = delete_graph.add_node(2, 0, 62).id;
        delete_graph.connect(d1, d2);
        delete_graph.set_start(d1);
        FakeMidi delete_midi;
        SequencerEngine delete_engine(delete_graph, delete_midi);
        delete_engine.start(0.0);
        delete_graph.delete_node(d2);
        CHECK(!delete_engine.handle_node_deleted(d2));
        delete_engine.process(0.25);
        CHECK(!delete_engine.playing());
    }


    std::vector<RecordedMidi> events_at(const FakeMidi &midi, double deadline, double tolerance) {
        std::vector<RecordedMidi> result;
        for (const auto &event: midi.events) {
            if ((event.kind == RecordedMidi::Kind::On || event.kind == RecordedMidi::Kind::Off || event.kind ==
                 RecordedMidi::Kind::Realtime) && near(event.deadline, deadline, tolerance)) {
                result.push_back(event);
            }
        }
        return result;
    }

    void test_release_gap_and_absolute_deadlines() {
        for (int gap = kMinReleaseGapEighths; gap <= kMaxReleaseGapEighths; ++gap) {
            Graph graph;
            const int first = graph.add_node(0, 0, 60).id;
            const int second = graph.add_node(1, 0, 62).id;
            graph.connect(first, second);
            graph.set_start(first);
            FakeMidi midi;
            SequencerEngine engine(graph, midi, 120.0, 0, gap, 10);
            engine.start(1.0);
            const double expected = 1.0 + (1.0 - gap / 8.0) * sixteenth_seconds(120.0);
            engine.process(expected - kEpsilon);
            CHECK(first_event(midi, RecordedMidi::Kind::Off) == nullptr);
            engine.process(expected);
            const auto *off = first_event(midi, RecordedMidi::Kind::Off);
            CHECK(off && near(off->deadline, expected));
        }

        Graph graph = make_chain();
        FakeMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 1, 11);
        engine.start(0.0);
        engine.process(10.0);
        std::vector<double> note_on_deadlines;
        for (const auto &event: midi.events) {
            if (event.kind == RecordedMidi::Kind::On) {
                note_on_deadlines.push_back(event.deadline);
            }
        }
        CHECK(note_on_deadlines.size() == 3);
        CHECK(near(note_on_deadlines[0], 0.0));
        CHECK(near(note_on_deadlines[1], 2.0 * sixteenth_seconds(120.0)));
        CHECK(near(note_on_deadlines[2], 3.0 * sixteenth_seconds(120.0)));

        bool invalid_low = false;
        try { SequencerEngine invalid(graph, midi, 120.0, 0, -1); } catch (const std::invalid_argument &) {
            invalid_low = true;
        }
        CHECK(invalid_low);
        bool invalid_high = false;
        try { SequencerEngine invalid(graph, midi, 120.0, 0, 5); } catch (const std::invalid_argument &) {
            invalid_high = true;
        }
        CHECK(invalid_high);
    }

    void test_clock_output_order_and_long_run() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(1, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);
        FakeMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 1, 12);
        engine.toggle_midi_clock(true, 0.0);
        engine.start(0.0);
        CHECK(midi.events.size() >= 5);
        CHECK(midi.events[0].kind == RecordedMidi::Kind::Clear);
        CHECK(midi.events[1].kind == RecordedMidi::Kind::AllOff);
        CHECK(midi.events[2].kind == RecordedMidi::Kind::Realtime && midi.events[2].status == kMidiStart);
        CHECK(midi.events[3].kind == RecordedMidi::Kind::On);
        CHECK(midi.events[4].kind == RecordedMidi::Kind::Realtime && midi.events[4].status == kMidiTimingClock);

        const double boundary = sixteenth_seconds(120.0);
        engine.process(boundary);
        const auto boundary_events = events_at(midi, boundary);
        CHECK(boundary_events.size() >= 2);
        CHECK(boundary_events[boundary_events.size() - 2].kind == RecordedMidi::Kind::On);
        CHECK(boundary_events.back().kind == RecordedMidi::Kind::Realtime);
        CHECK(boundary_events.back().status == kMidiTimingClock);

        Graph loop;
        const int a = loop.add_node(0, 0, 60).id;
        const int b = loop.add_node(1, 0, 62).id;
        loop.connect(a, b);
        loop.connect(b, a);
        loop.set_start(a);
        FakeMidi long_midi;
        SequencerEngine long_engine(loop, long_midi, 123.0, 0, 0, 13);
        long_engine.toggle_midi_clock(true, 0.0);
        long_engine.start(0.0);
        long_midi.events.clear();
        const double tick = sixteenth_seconds(123.0);
        int sounding = 60;
        const int tick_count = static_cast<int>(30.0 * 60.0 / tick);
        for (int index = 1; index <= tick_count; ++index) {
            const int next = sounding == 60 ? 62 : 60;
            long_engine.process(index * tick + 1e-6);
            CHECK(long_midi.events.size() >= 3);
            const std::size_t n = long_midi.events.size();
            CHECK(long_midi.events[n - 3].kind == RecordedMidi::Kind::Off);
            CHECK(long_midi.events[n - 3].pitches == std::vector<int>({sounding}));
            CHECK(long_midi.events[n - 2].kind == RecordedMidi::Kind::On);
            CHECK(long_midi.events[n - 2].pitches == std::vector<int>({next}));
            CHECK(long_midi.events[n - 1].kind == RecordedMidi::Kind::Realtime);
            CHECK(long_midi.events[n - 1].status == kMidiTimingClock);
            long_midi.events.clear();
            sounding = next;
        }
        CHECK(long_engine.playing());
    }

    void test_live_clock_switch_and_tempo_phase() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(4, 0, 62).id;
        graph.connect(first, second);
        graph.set_start(first);
        FakeMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 1, 14);
        engine.start(0.0);
        engine.toggle_midi_clock(true, 0.050);
        CHECK(engine.clock_switch_pending() == "output_on");
        engine.process(0.124999);
        CHECK(std::ranges::none_of(midi.events, [](const RecordedMidi& e) { return e.kind ==
            RecordedMidi::Kind::Realtime && e.status == kMidiStart; }));
        engine.process(0.125);
        CHECK(midi.events[midi.events.size() - 2].kind == RecordedMidi::Kind::Realtime);
        CHECK(midi.events[midi.events.size() - 2].status == kMidiStart);
        CHECK(midi.events.back().kind == RecordedMidi::Kind::Realtime && midi.events.back().status == kMidiTimingClock);

        engine.toggle_midi_clock(false, 0.130);
        CHECK(engine.clock_switch_pending() == "output_off");
        engine.process(0.250);
        CHECK(midi.events[midi.events.size() - 2].status == kMidiTimingClock);
        CHECK(midi.events.back().status == kMidiStop);

        Graph tempo_graph;
        const int t1 = tempo_graph.add_node(0, 0, 60).id;
        const int t2 = tempo_graph.add_node(2, 0, 62).id;
        tempo_graph.connect(t1, t2);
        tempo_graph.set_start(t1);
        FakeMidi tempo_midi;
        SequencerEngine tempo_engine(tempo_graph, tempo_midi, 120.0, 0, 1, 15);
        tempo_engine.toggle_midi_clock(true, 0.0);
        tempo_engine.start(0.0);
        tempo_engine.set_tempo(60.0, 0.050);
        tempo_engine.process(0.450);
        const auto at_boundary = events_at(tempo_midi, 0.450, 1e-7);
        bool saw_target = false;
        bool saw_clock_after = false;
        for (const auto &event: at_boundary) {
            if (event.kind == RecordedMidi::Kind::On && event.pitches == std::vector<int>({62})) {
                saw_target = true;
            }
            if (saw_target && event.kind == RecordedMidi::Kind::Realtime && event.status == kMidiTimingClock) {
                saw_clock_after = true;
            }
        }
        CHECK(saw_target && saw_clock_after);
    }

    void test_external_clock_and_handoffs() {
        Graph graph;
        const int first = graph.add_node(0, 0, 60).id;
        const int second = graph.add_node(1, 0, 62).id;
        graph.connect(first, second);
        graph.connect(second, first);
        graph.set_start(first);
        FakeMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 0, 1, 16);
        engine.set_external_clock(true, 0.0);
        engine.process_external_message(kMidiStart, 0.0);
        for (int pulse = 1; pulse <= 48; ++pulse) {
            engine.process_external_message(kMidiTimingClock, pulse / 48.0);
        }
        CHECK(std::abs(engine.bpm - 120.0) < 1e-6);
        CHECK(note_on_pitches(midi).size() >= 9);
        CHECK(engine.process_external_message(kMidiContinue, 2.0) == 0);
        engine.process_external_message(kMidiStop, 2.1);
        CHECK(!engine.playing());
        CHECK(first_event(midi, RecordedMidi::Kind::AllOff) != nullptr);

        Graph handoff;
        const int h1 = handoff.add_node(0, 0, 60).id;
        const int h2 = handoff.add_node(4, 0, 62).id;
        handoff.connect(h1, h2);
        handoff.set_start(h1);
        FakeMidi handoff_midi;
        SequencerEngine handoff_engine(handoff, handoff_midi, 120.0, 0, 1, 17);
        handoff_engine.start(0.0);
        handoff_engine.set_external_clock(true, 0.010);
        for (int pulse = 1; pulse <= 6; ++pulse) {
            handoff_engine.process_external_message(kMidiTimingClock, pulse / 48.0);
        }
        CHECK(handoff_engine.external_clock_active());
        handoff_engine.set_external_clock(false, 0.126);
        for (int pulse = 7; pulse <= 12; ++pulse) {
            handoff_engine.process_external_message(kMidiTimingClock, pulse / 48.0);
        }
        CHECK(!handoff_engine.external_clock_active());
        CHECK(handoff_engine.playing());
    }

    void test_cleanup_overrun_and_validation() {
        Graph graph;
        const int node = graph.add_node(0, 0, 60).id;
        graph.set_start(node);
        FakeMidi midi;
        SequencerEngine engine(graph, midi, 120.0, 7);
        engine.start(0.0);
        engine.stop();
        CHECK(!engine.playing());
        CHECK(engine.scheduled_event_count() == 0);
        CHECK(std::ranges::any_of(midi.events, [](const RecordedMidi& e) { return e.kind ==
            RecordedMidi::Kind::AllOff && e.channel == 7; }));
        CHECK(std::ranges::any_of(midi.events, [](const RecordedMidi& e) { return e.kind ==
            RecordedMidi::Kind::Clear; }));

        engine.start(1.0);
        engine.timing_overrun(6.0);
        CHECK(engine.state == TransportState::TimingOverrun);
        CHECK(engine.overrun_count == 1);

        expect_graph_error([] {
            constexpr auto invalid_routing_mode = R"({
            "format": "spatial-midi-project",
            "version": 1,
            "start_node_id": 1,
            "nodes": [{
                "id": 1,
                "x": 0,
                "y": 0,
                "pitches": [60],
                "routing_mode": "weighted"
            }],
            "edges": []
        })";
            (void) Graph::from_json(invalid_routing_mode);
        });
        expect_graph_error([] {
            constexpr auto duplicate_edges = R"({
            "format": "spatial-midi-project",
            "version": 1,
            "start_node_id": 1,
            "nodes": [
                {"id": 1, "x": 0, "y": 0, "pitches": [60]},
                {"id": 2, "x": 1, "y": 0, "pitches": [62]}
            ],
            "edges": [
                {"source_id": 1, "target_id": 2},
                {"source_id": 1, "target_id": 2}
            ]
        })";
            (void) Graph::from_json(duplicate_edges);
        });
    }

    void test_midi_note_input_worker() {
        auto input = std::make_shared<FakeNoteInput>(std::vector<MidiNoteMessage>{
            {.pitch = 60, .velocity = 80, .channel = 1},
            {.pitch = 61, .velocity = 0, .channel = 0},
            {.pitch = 62, .velocity = 96, .channel = 0},
        });

        std::mutex mutex;
        std::condition_variable condition;
        std::vector<MidiNoteMessage> received;
        MidiNoteInputWorker worker(input, 0, [&](const MidiNoteMessage &note) {
            {
                std::lock_guard lock(mutex);
                received.push_back(note);
            }
            condition.notify_one();
        });

        {
            std::unique_lock lock(mutex);
            CHECK(condition.wait_for(lock, std::chrono::milliseconds(100), [&] { return !received.empty(); }));
            CHECK(received.size() == 1);
            CHECK(received.front().pitch == 62);
            CHECK(received.front().velocity == 96);
            CHECK(received.front().channel == 0);
        }
        worker.close();

        auto failing_input = std::make_shared<FailingNoteInput>();
        MidiNoteInputWorker failing_worker(failing_input, 0, [](const MidiNoteMessage &) {});
        std::optional<std::string> failure;
        for (int attempt = 0; attempt < 100 && !failure; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            failure = failing_worker.pop_failure();
        }
        CHECK(failure.has_value());
        CHECK(*failure == "simulated note input failure");
        failing_worker.close();
    }

    void test_transport_worker() {
        auto midi = std::make_shared<FakeMidi>();
        TransportWorker worker(create_default_graph(), midi, 120.0, 4);
        worker.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const TransportSnapshot running = worker.snapshot();
        CHECK(running.playing);
        CHECK(running.output_channel == 4);
        CHECK(worker.adjust_release_gap_eighths(100) == 4);
        Graph replacement = create_default_graph();
        worker.replace_project(std::move(replacement), ProjectSettings{135.0, 2});
        const TransportSnapshot replaced = worker.snapshot();
        CHECK(near(replaced.bpm, 135.0));
        CHECK(replaced.release_gap_eighths == 2);
        const int node_id = worker.graph_snapshot().nodes().front().id;
        CHECK(worker.toggle_routing_mode(node_id) == RoutingMode::Counter);

        worker.stop();
        CHECK(!worker.snapshot().playing);
        worker.edit_graph([node_id](Graph &graph) {
            (void) graph.append_pitch(node_id);
            graph.set_silenced(node_id, true);
        });
        const Graph before_entry = worker.graph_snapshot();
        const int secondary_pitch = before_entry.find_node(node_id)->pitches[1];
        CHECK(worker.set_node_from_midi(node_id, 71, 91));
        const Graph after_entry = worker.graph_snapshot();
        const Node *edited = after_entry.find_node(node_id);
        CHECK(edited != nullptr);
        CHECK(edited->pitches.front() == 71);
        CHECK(edited->pitches[1] == secondary_pitch);
        CHECK(edited->velocity == 91);
        CHECK(edited->silenced);

        const int relay_id = worker.edit_graph([](Graph &graph) {
            return graph.add_relay_node(100, 100).id;
        });
        CHECK(!worker.set_node_from_midi(relay_id, 72, 92));

        worker.start();
        CHECK(!worker.set_node_from_midi(node_id, 73, 93));
        CHECK(worker.pause());
        CHECK(!worker.set_node_from_midi(node_id, 73, 93));
        worker.stop();
        worker.close();
    }
}

int main() {
    try {
        test_graph_model_and_json();
        test_transparent_relay_nodes();
        test_internal_transport();
        test_routing_pause_clock_and_deletion();
        test_release_gap_and_absolute_deadlines();
        test_clock_output_order_and_long_run();
        test_live_clock_switch_and_tempo_phase();
        test_external_clock_and_handoffs();
        test_cleanup_overrun_and_validation();
        test_midi_note_input_worker();
        test_transport_worker();
    } catch (const std::exception &error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All core tests passed.\n";
    return 0;
}
