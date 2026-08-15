#pragma once

#include "spatial_midi/core/types.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace spatial_midi {
    class MidiOutput {
    public:
        virtual ~MidiOutput() = default;

        [[nodiscard]] virtual std::string description() const = 0;

        virtual void notes_on(std::span<const int> pitches, int velocity, int channel, TimePoint deadline) = 0;

        virtual void notes_off(std::span<const int> pitches, int velocity, int channel, TimePoint deadline) = 0;

        virtual void all_notes_off(int channel) = 0;

        virtual void send_realtime(std::uint8_t status, TimePoint deadline) = 0;

        virtual void clear_scheduled() = 0;
    };

    class MidiClockInput {
    public:
        virtual ~MidiClockInput() = default;

        [[nodiscard]] virtual std::string description() const = 0;

        virtual std::vector<MidiRealtimeMessage> poll_realtime(std::size_t limit = 128) = 0;
    };

    struct MidiNoteMessage {
        int pitch = 0;
        int velocity = 0;
        int channel = 0;
    };

    class MidiNoteInput {
    public:
        virtual ~MidiNoteInput() = default;

        [[nodiscard]] virtual std::string description() const = 0;

        virtual std::optional<MidiNoteMessage> read_note(std::chrono::milliseconds timeout) = 0;
    };

    class NullMidiOutput final : public MidiOutput {
    public:
        [[nodiscard]] std::string description() const override { return "No MIDI output"; }

        void notes_on(std::span<const int>, int, int, TimePoint) override {
        }

        void notes_off(std::span<const int>, int, int, TimePoint) override {
        }

        void all_notes_off(int) override {
        }

        void send_realtime(std::uint8_t, TimePoint) override {
        }

        void clear_scheduled() override {
        }
    };

    class NullMidiClockInput final : public MidiClockInput {
    public:
        [[nodiscard]] std::string description() const override { return "No MIDI Clock input"; }
        std::vector<MidiRealtimeMessage> poll_realtime(std::size_t) override { return {}; }
    };
}
