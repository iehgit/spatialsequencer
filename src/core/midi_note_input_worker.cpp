#include "spatial_midi/core/midi_note_input_worker.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace spatial_midi {
    namespace {
        constexpr auto kReadTimeout = std::chrono::milliseconds(50);
    }

    MidiNoteInputWorker::MidiNoteInputWorker(std::shared_ptr<MidiNoteInput> input, int channel, NoteHandler handler)
        : input_(std::move(input)), channel_(channel), handler_(std::move(handler)) {
        if (!input_) {
            throw std::invalid_argument("MIDI note input cannot be null");
        }
        if (channel_ < 0 || channel_ > 15) {
            throw std::invalid_argument("MIDI note input channel must be between 0 and 15");
        }
        if (!handler_) {
            throw std::invalid_argument("MIDI note input handler cannot be empty");
        }

        worker_ = std::jthread([this](std::stop_token stop_token) { run(stop_token); });
    }

    MidiNoteInputWorker::~MidiNoteInputWorker() {
        close();
    }

    std::optional<std::string> MidiNoteInputWorker::pop_failure() {
        std::lock_guard lock(failure_mutex_);
        std::optional<std::string> result = std::move(failure_);
        failure_.reset();
        return result;
    }

    void MidiNoteInputWorker::close() {
        worker_.request_stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void MidiNoteInputWorker::run(std::stop_token stop_token) {
        try {
            while (!stop_token.stop_requested()) {
                const auto note = input_->read_note(kReadTimeout);
                if (!note || note->channel != channel_ || note->velocity == 0) {
                    continue;
                }
                handler_(*note);
            }
        } catch (const std::exception &error) {
            std::lock_guard lock(failure_mutex_);
            failure_ = error.what();
        } catch (...) {
            std::lock_guard lock(failure_mutex_);
            failure_ = "Unknown MIDI note input failure";
        }
    }
}
