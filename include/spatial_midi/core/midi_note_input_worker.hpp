#pragma once

#include "spatial_midi/core/midi_io.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace spatial_midi {
    class MidiNoteInputWorker {
    public:
        using NoteHandler = std::function<void(const MidiNoteMessage &)>;

        MidiNoteInputWorker(std::shared_ptr<MidiNoteInput> input, int channel, NoteHandler handler);

        ~MidiNoteInputWorker();

        MidiNoteInputWorker(const MidiNoteInputWorker &) = delete;

        MidiNoteInputWorker &operator=(const MidiNoteInputWorker &) = delete;

        [[nodiscard]] std::optional<std::string> pop_failure();

        void close();

    private:
        void run(std::stop_token stop_token);

        std::shared_ptr<MidiNoteInput> input_;
        int channel_ = 0;
        NoteHandler handler_;
        std::mutex failure_mutex_;
        std::optional<std::string> failure_;
        std::jthread worker_;
    };
}
