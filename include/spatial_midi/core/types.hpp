#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spatial_midi {
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    inline constexpr int kMinMidiPitch = 0;
    inline constexpr int kMaxMidiPitch = 127;
    inline constexpr int kDefaultVelocity = 100;
    inline constexpr int kMinPitchSlots = 1;
    inline constexpr int kMaxPitchSlots = 6;
    inline constexpr int kMinTempo = 20;
    inline constexpr int kMaxTempo = 300;
    inline constexpr double kDefaultTempo = 120.0;
    inline constexpr int kMinReleaseGapEighths = 0;
    inline constexpr int kMaxReleaseGapEighths = 4;
    inline constexpr int kDefaultReleaseGapEighths = 1;
    inline constexpr int kMidiClockPulsesPerSixteenth = 6;

    inline constexpr std::uint8_t kMidiTimingClock = 0xF8;
    inline constexpr std::uint8_t kMidiStart = 0xFA;
    inline constexpr std::uint8_t kMidiContinue = 0xFB;
    inline constexpr std::uint8_t kMidiStop = 0xFC;

    enum class RoutingMode {
        Random,
        Counter,
    };

    enum class NodeType {
        Musical,
        Relay,
    };

    enum class TransportState {
        Stopped,
        WaitingForClock,
        Running,
        Paused,
        ClockLost,
        TimingOverrun,
        DeviceError,
    };

    enum class ExternalClockSwitch {
        ToExternal,
        ToInternal,
    };

    struct Node {
        int id = 0;
        int x = 0;
        int y = 0;
        std::vector<int> pitches{60};
        int velocity = kDefaultVelocity;
        bool silenced = false;
        RoutingMode routing_mode = RoutingMode::Random;
        NodeType type = NodeType::Musical;
    };

    struct Edge {
        int source_id = 0;
        int target_id = 0;

        friend bool operator==(const Edge &, const Edge &) = default;
    };

    // Realtime input timestamps use the same steady-clock seconds domain as the
    // sequencer engine. MIDI backends are responsible for mapping device or kernel
    // timestamps into that domain before returning messages.
    struct MidiRealtimeMessage {
        std::uint8_t status = 0;
        double timestamp = 0.0;
    };

    struct TransportSnapshot {
        bool playing = false;
        std::optional<int> current_node_id;
        double bpm = kDefaultTempo;
        bool midi_clock_enabled = false;
        bool midi_clock_active = false;
        std::optional<bool> midi_clock_output_switch_pending;
        bool external_clock_enabled = false;
        int output_channel = 0;
        int release_gap_eighths = kDefaultReleaseGapEighths;
        bool external_clock_active = false;
        std::optional<ExternalClockSwitch> external_clock_switch_pending;
        TransportState state = TransportState::Stopped;
        double max_event_lateness_ms = 0.0;
        std::uint64_t missed_deadlines = 0;
        std::uint64_t input_gaps = 0;
        std::uint64_t overrun_count = 0;
        bool worker_alive = true;
        bool worker_responsive = true;
    };

    [[nodiscard]] inline int clamp_midi(int value) noexcept {
        return std::clamp(value, kMinMidiPitch, kMaxMidiPitch);
    }

    [[nodiscard]] inline bool is_relay(const Node &node) noexcept {
        return node.type == NodeType::Relay;
    }

    [[nodiscard]] inline std::string routing_mode_name(RoutingMode mode) {
        return mode == RoutingMode::Counter ? "round_robin" : "random";
    }

    [[nodiscard]] inline std::string transport_state_name(TransportState state) {
        switch (state) {
            case TransportState::Stopped:
                return "Stopped";
            case TransportState::WaitingForClock:
                return "Waiting for clock";
            case TransportState::Running:
                return "Running";
            case TransportState::Paused:
                return "Paused";
            case TransportState::ClockLost:
                return "Clock lost";
            case TransportState::TimingOverrun:
                return "Timing overrun";
            case TransportState::DeviceError:
                return "Device error";
        }
        return "Stopped";
    }

    [[nodiscard]] inline std::string pitch_name(int pitch) {
        static constexpr const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",};

        const int note = clamp_midi(pitch);
        return std::string(names[note % 12]) + std::to_string(note / 12 - 1);
    }

    [[nodiscard]] inline double monotonic_seconds() {
        return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    }
}
