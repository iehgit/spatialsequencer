#pragma once

#include "spatial_midi/core/midi_io.hpp"

#include <alsa/asoundlib.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spatial_midi {
    struct AlsaPortAddress {
        int client = -1;
        int port = -1;

        friend bool operator==(const AlsaPortAddress &, const AlsaPortAddress &) = default;
    };

    struct AlsaPortInfo {
        AlsaPortAddress address;
        std::string client_name;
        std::string port_name;
    };

    class AlsaMidiOutput final : public MidiOutput {
    public:
        explicit AlsaMidiOutput(const std::string &client_name = "Spatial MIDI Graph Sequencer");
        ~AlsaMidiOutput() override;
        AlsaMidiOutput(const AlsaMidiOutput &) = delete;
        AlsaMidiOutput &operator=(const AlsaMidiOutput &) = delete;

        void connect_to(AlsaPortAddress destination);
        [[nodiscard]] std::string description() const override;
        void notes_on(std::span<const int> pitches, int velocity, int channel, double deadline) override;
        void notes_off(std::span<const int> pitches, int velocity, int channel, double deadline) override;
        void all_notes_off(int channel) override;
        void send_realtime(std::uint8_t status, double deadline) override;
        void clear_scheduled() override;
        [[nodiscard]] int client_id() const noexcept;
        [[nodiscard]] std::optional<AlsaPortAddress> connected_destination() const;
        [[nodiscard]] static std::vector<AlsaPortInfo> list_destinations();
        [[nodiscard]] static std::optional<AlsaPortAddress> parse_address(std::string_view text);

    private:
        void check(int result, const char *operation) const;
        void create_queue_locked();
        void destroy_queue_locked() noexcept;
        void calibrate_queue_clock_locked();
        [[nodiscard]] snd_seq_real_time_t scheduled_time_locked(double deadline) const;
        void prepare_event_locked(snd_seq_event_t &event, const snd_seq_real_time_t &scheduled_time);
        void write_channel_event_locked(bool note_on, int pitch, int velocity, int channel,
                                        const snd_seq_real_time_t &time);
        void write_cc_direct_locked(int channel, int controller, int value);

        snd_seq_t *seq_ = nullptr;
        int port_ = -1;
        int queue_ = -1;
        double monotonic_epoch_ = 0.0;
        std::uint64_t queue_epoch_ns_ = 0;
        std::optional<AlsaPortAddress> destination_;
        std::string destination_description_;
        mutable std::mutex mutex_;
    };

    class AlsaMidiClockInput final : public MidiClockInput {
    public:
        explicit AlsaMidiClockInput(AlsaPortAddress source,
                                   const std::string &client_name = "Spatial MIDI Clock Input");
        ~AlsaMidiClockInput() override;
        AlsaMidiClockInput(const AlsaMidiClockInput &) = delete;
        AlsaMidiClockInput &operator=(const AlsaMidiClockInput &) = delete;

        [[nodiscard]] std::string description() const override;
        std::vector<MidiRealtimeMessage> poll_realtime(std::size_t limit = 128) override;
        [[nodiscard]] static std::vector<AlsaPortInfo> list_sources();

    private:
        static std::optional<std::uint8_t> realtime_status(int event_type) noexcept;
        static void check(int result, const char *operation);
        void create_timestamp_queue_locked();
        void destroy_timestamp_queue_locked() noexcept;
        void create_input_port_locked();
        void calibrate_queue_clock_locked();
        [[nodiscard]] double event_timestamp(const snd_seq_event_t &event, double fallback) const noexcept;

        snd_seq_t *seq_ = nullptr;
        int port_ = -1;
        int queue_ = -1;
        AlsaPortAddress source_;
        std::string source_description_;
        double monotonic_epoch_ = 0.0;
        std::uint64_t queue_epoch_ns_ = 0;
        mutable std::mutex mutex_;
    };

    class AlsaMidiNoteInput final : public MidiNoteInput {
    public:
        explicit AlsaMidiNoteInput(AlsaPortAddress source, const std::string &client_name = "Spatial MIDI Note Input");
        ~AlsaMidiNoteInput() override;
        AlsaMidiNoteInput(const AlsaMidiNoteInput &) = delete;
        AlsaMidiNoteInput &operator=(const AlsaMidiNoteInput &) = delete;

        [[nodiscard]] std::string description() const override;
        std::optional<MidiNoteMessage> read_note(std::chrono::milliseconds timeout) override;
        [[nodiscard]] static std::vector<AlsaPortInfo> list_sources();

    private:
        static void check(int result, const char *operation);
        void create_input_port();

        snd_seq_t *seq_ = nullptr;
        int port_ = -1;
        AlsaPortAddress source_;
        std::string source_description_;
    };
}
