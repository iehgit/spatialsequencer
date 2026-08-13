#include "spatial_midi/midi/alsa_sequencer.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <poll.h>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>

namespace spatial_midi {
    namespace {
        constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

        // ALSA stores source-port, queue, channel, note, and velocity fields in a
        // single byte. Keep every narrowing conversion explicit and validate values
        // whose range is not already guaranteed by the MIDI model.
        unsigned char checked_alsa_byte(int value, std::string_view field_name) {
            constexpr int kMaxAlsaByte = std::numeric_limits<unsigned char>::max();
            if (value < 0 || value > kMaxAlsaByte) {
                throw std::out_of_range(std::string(field_name) + " does not fit in an ALSA event byte");
            }
            return static_cast<unsigned char>(value);
        }

        unsigned char checked_midi_channel(int channel) {
            constexpr int kMaxMidiChannel = 15;
            if (channel < 0 || channel > kMaxMidiChannel) {
                throw std::out_of_range("MIDI channel must be in the range 0..15");
            }
            return static_cast<unsigned char>(channel);
        }

        unsigned char clamped_midi_byte(int value) noexcept {
            return static_cast<unsigned char>(clamp_midi(value));
        }

        std::uint64_t realtime_to_ns(const snd_seq_real_time_t &value) {
            return static_cast<std::uint64_t>(value.tv_sec) * kNanosecondsPerSecond +
                   static_cast<std::uint64_t>(value.tv_nsec);
        }

        snd_seq_real_time_t ns_to_realtime(std::uint64_t value) {
            snd_seq_real_time_t result{};
            result.tv_sec = static_cast<unsigned int>(value / kNanosecondsPerSecond);
            result.tv_nsec = static_cast<unsigned int>(value % kNanosecondsPerSecond);
            return result;
        }

        std::vector<AlsaPortInfo> list_ports(unsigned int required_capabilities) {
            snd_seq_t *raw_seq = nullptr;
            const int open_result = snd_seq_open(&raw_seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
            if (open_result < 0) {
                throw std::runtime_error(std::string("snd_seq_open: ") + snd_strerror(open_result));
            }

            const auto close_seq = [](snd_seq_t *seq) {
                if (seq != nullptr) {
                    snd_seq_close(seq);
                }
            };
            std::unique_ptr<snd_seq_t, decltype(close_seq)> seq(raw_seq, close_seq);

            std::vector<AlsaPortInfo> result;
            snd_seq_client_info_t *client_info = nullptr;
            snd_seq_port_info_t *port_info = nullptr;
            snd_seq_client_info_alloca(&client_info);
            snd_seq_port_info_alloca(&port_info);

            snd_seq_client_info_set_client(client_info, -1);
            while (snd_seq_query_next_client(seq.get(), client_info) >= 0) {
                const int client = snd_seq_client_info_get_client(client_info);
                snd_seq_port_info_set_client(port_info, client);
                snd_seq_port_info_set_port(port_info, -1);

                while (snd_seq_query_next_port(seq.get(), port_info) >= 0) {
                    const unsigned int capabilities = snd_seq_port_info_get_capability(port_info);
                    if ((capabilities & required_capabilities) != required_capabilities) {
                        continue;
                    }

                    result.push_back(AlsaPortInfo{
                        .address = {client, snd_seq_port_info_get_port(port_info)},
                        .client_name = snd_seq_client_info_get_name(client_info),
                        .port_name = snd_seq_port_info_get_name(port_info),
                    });
                }
            }

            return result;
        }
    }

