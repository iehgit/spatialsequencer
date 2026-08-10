#include "spatial_midi/core/graph.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <system_error>
#include <variant>

namespace spatial_midi {
    namespace {
        constexpr std::string_view kProjectFormat = "spatial-midi-project";
        constexpr int kProjectVersion = 1;

        struct JsonValue {
            using Array = std::vector<JsonValue>;
            using Object = std::map<std::string, JsonValue, std::less<> >;
            std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;

            [[nodiscard]] const Object &object(std::string_view context) const {
                if (const auto *result = std::get_if<Object>(&value)) {
                    return *result;
                }
                throw GraphError(std::string(context) + " must be a JSON object");
            }

            [[nodiscard]] const Array &array(std::string_view context) const {
                if (const auto *result = std::get_if<Array>(&value)) {
                    return *result;
                }
                throw GraphError(std::string(context) + " must be a JSON array");
            }

            [[nodiscard]] std::string string(std::string_view context) const {
                if (const auto *result = std::get_if<std::string>(&value)) {
                    return *result;
                }
                throw GraphError(std::string(context) + " must be a string");
            }

            [[nodiscard]] double number(std::string_view context) const {
                if (const auto *result = std::get_if<double>(&value)) {
                    return *result;
                }
                throw GraphError(std::string(context) + " must be a number");
            }

            [[nodiscard]] int integer(std::string_view context) const {
                if (const auto *result = std::get_if<double>(&value)) {
                    const int as_int = static_cast<int>(*result);
                    if (static_cast<double>(as_int) == *result) return as_int;
                }
                throw GraphError(std::string(context) + " must be an integer");
            }

            [[nodiscard]] bool boolean(std::string_view context) const {
                if (const auto *result = std::get_if<bool>(&value)) {
                    return *result;
                }
                throw GraphError(std::string(context) + " must be a boolean");
            }

            [[nodiscard]] bool is_null() const noexcept {
                return std::holds_alternative<std::nullptr_t>(value);
            }
        };

        class JsonParser {
        public:
            explicit JsonParser(std::string_view input) : input_(input) {
            }

            JsonValue parse() {
                skip_space();
                JsonValue result = parse_value();
                skip_space();
                if (position_ != input_.size()) fail("Unexpected trailing JSON data");
                return result;
            }

        private:
            [[noreturn]] static void fail(std::string_view message) {
                throw GraphError(std::string("Invalid JSON: ") + std::string(message));
            }

            void skip_space() {
                while (position_ < input_.size() &&
                       std::isspace(static_cast<unsigned char>(input_[position_]))) {
                    ++position_;
                }
            }

            bool consume(char expected) {
                skip_space();
                if (position_ < input_.size() && input_[position_] == expected) {
                    ++position_;
                    return true;
                }
                return false;
            }

            JsonValue parse_value() {
                skip_space();
                if (position_ >= input_.size()) fail("Unexpected end of input");
                switch (input_[position_]) {
                    case '{':
                        return JsonValue{parse_object()};
                    case '[':
                        return JsonValue{parse_array()};
                    case '"':
                        return JsonValue{parse_string()};
                    case 't':
                        parse_literal("true");
                        return JsonValue{true};
                    case 'f':
                        parse_literal("false");
                        return JsonValue{false};
                    case 'n':
                        parse_literal("null");
                        return JsonValue{nullptr};
                    default:
                        if (input_[position_] == '-' ||
                            std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                            return JsonValue{parse_number()};
                        }
                        fail("Unexpected token");
                }
            }

            JsonValue::Object parse_object() {
                if (!consume('{')) fail("Expected object");
                JsonValue::Object object;
                if (consume('}')) return object;
                for (;;) {
                    skip_space();
                    if (position_ >= input_.size() || input_[position_] != '"') {
                        fail("Expected object key");
                    }
                    std::string key = parse_string();
                    if (!consume(':')) fail("Expected ':'");
                    object.insert_or_assign(std::move(key), parse_value());
                    if (consume('}')) break;
                    if (!consume(',')) fail("Expected ','");
                }
                return object;
            }

            JsonValue::Array parse_array() {
                if (!consume('[')) fail("Expected array");
                JsonValue::Array array;
                if (consume(']')) return array;
                for (;;) {
                    array.push_back(parse_value());
                    if (consume(']')) break;
                    if (!consume(',')) fail("Expected ','");
                }
                return array;
            }

