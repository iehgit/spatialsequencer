#pragma once

#include "spatial_midi/core/types.hpp"

#include <filesystem>
#include <stdexcept>
#include <vector>

namespace spatial_midi {
    class GraphError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct ProjectSettings {
        double bpm = kDefaultTempo;
        int release_gap_eighths = kDefaultReleaseGapEighths;
    };

    class Graph {
    public:
        [[nodiscard]] const std::vector<Node> &nodes() const noexcept { return nodes_; }
        [[nodiscard]] const std::vector<Edge> &edges() const noexcept { return edges_; }
        [[nodiscard]] std::optional<int> start_node_id() const noexcept { return start_node_id_; }
        [[nodiscard]] int next_node_id() const noexcept { return next_node_id_; }

        [[nodiscard]] Node *find_node(int node_id) noexcept;

        [[nodiscard]] const Node *find_node(int node_id) const noexcept;

        [[nodiscard]] Node *node_at(int x, int y) noexcept;

        [[nodiscard]] const Node *node_at(int x, int y) const noexcept;

        Node &add_node(int x, int y, int pitch = 60, int velocity = kDefaultVelocity, bool silenced = false,
                       std::vector<int> pitches = {}, RoutingMode routing_mode = RoutingMode::Random);

        Node &add_relay_node(int x, int y, RoutingMode routing_mode = RoutingMode::Random);

        void insert_node(Node node);

        void delete_node(int node_id);

        void move_node(int node_id, int x, int y);

        int set_pitch(int node_id, int pitch);

        int transpose(int node_id, int semitones);

        int set_last_pitch(int node_id, int pitch);

        int transpose_last(int node_id, int semitones);

        int set_velocity(int node_id, int velocity);

        void set_primary_pitch_and_velocity(int node_id, int pitch, int velocity);

        int adjust_velocity(int node_id, int delta);

        int append_pitch(int node_id);

        int remove_last_pitch(int node_id);

        bool set_silenced(int node_id, bool silenced);

        bool toggle_silenced(int node_id);

        RoutingMode set_routing_mode(int node_id, RoutingMode mode);

        RoutingMode toggle_routing_mode(int node_id);

        void set_start(int node_id);

        Edge connect(int source_id, int target_id);

        int remove_outgoing(int source_id);

        Edge disconnect(int source_id, int target_id);

        [[nodiscard]] std::vector<Edge> outgoing_edges(int source_id) const;

        [[nodiscard]] int edge_ticks(int source_id, std::optional<int> target_id = std::nullopt) const;

        void save_json(const std::filesystem::path &path, const ProjectSettings &settings = {}) const;

        [[nodiscard]] static Graph load_json(const std::filesystem::path &path, ProjectSettings *settings = nullptr);

        [[nodiscard]] static Graph from_json(std::string_view json, ProjectSettings *settings = nullptr);

        [[nodiscard]] std::string to_json(const ProjectSettings &settings = {}) const;

    private:
        Node &require_node(int node_id);

        const Node &require_node(int node_id) const;

        Node &require_musical_node(int node_id);

        const Node &require_musical_node(int node_id) const;

        static void validate_node(const Node &node);

        std::vector<Node> nodes_;
        std::vector<Edge> edges_;
        std::optional<int> start_node_id_;
        int next_node_id_ = 1;
    };

    [[nodiscard]] int manhattan_ticks(const Node &first, const Node &second) noexcept;

    [[nodiscard]] Graph create_default_graph(int velocity = kDefaultVelocity);
}
