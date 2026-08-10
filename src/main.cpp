#include "spatial_midi/core/graph.hpp"
#include "spatial_midi/core/midi_io.hpp"
#include "spatial_midi/core/transport_worker.hpp"
#include "spatial_midi/midi/alsa_sequencer.hpp"
#include "spatial_midi/ui/sdl_app.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    using spatial_midi::AlsaMidiClockInput;
    using spatial_midi::AlsaMidiNoteInput;
    using spatial_midi::AlsaMidiOutput;
    using spatial_midi::AlsaPortAddress;
    using spatial_midi::ClockInputOpenResult;
    using spatial_midi::NoteInputOpenResult;
    using spatial_midi::NullMidiOutput;
    using spatial_midi::OutputOpenResult;

    struct Options {
        bool help = false;
        bool list_midi = false;
        std::optional<AlsaPortAddress> output;
        std::optional<AlsaPortAddress> input;
        std::optional<AlsaPortAddress> note_input;
        int midi_channel = 1;
        int note_input_channel = 1;
        int default_velocity = spatial_midi::kDefaultVelocity;
        std::filesystem::path project_file = "graph.json";
        std::optional<std::filesystem::path> font;
    };

    void print_usage(const char *executable) {
        std::cout << "Usage: " << executable << " [options]\n\n" << "Options:\n" <<
                "  --list-midi                         " "List ALSA MIDI output and input ports\n" <<
                "  --midi-device CLIENT:PORT          " "Select an ALSA MIDI output destination\n" <<
                "  --midi-input-device CLIENT:PORT    " "Select a MIDI Clock input source\n" <<
                "  --midi-note-input-device CLIENT:PORT " "Select a MIDI note-entry input source\n" <<
                "  --midi-channel 1..16               " "Output channel (default 1)\n" <<
                "  --midi-note-input-channel 1..16    " "Note-entry input channel (default 1)\n" <<
                "  --default-velocity 0..127          " "Velocity for newly created nodes (default " <<
                spatial_midi::kDefaultVelocity << ")\n" <<
                "  --project-file PATH                " "F1/F2 project path (default graph.json)\n" <<
                "  --font PATH                        " "TrueType font for SDL2_ttf\n" <<
                "  -h, --help                         Show this help\n";
    }

    AlsaPortAddress parse_address(const std::string &value, const std::string &option) {
        const auto parsed = AlsaMidiOutput::parse_address(value);
        if (!parsed) {
            throw std::runtime_error(option + " requires CLIENT:PORT, got: " + value);
        }
        return *parsed;
    }

    Options parse_options(int argc, char **argv) {
        Options options;

        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            const auto require_value = [&](const std::string &option) -> std::string {
                if (index + 1 >= argc) {
                    throw std::runtime_error(option + " requires a value");
                }
                return argv[++index];
            };

            if (argument == "--help" || argument == "-h") {
                options.help = true;
            } else if (argument == "--list-midi") {
                options.list_midi = true;
            } else if (argument == "--midi-device") {
                options.output = parse_address(require_value(argument), argument);
            } else if (argument == "--midi-input-device") {
                options.input = parse_address(require_value(argument), argument);
            } else if (argument == "--midi-note-input-device") {
                options.note_input = parse_address(require_value(argument), argument);
            } else if (argument == "--midi-channel") {
                const std::string value = require_value(argument);
                try {
                    options.midi_channel = std::stoi(value);
                } catch (...) {
                    throw std::runtime_error("--midi-channel must be an integer from 1 through 16");
                }

                if (options.midi_channel < 1 || options.midi_channel > 16) {
                    throw std::runtime_error("--midi-channel must be from 1 through 16");
                }
            } else if (argument == "--midi-note-input-channel") {
                const std::string value = require_value(argument);
                try {
                    options.note_input_channel = std::stoi(value);
                } catch (...) {
                    throw std::runtime_error("--midi-note-input-channel must be an integer from 1 through 16");
                }

                if (options.note_input_channel < 1 || options.note_input_channel > 16) {
                    throw std::runtime_error("--midi-note-input-channel must be from 1 through 16");
                }
            } else if (argument == "--default-velocity") {
                const std::string value = require_value(argument);
                try {
                    options.default_velocity = std::stoi(value);
                } catch (...) {
                    throw std::runtime_error("--default-velocity must be an integer from 0 through 127");
                }

                if (options.default_velocity < 0 || options.default_velocity > 127) {
                    throw std::runtime_error("--default-velocity must be from 0 through 127");
                }
            } else if (argument == "--project-file") {
                options.project_file = require_value(argument);
            } else if (argument == "--font") {
                options.font = std::filesystem::path(require_value(argument));
            } else {
                throw std::runtime_error("Unknown option: " + argument);
            }
        }

        return options;
    }

    void print_ports(std::string_view heading, const std::vector<spatial_midi::AlsaPortInfo> &ports) {
        std::cout << heading << ":\n";
        if (ports.empty()) {
            std::cout << "  (none)\n";
        }

        for (const auto &port: ports) {
            std::cout << "  " << port.address.client << ':' << port.address.port << "  " << port.client_name << " / " <<
                    port.port_name << '\n';
        }
    }

    OutputOpenResult open_output(const std::optional<AlsaPortAddress> &selected) {
        try {
            auto output = std::make_shared<AlsaMidiOutput>();
            std::optional<AlsaPortAddress> destination = selected;

            // An explicitly selected port wins; otherwise use the first writable ALSA destination.
            if (!destination) {
                const auto ports = AlsaMidiOutput::list_destinations();
                if (!ports.empty()) {
                    destination = ports.front().address;
                }
            }

            if (destination) {
                output->connect_to(*destination);
            }

            const bool connected = destination.has_value();
            std::string status = output->description();
            if (!connected) {
                status += " (virtual port; connect with aconnect)";
            }
            return {output, std::move(status), connected};
        } catch (const std::exception &error) {
            return {std::make_shared<NullMidiOutput>(), "No MIDI output (" + std::string(error.what()) + ')', false,};
        }
    }

    ClockInputOpenResult open_clock_input(const std::optional<AlsaPortAddress> &selected) {
        try {
            std::optional<AlsaPortAddress> source = selected;
            if (!source) {
                const auto ports = AlsaMidiClockInput::list_sources();
                // Avoid selecting our own ports or ALSA's System client as the
                // implicit Clock source.
                const auto it = std::ranges::find_if(ports, [](const auto &port) {
                    return port.client_name.find("Spatial MIDI") == std::string::npos &&
                           port.client_name != "System";
                });
                if (it != ports.end()) {
                    source = it->address;
                }
            }

            if (!source) {
                return {nullptr, "No MIDI Clock input source found"};
            }

            auto input = std::make_shared<AlsaMidiClockInput>(*source);
            return {input, input->description()};
        } catch (const std::exception &error) {
            return {nullptr, "MIDI Clock input unavailable (" + std::string(error.what()) + ')',};
        }
    }

    NoteInputOpenResult open_note_input(const std::optional<AlsaPortAddress> &source) {
        if (!source) {
            return {nullptr, "no readable source selected"};
        }

        try {
            auto input = std::make_shared<AlsaMidiNoteInput>(*source);
            return {input, input->description()};
        } catch (const std::exception &error) {
            return {nullptr, std::string(error.what())};
        }
    }
}