            std::string parse_string() {
                if (!consume('"')) fail("Expected string");
                std::string result;
                while (position_ < input_.size()) {
                    const char ch = input_[position_++];
                    if (ch == '"') return result;
                    if (ch == '\\') {
                        if (position_ >= input_.size()) fail("Bad string escape");
                        const char escaped = input_[position_++];
                        switch (escaped) {
                            case '"':
                                result.push_back('"');
                                break;
                            case '\\':
                                result.push_back('\\');
                                break;
                            case '/':
                                result.push_back('/');
                                break;
                            case 'b':
                                result.push_back('\b');
                                break;
                            case 'f':
                                result.push_back('\f');
                                break;
                            case 'n':
                                result.push_back('\n');
                                break;
                            case 'r':
                                result.push_back('\r');
                                break;
                            case 't':
                                result.push_back('\t');
                                break;
                            default:
                                fail("Unsupported string escape");
                        }
                    } else {
                        result.push_back(ch);
                    }
                }
                fail("Unterminated string");
            }

            double parse_number() {
                const std::size_t begin = position_;
                if (input_[position_] == '-') ++position_;
                while (position_ < input_.size() &&
                       std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                    ++position_;
                }
                if (position_ < input_.size() && input_[position_] == '.') {
                    ++position_;
                    while (position_ < input_.size() &&
                           std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                        ++position_;
                    }
                }
                double value = 0.0;
                const auto token = input_.substr(begin, position_ - begin);
                std::string copy(token);
                try {
                    value = std::stod(copy);
                } catch (...) {
                    fail("Invalid number");
                }
                return value;
            }

            void parse_literal(std::string_view literal) {
                if (input_.substr(position_, literal.size()) != literal) fail("Invalid literal");
                position_ += literal.size();
            }

            std::string_view input_;
            std::size_t position_ = 0;
        };

        const JsonValue &required(const JsonValue::Object &object, std::string_view key) {
            const auto it = object.find(key);
            if (it == object.end()) {
                throw GraphError("Invalid project file: missing " + std::string(key));
            }
            return it->second;
        }

        const JsonValue *optional(const JsonValue::Object &object, std::string_view key) {
            const auto it = object.find(key);
            return it == object.end() ? nullptr : &it->second;
        }

        std::string escape_json(std::string_view text) {
            std::string result;
            for (const char ch: text) {
                switch (ch) {
                    case '"':
                        result += "\\\"";
                        break;
                    case '\\':
                        result += "\\\\";
                        break;
                    case '\n':
                        result += "\\n";
                        break;
                    case '\r':
                        result += "\\r";
                        break;
                    case '\t':
                        result += "\\t";
                        break;
                    default:
                        result.push_back(ch);
                        break;
                }
            }
            return result;
        }
    }

    Node *Graph::find_node(int node_id) noexcept {
        const auto it = std::ranges::find(nodes_, node_id, &Node::id);
        return it == nodes_.end() ? nullptr : &*it;
    }

    const Node *Graph::find_node(int node_id) const noexcept {
        const auto it = std::ranges::find(nodes_, node_id, &Node::id);
        return it == nodes_.end() ? nullptr : &*it;
    }

    Node *Graph::node_at(int x, int y) noexcept {
        const auto it = std::ranges::find_if(
            nodes_, [x, y](const Node &node) { return node.x == x && node.y == y; });
        return it == nodes_.end() ? nullptr : &*it;
    }

    const Node *Graph::node_at(int x, int y) const noexcept {
        const auto it = std::ranges::find_if(
            nodes_, [x, y](const Node &node) { return node.x == x && node.y == y; });
        return it == nodes_.end() ? nullptr : &*it;
    }