    AlsaMidiOutput::AlsaMidiOutput(const std::string &client_name) {
        check(snd_seq_open(&seq_, "default", SND_SEQ_OPEN_OUTPUT, 0), "snd_seq_open");

        try {
            check(snd_seq_set_client_name(seq_, client_name.c_str()), "snd_seq_set_client_name");

            port_ = snd_seq_create_simple_port(seq_, "MIDI Out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                               SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
            check(port_, "snd_seq_create_simple_port");

            std::lock_guard lock(mutex_);
            create_queue_locked();
        } catch (...) {
            snd_seq_close(seq_);
            seq_ = nullptr;
            throw;
        }
    }

    AlsaMidiOutput::~AlsaMidiOutput() {
        try {
            all_notes_off(0);
        } catch (...) {
            // Destructors must not throw. Closing the ALSA client still releases
            // the port and any remaining queue resources.
        }

        std::lock_guard lock(mutex_);
        if (seq_ == nullptr) {
            return;
        }

        destroy_queue_locked();
        snd_seq_close(seq_);
        seq_ = nullptr;
    }

    void AlsaMidiOutput::connect_to(AlsaPortAddress destination) {
        std::lock_guard lock(mutex_);

        if (destination_) {
            snd_seq_disconnect_to(seq_, port_, destination_->client, destination_->port);
        }

        check(snd_seq_connect_to(seq_, port_, destination.client, destination.port), "snd_seq_connect_to");
        destination_ = destination;
        destination_description_.clear();

        // Device-name lookup is optional and must not make a successful connection fail.
        try {
            const auto ports = list_destinations();
            const auto match = std::ranges::find(ports, destination, &AlsaPortInfo::address);
            if (match != ports.end()) {
                destination_description_ = match->client_name;
                if (!match->port_name.empty() && match->port_name != match->client_name) {
                    destination_description_ += " / " + match->port_name;
                }
            }
        } catch (...) {
            destination_description_.clear();
        }
    }

    std::string AlsaMidiOutput::description() const {
        std::lock_guard lock(mutex_);

        std::string result = "MIDI: ALSA " + std::to_string(client_id()) + ':' + std::to_string(port_);
        if (destination_) {
            result += " -> " + std::to_string(destination_->client) + ':' + std::to_string(destination_->port);
            if (!destination_description_.empty()) {
                result += " (" + destination_description_ + ')';
            }
        }
        return result;
    }

    void AlsaMidiOutput::notes_on(std::span<const int> pitches, int velocity, int channel, double deadline) {
        std::lock_guard lock(mutex_);
        const snd_seq_real_time_t time = scheduled_time_locked(deadline);

        for (int pitch: pitches) {
            write_channel_event_locked(true, pitch, velocity, channel, time);
        }
        check(snd_seq_drain_output(seq_), "snd_seq_drain_output");
    }

    void AlsaMidiOutput::notes_off(std::span<const int> pitches, int velocity, int channel, double deadline) {
        std::lock_guard lock(mutex_);
        const snd_seq_real_time_t time = scheduled_time_locked(deadline);

        for (int pitch: pitches) {
            write_channel_event_locked(false, pitch, velocity, channel, time);
        }
        check(snd_seq_drain_output(seq_), "snd_seq_drain_output");
    }

    void AlsaMidiOutput::all_notes_off(int channel) {
        std::lock_guard lock(mutex_);
        write_cc_direct_locked(channel, 123, 0);
        check(snd_seq_drain_output(seq_), "snd_seq_drain_output");
    }

    void AlsaMidiOutput::send_realtime(std::uint8_t status, double deadline) {
        std::lock_guard lock(mutex_);

        snd_seq_event_t event;
        const snd_seq_real_time_t time = scheduled_time_locked(deadline);
        prepare_event_locked(event, time);

        switch (status) {
            case kMidiTimingClock:
                event.type = SND_SEQ_EVENT_CLOCK;
                break;
            case kMidiStart:
                event.type = SND_SEQ_EVENT_START;
                break;
            case kMidiContinue:
                event.type = SND_SEQ_EVENT_CONTINUE;
                break;
            case kMidiStop:
                event.type = SND_SEQ_EVENT_STOP;
                break;
            default:
                throw std::invalid_argument("Unsupported MIDI real-time status");
        }

        check(snd_seq_event_output(seq_, &event), "snd_seq_event_output");
        check(snd_seq_drain_output(seq_), "snd_seq_drain_output");
    }

    void AlsaMidiOutput::clear_scheduled() {
        std::lock_guard lock(mutex_);
        if (seq_ == nullptr || queue_ < 0) {
            return;
        }

        snd_seq_remove_events_t *remove = nullptr;
        snd_seq_remove_events_alloca(&remove);
        snd_seq_remove_events_set_queue(remove, queue_);
        snd_seq_remove_events_set_condition(remove, SND_SEQ_REMOVE_OUTPUT);

        check(snd_seq_remove_events(seq_, remove), "snd_seq_remove_events");
        check(snd_seq_drain_output(seq_), "snd_seq_drain_output");
    }

    int AlsaMidiOutput::client_id() const noexcept {
        return seq_ != nullptr ? snd_seq_client_id(seq_) : -1;
    }

    std::optional<AlsaPortAddress> AlsaMidiOutput::connected_destination() const {
        std::lock_guard lock(mutex_);
        return destination_;
    }

    std::vector<AlsaPortInfo> AlsaMidiOutput::list_destinations() {
        return list_ports(SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
    }

    std::optional<AlsaPortAddress> AlsaMidiOutput::parse_address(std::string_view text) {
        const std::size_t colon = text.find(':');
        if (colon == std::string_view::npos || text.find(':', colon + 1) != std::string_view::npos) {
            return std::nullopt;
        }

        AlsaPortAddress address;
        const auto first = std::from_chars(text.data(), text.data() + colon, address.client);
        const auto second = std::from_chars(text.data() + colon + 1, text.data() + text.size(), address.port);

        const bool valid = first.ec == std::errc{} && second.ec == std::errc{} && first.ptr == text.data() + colon &&
                           second.ptr == text.data() + text.size() && address.client >= 0 && address.port >= 0;
        return valid ? std::optional<AlsaPortAddress>{address} : std::nullopt;
    }

    void AlsaMidiOutput::check(int result, const char *operation) const {
        if (result < 0) {
            throw std::runtime_error(std::string(operation) + ": " + snd_strerror(result));
        }
    }

    void AlsaMidiOutput::create_queue_locked() {
        queue_ = snd_seq_alloc_named_queue(seq_, "Spatial MIDI transport");
        check(queue_, "snd_seq_alloc_named_queue");
        check(snd_seq_start_queue(seq_, queue_, nullptr), "snd_seq_start_queue");
        check(snd_seq_drain_output(seq_), "snd_seq_drain_output");
        calibrate_queue_clock_locked();
    }

    void AlsaMidiOutput::destroy_queue_locked() noexcept {
        if (queue_ < 0) {
            return;
        }

        snd_seq_stop_queue(seq_, queue_, nullptr);
        snd_seq_drain_output(seq_);
        snd_seq_free_queue(seq_, queue_);
        queue_ = -1;
    }

    void AlsaMidiOutput::calibrate_queue_clock_locked() {
        snd_seq_queue_status_t *status = nullptr;
        snd_seq_queue_status_alloca(&status);

        // The ALSA queue and std::chrono::steady_clock use different epochs. Take
        // the midpoint around the status call to keep the mapping error bounded by
        // roughly half of the system-call latency.
        const double before = monotonic_seconds();
        check(snd_seq_get_queue_status(seq_, queue_, status), "snd_seq_get_queue_status");
        const double after = monotonic_seconds();

        const snd_seq_real_time_t *queue_time = snd_seq_queue_status_get_real_time(status);
        queue_epoch_ns_ = realtime_to_ns(*queue_time);
        monotonic_epoch_ = (before + after) * 0.5;
    }

    snd_seq_real_time_t AlsaMidiOutput::scheduled_time_locked(double deadline) const {
        const long double delta_seconds = std::max<long double>(
            0.0L, static_cast<long double>(deadline) - monotonic_epoch_);
        const long double available = static_cast<long double>(
            std::numeric_limits<std::uint64_t>::max() - queue_epoch_ns_);
        const auto delta_ns = static_cast<std::uint64_t>(std::min(delta_seconds * kNanosecondsPerSecond, available));

        return ns_to_realtime(queue_epoch_ns_ + delta_ns);
    }

    void AlsaMidiOutput::prepare_event_locked(snd_seq_event_t &event, const snd_seq_real_time_t &scheduled_time) {
        snd_seq_ev_clear(&event);
        snd_seq_ev_set_source(&event, checked_alsa_byte(port_, "ALSA port id"));
        snd_seq_ev_set_subs(&event);

        snd_seq_real_time_t mutable_time = scheduled_time;
        snd_seq_ev_schedule_real(&event, checked_alsa_byte(queue_, "ALSA queue id"), 0, &mutable_time);
    }

    void AlsaMidiOutput::write_channel_event_locked(bool note_on, int pitch, int velocity, int channel,
                                                    const snd_seq_real_time_t &time) {
        snd_seq_event_t event;
        prepare_event_locked(event, time);

        if (note_on) {
            snd_seq_ev_set_noteon(&event, checked_midi_channel(channel), clamped_midi_byte(pitch),
                                  clamped_midi_byte(velocity));
        } else {
            snd_seq_ev_set_noteoff(&event, checked_midi_channel(channel), clamped_midi_byte(pitch),
                                   clamped_midi_byte(velocity));
        }

        check(snd_seq_event_output(seq_, &event), "snd_seq_event_output");
    }

    void AlsaMidiOutput::write_cc_direct_locked(int channel, int controller, int value) {
        snd_seq_event_t event;
        snd_seq_ev_clear(&event);
        snd_seq_ev_set_source(&event, checked_alsa_byte(port_, "ALSA port id"));
        snd_seq_ev_set_subs(&event);
        snd_seq_ev_set_direct(&event);
        snd_seq_ev_set_controller(&event, checked_midi_channel(channel), controller, value);

        check(snd_seq_event_output_direct(seq_, &event), "snd_seq_event_output_direct");
    }

    AlsaMidiClockInput::AlsaMidiClockInput(AlsaPortAddress source, const std::string &client_name)
        : source_(source) {
        // Queue control requires output access even though this class only exposes
        // received MIDI messages to the rest of the application.
        check(snd_seq_open(&seq_, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK), "snd_seq_open");

        try {
            check(snd_seq_set_client_name(seq_, client_name.c_str()), "snd_seq_set_client_name");

            std::lock_guard lock(mutex_);
            create_timestamp_queue_locked();
            create_input_port_locked();

            source_description_ = "MIDI clock input: " + std::to_string(source.client) + ':' + std::to_string(
                                      source.port);
        } catch (...) {
            if (seq_ != nullptr) {
                destroy_timestamp_queue_locked();
                snd_seq_close(seq_);
                seq_ = nullptr;
            }
            throw;
        }
    }

    AlsaMidiClockInput::~AlsaMidiClockInput() {
        std::lock_guard lock(mutex_);
        if (seq_ == nullptr) {
            return;
        }

        snd_seq_port_subscribe_t *subscription = nullptr;
        snd_seq_port_subscribe_alloca(&subscription);

        const snd_seq_addr_t sender{
            static_cast<unsigned char>(source_.client), static_cast<unsigned char>(source_.port),
        };
        const snd_seq_addr_t destination{
            static_cast<unsigned char>(snd_seq_client_id(seq_)), static_cast<unsigned char>(port_),
        };
        snd_seq_port_subscribe_set_sender(subscription, &sender);
        snd_seq_port_subscribe_set_dest(subscription, &destination);
        snd_seq_unsubscribe_port(seq_, subscription);

        destroy_timestamp_queue_locked();
        snd_seq_close(seq_);
        seq_ = nullptr;
    }

    std::string AlsaMidiClockInput::description() const {
        std::lock_guard lock(mutex_);
        return source_description_;
    }

    std::vector<MidiRealtimeMessage> AlsaMidiClockInput::poll_realtime(std::size_t limit) {
        std::lock_guard lock(mutex_);

        std::vector<MidiRealtimeMessage> result;
        result.reserve(std::min<std::size_t>(limit, 128));

        while (result.size() < limit && snd_seq_event_input_pending(seq_, 1) > 0) {
            snd_seq_event_t *event = nullptr;
            const int read = snd_seq_event_input(seq_, &event);
            if (read == -EAGAIN) {
                break;
            }
            check(read, "snd_seq_event_input");

            if (event == nullptr) {
                continue;
            }

            const auto status = realtime_status(event->type);
            if (!status) {
                continue;
            }

            const double polled_at = monotonic_seconds();
            result.push_back(MidiRealtimeMessage{*status, event_timestamp(*event, polled_at),});
        }

        return result;
    }

    std::vector<AlsaPortInfo> AlsaMidiClockInput::list_sources() {
        return list_ports(SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ);
    }

    std::optional<std::uint8_t> AlsaMidiClockInput::realtime_status(int event_type) noexcept {
        switch (event_type) {
            case SND_SEQ_EVENT_CLOCK:
                return kMidiTimingClock;
            case SND_SEQ_EVENT_START:
                return kMidiStart;
            case SND_SEQ_EVENT_CONTINUE:
                return kMidiContinue;
            case SND_SEQ_EVENT_STOP:
                return kMidiStop;
            default:
                return std::nullopt;
        }
    }

    void AlsaMidiClockInput::check(int result, const char *operation) {
        if (result < 0) {
            throw std::runtime_error(std::string(operation) + ": " + snd_strerror(result));
        }
    }

    void AlsaMidiClockInput::create_timestamp_queue_locked() {
        queue_ = snd_seq_alloc_named_queue(seq_, "Spatial MIDI Clock input timestamps");
        check(queue_, "snd_seq_alloc_named_queue");
        check(snd_seq_start_queue(seq_, queue_, nullptr), "snd_seq_start_queue");
        check(snd_seq_drain_output(seq_), "snd_seq_drain_output");
        calibrate_queue_clock_locked();
    }

    void AlsaMidiClockInput::destroy_timestamp_queue_locked() noexcept {
        if (seq_ == nullptr || queue_ < 0) {
            return;
        }

        snd_seq_stop_queue(seq_, queue_, nullptr);
        snd_seq_drain_output(seq_);
        snd_seq_free_queue(seq_, queue_);
        queue_ = -1;
    }

    void AlsaMidiClockInput::create_input_port_locked() {
        port_ = snd_seq_create_simple_port(seq_, "Clock In", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                           SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        check(port_, "snd_seq_create_simple_port");

        snd_seq_port_subscribe_t *subscription = nullptr;
        snd_seq_port_subscribe_alloca(&subscription);

        const snd_seq_addr_t sender{
            static_cast<unsigned char>(source_.client), static_cast<unsigned char>(source_.port),
        };
        const snd_seq_addr_t destination{
            static_cast<unsigned char>(snd_seq_client_id(seq_)), static_cast<unsigned char>(port_),
        };

        snd_seq_port_subscribe_set_sender(subscription, &sender);
        snd_seq_port_subscribe_set_dest(subscription, &destination);

        // Ask ALSA to replace each incoming event's timestamp with the current
        // queue time, preserving spacing when several Clock messages are read in one poll.
        snd_seq_port_subscribe_set_queue(subscription, queue_);
        snd_seq_port_subscribe_set_time_update(subscription, 1);
        snd_seq_port_subscribe_set_time_real(subscription, 1);

        check(snd_seq_subscribe_port(seq_, subscription), "snd_seq_subscribe_port");
    }

    void AlsaMidiClockInput::calibrate_queue_clock_locked() {
        snd_seq_queue_status_t *status = nullptr;
        snd_seq_queue_status_alloca(&status);

        const double before = monotonic_seconds();
        check(snd_seq_get_queue_status(seq_, queue_, status), "snd_seq_get_queue_status");
        const double after = monotonic_seconds();

        const snd_seq_real_time_t *queue_time = snd_seq_queue_status_get_real_time(status);
        queue_epoch_ns_ = realtime_to_ns(*queue_time);
        monotonic_epoch_ = (before + after) * 0.5;
    }

    double AlsaMidiClockInput::event_timestamp(const snd_seq_event_t &event, double fallback) const noexcept {
        const bool is_realtime = (event.flags & SND_SEQ_TIME_STAMP_MASK) == SND_SEQ_TIME_STAMP_REAL;
        const bool is_absolute = (event.flags & SND_SEQ_TIME_MODE_MASK) == SND_SEQ_TIME_MODE_ABS;
        if (!is_realtime || !is_absolute) {
            return fallback;
        }

        const long double event_ns = static_cast<long double>(realtime_to_ns(event.time.time));
        const long double epoch_ns = static_cast<long double>(queue_epoch_ns_);
        const double mapped = monotonic_epoch_ + static_cast<double>(event_ns - epoch_ns) / static_cast<double>(kNanosecondsPerSecond);

        return std::isfinite(mapped) ? mapped : fallback;
    }


    AlsaMidiNoteInput::AlsaMidiNoteInput(AlsaPortAddress source, const std::string &client_name)
        : source_(source) {
        check(snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK), "snd_seq_open");

        try {
            check(snd_seq_set_client_name(seq_, client_name.c_str()), "snd_seq_set_client_name");
            create_input_port();
            source_description_ = "MIDI note input: " + std::to_string(source.client) + ':' +
                                  std::to_string(source.port);
        } catch (...) {
            snd_seq_close(seq_);
            seq_ = nullptr;
            throw;
        }
    }

    AlsaMidiNoteInput::~AlsaMidiNoteInput() {
        if (seq_ == nullptr) {
            return;
        }

        snd_seq_port_subscribe_t *subscription = nullptr;
        snd_seq_port_subscribe_alloca(&subscription);
        const snd_seq_addr_t sender{
            static_cast<unsigned char>(source_.client), static_cast<unsigned char>(source_.port),
        };
        const snd_seq_addr_t destination{
            static_cast<unsigned char>(snd_seq_client_id(seq_)), static_cast<unsigned char>(port_),
        };
        snd_seq_port_subscribe_set_sender(subscription, &sender);
        snd_seq_port_subscribe_set_dest(subscription, &destination);
        snd_seq_unsubscribe_port(seq_, subscription);

        snd_seq_close(seq_);
        seq_ = nullptr;
    }

    std::string AlsaMidiNoteInput::description() const {
        return source_description_;
    }

    std::optional<MidiNoteMessage> AlsaMidiNoteInput::read_note(std::chrono::milliseconds timeout) {
        const int descriptor_count_result = snd_seq_poll_descriptors_count(seq_, POLLIN);
        check(descriptor_count_result, "snd_seq_poll_descriptors_count");
        if (descriptor_count_result == 0) {
            return std::nullopt;
        }

        const unsigned int descriptor_count = static_cast<unsigned int>(descriptor_count_result);
        std::vector<pollfd> descriptors(descriptor_count);
        check(snd_seq_poll_descriptors(seq_, descriptors.data(), descriptor_count, POLLIN),
              "snd_seq_poll_descriptors");

        const auto bounded_timeout = std::clamp<long long>(timeout.count(), 0, std::numeric_limits<int>::max());
        const int ready = ::poll(descriptors.data(), descriptors.size(), static_cast<int>(bounded_timeout));
        if (ready == 0) {
            return std::nullopt;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                return std::nullopt;
            }
            throw std::system_error(errno, std::generic_category(), "poll MIDI note input");
        }

        for (;;) {
            const int pending = snd_seq_event_input_pending(seq_, 1);
            check(pending, "snd_seq_event_input_pending");
            if (pending == 0) {
                return std::nullopt;
            }

            snd_seq_event_t *event = nullptr;
            const int result = snd_seq_event_input(seq_, &event);
            if (result == -EAGAIN) {
                return std::nullopt;
            }
            check(result, "snd_seq_event_input");
            if (event == nullptr || event->type != SND_SEQ_EVENT_NOTEON) {
                continue;
            }

            return MidiNoteMessage{
                .pitch = event->data.note.note,
                .velocity = event->data.note.velocity,
                .channel = event->data.note.channel,
            };
        }

        return std::nullopt;
    }

    std::vector<AlsaPortInfo> AlsaMidiNoteInput::list_sources() {
        return list_ports(SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ);
    }

    void AlsaMidiNoteInput::check(int result, const char *operation) {
        if (result < 0) {
            throw std::runtime_error(std::string(operation) + ": " + snd_strerror(result));
        }
    }

    void AlsaMidiNoteInput::create_input_port() {
        port_ = snd_seq_create_simple_port(
            seq_, "Note In", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        check(port_, "snd_seq_create_simple_port");

        snd_seq_port_subscribe_t *subscription = nullptr;
        snd_seq_port_subscribe_alloca(&subscription);
        const snd_seq_addr_t sender{
            static_cast<unsigned char>(source_.client), static_cast<unsigned char>(source_.port),
        };
        const snd_seq_addr_t destination{
            static_cast<unsigned char>(snd_seq_client_id(seq_)), static_cast<unsigned char>(port_),
        };
        snd_seq_port_subscribe_set_sender(subscription, &sender);
        snd_seq_port_subscribe_set_dest(subscription, &destination);
        check(snd_seq_subscribe_port(seq_, subscription), "snd_seq_subscribe_port");
    }
}