int main(int argc, char **argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            print_usage(argv[0]);
            return 0;
        }
        if (options.list_midi) {
            print_ports("MIDI output destinations", AlsaMidiOutput::list_destinations());
            print_ports("MIDI input sources", AlsaMidiNoteInput::list_sources());
            return 0;
        }

        // Keep port opening in reusable closures so the UI can reconnect after
        // a device disappears without duplicating command-line policy.
        const auto output_opener = [selected = options.output] {
            return open_output(selected);
        };
        const auto clock_input_opener = [selected = options.input] {
            return open_clock_input(selected);
        };

        OutputOpenResult opened = output_opener();
        std::cout << opened.status << '\n';

        std::optional<AlsaPortAddress> note_input_source = options.note_input;
        if (!note_input_source) {
            if (const auto alsa_output = std::dynamic_pointer_cast<AlsaMidiOutput>(opened.backend)) {
                note_input_source = alsa_output->connected_destination();
            }
        }
        const auto note_input_opener = [source = note_input_source] {
            return open_note_input(source);
        };

        spatial_midi::TransportWorker worker(
            spatial_midi::create_default_graph(options.default_velocity),
            opened.backend,
            spatial_midi::kDefaultTempo,
            options.midi_channel - 1);
        spatial_midi::SdlApp app(worker, opened.backend, opened.status, output_opener, clock_input_opener,
                                 note_input_opener, options.midi_channel, options.note_input_channel,
                                 options.default_velocity, options.project_file, options.font);
        return app.run();
    } catch (const std::exception &error) {
        std::cerr << "spatial-midi: " << error.what() << '\n';
        return 1;
    }
}