    void Graph::validate_node(const Node &node) {
        if (node.id < 1) {
            throw GraphError("Invalid node ID");
        }

        if (is_relay(node)) {
            if (!node.pitches.empty()) {
                throw GraphError("Relay nodes cannot contain pitches");
            }
            if (node.velocity != kDefaultVelocity || node.silenced) {
                throw GraphError("Relay nodes cannot contain musical state");
            }
            return;
        }

        if (node.pitches.size() < kMinPitchSlots ||
            node.pitches.size() > kMaxPitchSlots) {
            throw GraphError("A musical node must contain between 1 and 6 pitches");
        }
        if (std::ranges::any_of(
            node.pitches, [](int pitch) { return pitch < 0 || pitch > 127; })) {
            throw GraphError("MIDI pitches must be between 0 and 127");
        }
        if (node.velocity < 0 || node.velocity > 127) {
            throw GraphError("MIDI velocity must be between 0 and 127");
        }
    }

    Node &Graph::add_node(
        int x,
        int y,
        int pitch,
        int velocity,
        bool silenced,
        std::vector<int> pitches,
        RoutingMode routing_mode) {
        if (node_at(x, y) != nullptr) {
            throw GraphError("That grid cell is already occupied");
        }
        if (pitches.empty()) {
            pitches.push_back(pitch);
        }

        Node node{
            next_node_id_,
            x,
            y,
            std::move(pitches),
            velocity,
            silenced,
            routing_mode,
        };
        validate_node(node);
        nodes_.push_back(std::move(node));
        ++next_node_id_;
        return nodes_.back();
    }

    Node &Graph::add_relay_node(int x, int y, RoutingMode routing_mode) {
        if (node_at(x, y) != nullptr) {
            throw GraphError("That grid cell is already occupied");
        }

        Node node{
            next_node_id_,
            x,
            y,
            {},
            kDefaultVelocity,
            false,
            routing_mode,
            NodeType::Relay,
        };
        validate_node(node);
        nodes_.push_back(std::move(node));
        ++next_node_id_;
        return nodes_.back();
    }

    void Graph::insert_node(Node node) {
        validate_node(node);
        if (find_node(node.id) != nullptr) {
            throw GraphError(
                "Invalid or duplicate node ID: " + std::to_string(node.id));
        }
        if (node_at(node.x, node.y) != nullptr) {
            throw GraphError("Project file has multiple nodes in one grid cell");
        }

        next_node_id_ = std::max(next_node_id_, node.id + 1);
        nodes_.push_back(std::move(node));
    }

    void Graph::delete_node(int node_id) {
        require_node(node_id);
        std::erase_if(
            nodes_,
            [node_id](const Node &node) { return node.id == node_id; });
        std::erase_if(edges_, [node_id](const Edge &edge) {
            return edge.source_id == node_id || edge.target_id == node_id;
        });

        if (start_node_id_ == node_id) {
            start_node_id_.reset();
        }
    }

    void Graph::move_node(int node_id, int x, int y) {
        Node &node = require_node(node_id);
        if (const Node *occupant = node_at(x, y);
            occupant != nullptr && occupant->id != node_id) {
            throw GraphError("That grid cell is already occupied");
        }

        node.x = x;
        node.y = y;
    }

    int Graph::set_pitch(int node_id, int pitch) {
        Node &node = require_musical_node(node_id);
        node.pitches.front() = clamp_midi(pitch);
        return node.pitches.front();
    }

    int Graph::transpose(int node_id, int semitones) {
        return set_pitch(
            node_id,
            require_musical_node(node_id).pitches.front() + semitones);
    }

    int Graph::set_last_pitch(int node_id, int pitch) {
        Node &node = require_musical_node(node_id);
        node.pitches.back() = clamp_midi(pitch);
        return node.pitches.back();
    }

    int Graph::transpose_last(int node_id, int semitones) {
        return set_last_pitch(
            node_id,
            require_musical_node(node_id).pitches.back() + semitones);
    }

    int Graph::set_velocity(int node_id, int velocity) {
        Node &node = require_musical_node(node_id);
        node.velocity = clamp_midi(velocity);
        return node.velocity;
    }

    void Graph::set_primary_pitch_and_velocity(int node_id, int pitch, int velocity) {
        Node &node = require_musical_node(node_id);
        node.pitches.front() = clamp_midi(pitch);
        node.velocity = clamp_midi(velocity);
    }

    int Graph::adjust_velocity(int node_id, int delta) {
        return set_velocity(node_id, require_musical_node(node_id).velocity + delta);
    }

    int Graph::append_pitch(int node_id) {
        Node &node = require_musical_node(node_id);
        if (node.pitches.size() >= kMaxPitchSlots) {
            throw GraphError("A node cannot contain more than 6 pitches");
        }

        const int last = node.pitches.back();
        const int pitch = last <= 115 ? last + 12 : last - 12;
        node.pitches.push_back(pitch);
        return pitch;
    }

    int Graph::remove_last_pitch(int node_id) {
        Node &node = require_musical_node(node_id);
        if (node.pitches.size() <= kMinPitchSlots) {
            throw GraphError("A node must contain at least one pitch");
        }

        const int result = node.pitches.back();
        node.pitches.pop_back();
        return result;
    }

    bool Graph::set_silenced(int node_id, bool silenced) {
        Node &node = require_musical_node(node_id);
        node.silenced = silenced;
        return silenced;
    }

    bool Graph::toggle_silenced(int node_id) {
        return set_silenced(node_id, !require_musical_node(node_id).silenced);
    }

    RoutingMode Graph::set_routing_mode(int node_id, RoutingMode mode) {
        Node &node = require_node(node_id);
        node.routing_mode = mode;
        return mode;
    }

    RoutingMode Graph::toggle_routing_mode(int node_id) {
        const RoutingMode next =
                require_node(node_id).routing_mode == RoutingMode::Random
                    ? RoutingMode::Counter
                    : RoutingMode::Random;
        return set_routing_mode(node_id, next);
    }

    void Graph::set_start(int node_id) {
        require_node(node_id);
        start_node_id_ = node_id;
    }

    Edge Graph::connect(int source_id, int target_id) {
        const Node &source = require_node(source_id);
        const Node &target = require_node(target_id);

        if (source_id == target_id) {
            throw GraphError("Self-connections are not allowed");
        }
        if (is_relay(source) && is_relay(target)) {
            throw GraphError("Relay nodes cannot connect to other relay nodes");
        }
        if (source.x == target.x && source.y == target.y) {
            throw GraphError("Zero-length edges are not allowed");
        }
        if (std::ranges::find(edges_, Edge{source_id, target_id}) != edges_.end()) {
            throw GraphError("That directed edge already exists");
        }

        // Edge insertion order is part of round-robin routing behavior and is therefore
        // preserved in memory and in the JSON representation.
        edges_.push_back(Edge{source_id, target_id});
        return edges_.back();
    }

    int Graph::remove_outgoing(int source_id) {
        require_node(source_id);
        const auto old_size = edges_.size();
        std::erase_if(
            edges_,
            [source_id](const Edge &edge) {
                return edge.source_id == source_id;
            });
        return static_cast<int>(old_size - edges_.size());
    }

    Edge Graph::disconnect(int source_id, int target_id) {
        require_node(source_id);
        require_node(target_id);

        const auto it = std::ranges::find(edges_, Edge{source_id, target_id});
        if (it == edges_.end()) {
            throw GraphError("Directed edge does not exist");
        }

        const Edge result = *it;
        edges_.erase(it);
        return result;
    }

    std::vector<Edge> Graph::outgoing_edges(int source_id) const {
        require_node(source_id);
        std::vector<Edge> result;
        for (const Edge &edge: edges_) {
            if (edge.source_id == source_id) {
                result.push_back(edge);
            }
        }
        return result;
    }

    int Graph::edge_ticks(
        int source_id,
        std::optional<int> target_id) const {
        require_node(source_id);
        Edge chosen{};

        if (!target_id) {
            const auto outgoing = outgoing_edges(source_id);
            if (outgoing.empty()) {
                throw GraphError("Node has no outgoing edge");
            }
            if (outgoing.size() > 1) {
                throw GraphError(
                    "Specify a target for a node with multiple outputs");
            }
            chosen = outgoing.front();
        } else {
            require_node(*target_id);
            const auto it = std::ranges::find(edges_, Edge{source_id, *target_id});
            if (it == edges_.end()) {
                throw GraphError("Directed edge does not exist");
            }
            chosen = *it;
        }

        const Node &source = require_node(chosen.source_id);
        if (is_relay(source)) {
            return 0;
        }
        return manhattan_ticks(source, require_node(chosen.target_id));
    }

    Node &Graph::require_node(int node_id) {
        if (Node *node = find_node(node_id)) {
            return *node;
        }
        throw GraphError("Unknown node ID: " + std::to_string(node_id));
    }

    const Node &Graph::require_node(int node_id) const {
        if (const Node *node = find_node(node_id)) {
            return *node;
        }
        throw GraphError("Unknown node ID: " + std::to_string(node_id));
    }

    Node &Graph::require_musical_node(int node_id) {
        Node &node = require_node(node_id);
        if (is_relay(node)) {
            throw GraphError("Relay nodes do not have musical properties");
        }
        return node;
    }

    const Node &Graph::require_musical_node(int node_id) const {
        const Node &node = require_node(node_id);
        if (is_relay(node)) {
            throw GraphError("Relay nodes do not have musical properties");
        }
        return node;
    }

    std::string Graph::to_json(const ProjectSettings &settings) const {
        if (!std::isfinite(settings.bpm) || settings.bpm < kMinTempo || settings.bpm > kMaxTempo) {
            throw GraphError("Project BPM must be between 20 and 300");
        }
        if (settings.release_gap_eighths < kMinReleaseGapEighths ||
            settings.release_gap_eighths > kMaxReleaseGapEighths) {
            throw GraphError("Project Release Gap must be between 0/8 and 4/8");
        }

        std::ostringstream out;
        out << std::setprecision(15);
        out << "{\n  \"format\": \"" << escape_json(kProjectFormat) << "\",\n"
                << "  \"version\": " << kProjectVersion << ",\n"
                << "  \"bpm\": " << settings.bpm << ",\n"
                << "  \"release_gap_eighths\": " << settings.release_gap_eighths << ",\n"
                << "  \"start_node_id\": ";
        if (start_node_id_) out << *start_node_id_;
        else out << "null";
        out << ",\n  \"nodes\": [";
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const Node &node = nodes_[i];
            out << (i == 0 ? "\n" : ",\n")
                    << "    {\n"
                    << "      \"id\": " << node.id << ",\n"
                    << "      \"x\": " << node.x << ",\n"
                    << "      \"y\": " << node.y << ",\n";

            if (is_relay(node)) {
                out << "      \"type\": \"relay\",\n";
            } else {
                out << "      \"pitches\": [";
                for (std::size_t p = 0; p < node.pitches.size(); ++p) {
                    if (p) out << ", ";
                    out << node.pitches[p];
                }
                out << "],\n"
                        << "      \"velocity\": " << node.velocity << ",\n"
                        << "      \"silenced\": " << (node.silenced ? "true" : "false") << ",\n";
            }

            out << "      \"routing_mode\": \"" << routing_mode_name(node.routing_mode) << "\"\n"
                    << "    }";
        }
        if (!nodes_.empty()) out << '\n';
        out << "  ],\n  \"edges\": [";
        for (std::size_t i = 0; i < edges_.size(); ++i) {
            const Edge &edge = edges_[i];
            out << (i == 0 ? "\n" : ",\n")
                    << "    {\"source_id\": " << edge.source_id << ", \"target_id\": " << edge.target_id << "}";
        }
        if (!edges_.empty()) out << '\n';
        out << "  ]\n}\n";
        return out.str();
    }

    Graph Graph::from_json(std::string_view json, ProjectSettings *settings) {
        const JsonValue parsed = JsonParser(json).parse();
        const auto &root = parsed.object("Project file");
        if (required(root, "format").string("format") != kProjectFormat) throw GraphError(
            "Unrecognized project file format");
        const int version = required(root, "version").integer("version");
        if (version != kProjectVersion) throw GraphError("Unsupported project file version: " + std::to_string(version));
        ProjectSettings loaded_settings;
        if (const JsonValue *bpm = optional(root, "bpm")) {
            loaded_settings.bpm = bpm->number("Project BPM");
            if (!std::isfinite(loaded_settings.bpm) || loaded_settings.bpm < kMinTempo ||
                loaded_settings.bpm > kMaxTempo) {
                throw GraphError("Project BPM must be between 20 and 300");
            }
        }
        if (const JsonValue *gap = optional(root, "release_gap_eighths")) {
            loaded_settings.release_gap_eighths = gap->integer("Project Release Gap");
            if (loaded_settings.release_gap_eighths < kMinReleaseGapEighths ||
                loaded_settings.release_gap_eighths > kMaxReleaseGapEighths) {
                throw GraphError("Project Release Gap must be between 0/8 and 4/8");
            }
        }
        const auto &raw_nodes = required(root, "nodes").array("Project file nodes");
        const auto &raw_edges = required(root, "edges").array("Project file edges");

        Graph graph;
        for (const JsonValue &value: raw_nodes) {
            const auto &object = value.object("Every node");
            if (object.contains("pitch") || object.contains("secondary_pitch")) {
                throw GraphError("Unsupported node pitch fields; use pitches");
            }

            Node node;
            node.id = required(object, "id").integer("Node id");
            node.x = required(object, "x").integer("Node x");
            node.y = required(object, "y").integer("Node y");

            if (const JsonValue *type = optional(object, "type")) {
                const std::string name = type->string("Node type");
                if (name == "relay") {
                    node.type = NodeType::Relay;
                } else if (name != "musical") {
                    throw GraphError("Node type must be 'musical' or 'relay'");
                }
            }

            node.pitches.clear();
            if (is_relay(node)) {
                if (object.contains("pitches") || object.contains("velocity") || object.contains("silenced")) {
                    throw GraphError("Relay nodes cannot contain pitches, velocity, or silenced fields");
                }
            } else {
                const auto &pitches = required(object, "pitches").array("Node pitches");
                for (const JsonValue &pitch: pitches) {
                    node.pitches.push_back(pitch.integer("Node pitch"));
                }
                if (const JsonValue *velocity = optional(object, "velocity")) {
                    node.velocity = velocity->integer("Node velocity");
                }
                if (const JsonValue *silenced = optional(object, "silenced")) {
                    node.silenced = silenced->boolean("Node silenced");
                }
            }

            if (const JsonValue *routing = optional(object, "routing_mode")) {
                const std::string mode = routing->string("Node routing_mode");
                if (mode == "random") node.routing_mode = RoutingMode::Random;
                else if (mode == "round_robin") node.routing_mode = RoutingMode::Counter;
                else throw GraphError("Routing mode must be 'random' or 'round_robin'");
            }
            graph.insert_node(std::move(node));
        }
        for (const JsonValue &value: raw_edges) {
            const auto &object = value.object("Every edge");
            graph.connect(required(object, "source_id").integer("Edge source_id"),
                          required(object, "target_id").integer("Edge target_id"));
        }
        if (const JsonValue *start = optional(root, "start_node_id"); start != nullptr && !start->is_null()) {
            graph.set_start(start->integer("start_node_id"));
        }
        if (settings) {
            *settings = loaded_settings;
        }
        return graph;
    }

    void Graph::save_json(const std::filesystem::path &path, const ProjectSettings &settings) const {
        const std::filesystem::path temporary = path.string() + ".tmp";
        try {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::system_error(errno, std::generic_category(), "open " + temporary.string());
            output << to_json(settings);
            output.close();
            if (!output) throw std::system_error(errno, std::generic_category(), "write " + temporary.string());
            std::filesystem::rename(temporary, path);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }

    Graph Graph::load_json(const std::filesystem::path &path, ProjectSettings *settings) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::system_error(errno, std::generic_category(), "open " + path.string());
        std::ostringstream contents;
        contents << input.rdbuf();
        return from_json(contents.str(), settings);
    }

    int manhattan_ticks(const Node &first, const Node &second) noexcept {
        return std::abs(second.x - first.x) + std::abs(second.y - first.y);
    }

    Graph create_default_graph(int velocity) {
        if (velocity < 0 || velocity > 127) {
            throw GraphError("Default MIDI velocity must be between 0 and 127");
        }

        Graph graph;
        constexpr std::array positions{
            std::array{2, 2},
            std::array{6, 2},
            std::array{6, 6},
            std::array{2, 6},
        };
        constexpr std::array pitches{69, 72, 69, 71};

        std::vector<int> ids;
        ids.reserve(positions.size());
        for (std::size_t index = 0; index < positions.size(); ++index) {
            ids.push_back(
                graph.add_node(
                    positions[index][0],
                    positions[index][1],
                    pitches[index],
                    velocity)
                .id);
        }

        for (std::size_t index = 0; index < ids.size(); ++index) {
            graph.connect(ids[index], ids[(index + 1) % ids.size()]);
        }
        graph.set_start(ids.front());
        return graph;
    }
}
