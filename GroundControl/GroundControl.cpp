#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using json = nlohmann::json;

// -----------------------------
// Модели данных (как в ТЗ)
// -----------------------------
struct VehicleOnEdge {
    std::string guid;
    std::string vehicleType;  // car/plane/...
    std::string from;         // направление (в ответах можно отдавать; в вашем ТЗ иногда скрывается)
    std::string to;
    std::string status;       // reserved/moving/waiting
};

struct VehicleOnNode {
    std::string guid;
    std::string vehicleType;
    std::string status;       // reserved/waiting/action
};

struct Node {
    std::string name;
    int capacity = 1;
    std::string type; // gate/crossroad/...
    std::vector<VehicleOnNode> vehicles;
};

struct Edge {
    std::string name;
    int capacity = 2;
    std::string type; // carRoad/planeRoad
    std::string node1;
    std::string node2;
    std::vector<VehicleOnEdge> vehicles;
};

// Состояние машины в GC
struct VehicleState {
    std::string guid;
    std::string vehicleType;

    // где машина сейчас "стоит" (после arrived):
    // либо nodeName, либо edgeName
    std::string location;
    bool locationIsNode = true;

    // активная резервация на следующий шаг:
    std::optional<std::string> reservedTo;
    std::optional<bool> reservedToIsNode;

    // активное движение (после move и до arrived):
    bool inMove = false;
    std::string moveFrom;
    std::string moveTo;
    bool moveToIsNode = true;
};

// -----------------------------
// Утилиты
// -----------------------------
static std::string getenv_str(const char* key) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string();
}

static std::string to_upper(std::string s) {
    for (auto& ch : s) ch = (char)std::toupper((unsigned char)ch);
    return s;
}

static bool is_same_dir(const VehicleOnEdge& v, const std::string& from, const std::string& to) {
    return v.from == from && v.to == to;
}

static json vehicle_on_node_to_json(const VehicleOnNode& v) {
    return json{
        {"guid", v.guid},
        {"vehicleType", v.vehicleType},
        {"status", v.status}
    };
}

static json vehicle_on_edge_to_json(const VehicleOnEdge& v) {
    return json{
        {"guid", v.guid},
        {"vehicleType", v.vehicleType},
        {"from", v.from},
        {"to", v.to},
        {"status", v.status}
    };
}

static json node_to_json(const Node& n) {
    json arr = json::array();
    for (const auto& v : n.vehicles) arr.push_back(vehicle_on_node_to_json(v));
    return json{
        {"name", n.name},
        {"capacity", n.capacity},
        {"type", n.type},
        {"vehicles", arr}
    };
}

static json edge_to_json(const Edge& e) {
    json arr = json::array();
    for (const auto& v : e.vehicles) arr.push_back(vehicle_on_edge_to_json(v));
    return json{
        {"name", e.name},
        {"capacity", e.capacity},
        {"type", e.type},
        {"node1", e.node1},
        {"node2", e.node2},
        {"vehicles", arr}
    };
}

// -----------------------------
// GroundControl (in-memory)
// -----------------------------
class GroundControl {
public:
    void load_map_from_json(const json& j) {
        std::scoped_lock lk(mu_);

        nodes_.clear();
        edges_.clear();
        vehicles_.clear();

        if (!j.contains("nodes") || !j.contains("edges")) {
            throw std::runtime_error("map json must contain nodes and edges");
        }

        for (const auto& nn : j["nodes"]) {
            Node n;
            n.name = nn.at("name").get<std::string>();
            n.capacity = nn.value("capacity", 1);
            n.type = nn.value("type", "");
            nodes_[n.name] = std::move(n);
        }

        for (const auto& ee : j["edges"]) {
            Edge e;
            e.name = ee.at("name").get<std::string>();
            e.capacity = ee.value("capacity", 2);
            e.type = ee.value("type", "");
            e.node1 = ee.at("node1").get<std::string>();
            e.node2 = ee.at("node2").get<std::string>();
            if (!nodes_.count(e.node1) || !nodes_.count(e.node2)) {
                throw std::runtime_error("edge references unknown nodes: " + e.name);
            }
            edges_[e.name] = std::move(e);
        }
    }

    json get_map() const {
        std::scoped_lock lk(mu_);
        json nodes = json::array();
        for (const auto& [_, n] : nodes_) nodes.push_back(node_to_json(n));
        json edges = json::array();
        for (const auto& [_, e] : edges_) edges.push_back(edge_to_json(e));
        return json{
            {"nodes", nodes},
            {"edges", edges}
        };
    }

    std::optional<Node> get_node_copy(const std::string& name) const {
        std::scoped_lock lk(mu_);
        auto it = nodes_.find(name);
        if (it == nodes_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<Edge> get_edge_copy(const std::string& name) const {
        std::scoped_lock lk(mu_);
        auto it = edges_.find(name);
        if (it == edges_.end()) return std::nullopt;
        return it->second;
    }

    // BFS по "node<->edge<->node"
    json get_path(const std::string& fromNode, const std::string& toNode) const {
        std::scoped_lock lk(mu_);
        if (!nodes_.count(fromNode) || !nodes_.count(toNode)) {
            return json{{"path", json::array()}}; // пусто
        }

        // Вершины поиска: и nodes, и edges (по имени).
        auto neighbors = [&](const std::string& v) -> std::vector<std::string> {
            std::vector<std::string> out;
            if (nodes_.count(v)) {
                // из node -> все edges, которые инцидентны node
                for (const auto& [ename, e] : edges_) {
                    if (e.node1 == v || e.node2 == v) out.push_back(ename);
                }
            } else if (edges_.count(v)) {
                // из edge -> 2 nodes
                const auto& e = edges_.at(v);
                out.push_back(e.node1);
                out.push_back(e.node2);
            }
            return out;
        };

        std::queue<std::string> q;
        std::unordered_map<std::string, std::string> parent;
        std::unordered_set<std::string> vis;

        q.push(fromNode);
        vis.insert(fromNode);

        while (!q.empty()) {
            auto cur = q.front();
            q.pop();
            if (cur == toNode) break;

            for (auto& nb : neighbors(cur)) {
                if (vis.insert(nb).second) {
                    parent[nb] = cur;
                    q.push(nb);
                }
            }
        }

        if (!vis.count(toNode)) {
            return json{{"path", json::array()}};
        }

        std::vector<std::string> path;
        for (std::string v = toNode; !v.empty();) {
            path.push_back(v);
            auto it = parent.find(v);
            if (it == parent.end()) break;
            v = it->second;
        }
        std::reverse(path.begin(), path.end());

        return json{{"path", path}};
    }

    // POST /v1/vehicles/init
    // body: { "vehicles": ["id1","id2"], "nodes": ["G11","G12"] }
    // Для простоты считаем vehicleType="car" если не было раньше; если был - сохраняем.
    std::pair<int, json> init_vehicles(const json& body) {
        std::scoped_lock lk(mu_);

        if (!body.contains("vehicles") || !body.contains("nodes")) {
            return {400, json{{"error", "vehicles and nodes are required"}}};
        }
        auto vs = body["vehicles"];
        auto ns = body["nodes"];
        if (!vs.is_array() || !ns.is_array() || vs.size() != ns.size()) {
            return {400, json{{"error", "vehicles and nodes must be arrays of same size"}}};
        }

        for (size_t i = 0; i < vs.size(); ++i) {
            const auto guid = vs[i].get<std::string>();
            const auto nodeName = ns[i].get<std::string>();

            if (!nodes_.count(nodeName)) {
                return {400, json{{"error", "node not found: " + nodeName}}};
            }
            auto& node = nodes_.at(nodeName);
            if ((int)node.vehicles.size() >= node.capacity) {
                return {400, json{{"error", "node capacity exceeded: " + nodeName}}};
            }
            if (vehicles_.count(guid)) {
                // уже есть — нельзя “телепортировать” без clear (чтобы не ломать модель)
                return {400, json{{"error", "vehicle already exists: " + guid}}};
            }

            VehicleState st;
            st.guid = guid;
            st.vehicleType = "car";
            st.location = nodeName;
            st.locationIsNode = true;

            vehicles_[guid] = st;
            node.vehicles.push_back(VehicleOnNode{guid, st.vehicleType, "action"});
        }

        return {200, json{{"success", true}}};
    }

    // DELETE /v1/vehicles/clear?vehicleType=BUS
    std::pair<int, json> clear_vehicles_of_type(const std::string& vehicleType) {
        std::scoped_lock lk(mu_);

        std::string vt = vehicleType;
        if (vt.empty()) return {400, json{{"error", "vehicleType is required"}}};

        // удалить из nodes
        for (auto& [_, n] : nodes_) {
            n.vehicles.erase(
                std::remove_if(n.vehicles.begin(), n.vehicles.end(),
                               [&](const VehicleOnNode& v){ return to_upper(v.vehicleType) == to_upper(vt); }),
                n.vehicles.end()
            );
        }
        // удалить из edges
        for (auto& [_, e] : edges_) {
            e.vehicles.erase(
                std::remove_if(e.vehicles.begin(), e.vehicles.end(),
                               [&](const VehicleOnEdge& v){ return to_upper(v.vehicleType) == to_upper(vt); }),
                e.vehicles.end()
            );
        }
        // удалить из vehicle states
        for (auto it = vehicles_.begin(); it != vehicles_.end();) {
            if (to_upper(it->second.vehicleType) == to_upper(vt)) it = vehicles_.erase(it);
            else ++it;
        }

        return {200, json{{"success", true}}};
    }

    // GET /v1/vehicles/move_permission?guid=...&from=...&to=...
    std::pair<int, json> move_permission(const std::string& guid,
                                         const std::string& from,
                                         const std::string& to) {
        std::scoped_lock lk(mu_);

        if (!vehicles_.count(guid)) return {400, json{{"error", "unknown vehicle guid"}}};
        auto& st = vehicles_.at(guid);

        if (st.inMove) {
            return {400, json{{"error", "vehicle is in move, arrived required"}}};
        }
        if (st.location != from) {
            return {400, json{{"error", "vehicle not at 'from' location"}}};
        }

        bool toIsNode = nodes_.count(to) > 0;
        bool toIsEdge = edges_.count(to) > 0;
        if (!toIsNode && !toIsEdge) return {400, json{{"error", "unknown 'to' location"}}};

        // Резерв на node
        if (toIsNode) {
            auto& node = nodes_.at(to);

            // уже занято?
            if ((int)node.vehicles.size() >= node.capacity) {
                return {200, json{{"guid", guid}, {"from", from}, {"to", to}, {"allowed", false}}};
            }
            // резервируем
            node.vehicles.push_back(VehicleOnNode{guid, st.vehicleType, "reserved"});
            st.reservedTo = to;
            st.reservedToIsNode = true;

            return {200, json{{"guid", guid}, {"from", from}, {"to", to}, {"allowed", true}}};
        }

        // Резерв на edge
        auto& e = edges_.at(to);

        // Валидность направления: если едем в edge, from должен быть endpoint edge
        if (nodes_.count(from)) {
            if (!(e.node1 == from || e.node2 == from)) {
                return {400, json{{"error", "from node is not incident to edge"}}};
            }
            std::string other = (e.node1 == from) ? e.node2 : e.node1;

            // capacity edge
            if ((int)e.vehicles.size() >= e.capacity) {
                return {200, json{{"guid", guid}, {"from", from}, {"to", to}, {"allowed", false}}};
            }
            // per-direction exclusivity for reserved/moving
            for (const auto& v : e.vehicles) {
                if (is_same_dir(v, from, other) && (v.status == "reserved" || v.status == "moving")) {
                    return {200, json{{"guid", guid}, {"from", from}, {"to", to}, {"allowed", false}}};
                }
            }

            e.vehicles.push_back(VehicleOnEdge{guid, st.vehicleType, from, other, "reserved"});
            st.reservedTo = to;
            st.reservedToIsNode = false;

            return {200, json{{"guid", guid}, {"from", from}, {"to", to}, {"allowed", true}}};
        }

        // Если from — edge, а to — edge (редко, но формально вы это не запрещали).
        // Мы это запрещаем как некорректный шаг маршрута (обычно edge->node->edge).
        return {400, json{{"error", "edge->edge move is not supported; use edge->node or node->edge"}}};
    }

    // POST /v1/vehicles/move
    // body: { guid, vehicleType, from, to }
    std::pair<int, json> move_start(const json& body) {
        std::scoped_lock lk(mu_);

        const auto guid = body.value("guid", "");
        const auto from = body.value("from", "");
        const auto to = body.value("to", "");
        const auto vtype = body.value("vehicleType", "");

        if (guid.empty() || from.empty() || to.empty() || vtype.empty()) {
            return {400, json{{"error", "guid, vehicleType, from, to are required"}}};
        }
        if (!vehicles_.count(guid)) return {400, json{{"error", "unknown vehicle guid"}}};

        auto& st = vehicles_.at(guid);
        if (st.vehicleType != vtype) st.vehicleType = vtype; // позволим обновить тип

        if (st.inMove) return {400, json{{"error", "already moving"}}};
        if (st.location != from) return {400, json{{"error", "vehicle not at 'from' location"}}};

        // проверить резервацию
        if (!st.reservedTo.has_value() || st.reservedTo.value() != to) {
            return {400, json{{"error", "move_permission was not granted or reservation mismatch"}}};
        }

        bool toIsNode = (st.reservedToIsNode.has_value() ? st.reservedToIsNode.value() : nodes_.count(to) > 0);
        bool toIsEdge = edges_.count(to) > 0;

        if (toIsNode) {
            // Движение edge->node: from должен быть edge
            if (!edges_.count(from)) {
                return {400, json{{"error", "node->node direct move is not supported; use node->edge->node"}}};
            }
            auto& e = edges_.at(from);

            // Найдем запись машины в edge и поставим moving к to-node
            bool found = false;
            for (auto& v : e.vehicles) {
                if (v.guid == guid) {
                    v.status = "moving";
                    v.to = to; // конечная точка (node)
                    found = true;
                    break;
                }
            }
            if (!found) {
                // если машины нет на ребре, значит модель нарушена
                return {400, json{{"error", "vehicle not found on 'from' edge"}}};
            }

            // Освобождаем "предыдущую точку": это edge не освобождаем (она там остается пока едет),
            // но мы обязаны освободить node, если вдруг была на node. Здесь from=edge, так что ничего.

            // Переводим reservation на node: оставляем reserved запись в node (она уже была добавлена в move_permission)
            st.inMove = true;
            st.moveFrom = from;
            st.moveTo = to;
            st.moveToIsNode = true;

            return {200, json{{"success", true}}};
        }

        if (toIsEdge) {
            // Движение node->edge: from должен быть node
            if (!nodes_.count(from)) {
                return {400, json{{"error", "edge->edge move is not supported"}}};
            }
            auto& n = nodes_.at(from);
            // убираем машину из node (освобождаем точку)
            n.vehicles.erase(
                std::remove_if(n.vehicles.begin(), n.vehicles.end(),
                               [&](const VehicleOnNode& v){ return v.guid == guid; }),
                n.vehicles.end()
            );

            auto& e = edges_.at(to);
            // запись на ребре уже есть со статусом reserved (создана move_permission) — меняем на moving
            bool ok = false;
            for (auto& v : e.vehicles) {
                if (v.guid == guid && v.status == "reserved") {
                    v.status = "moving";
                    ok = true;
                    break;
                }
            }
            if (!ok) return {400, json{{"error", "edge reservation not found"}}};

            st.inMove = true;
            st.moveFrom = from;
            st.moveTo = to;
            st.moveToIsNode = false;

            // location пока считаем "на ребре" (фактически едет по нему)
            st.location = to;
            st.locationIsNode = false;

            return {200, json{{"success", true}}};
        }

        return {400, json{{"error", "unknown to"}}};
    }

    // POST /v1/vehicles/arrived
    // body: { guid, vehicleType, from, to }
    std::pair<int, json> arrived(const json& body) {
        std::scoped_lock lk(mu_);

        const auto guid = body.value("guid", "");
        const auto from = body.value("from", "");
        const auto to = body.value("to", "");
        const auto vtype = body.value("vehicleType", "");

        if (guid.empty() || from.empty() || to.empty() || vtype.empty()) {
            return {400, json{{"error", "guid, vehicleType, from, to are required"}}};
        }
        if (!vehicles_.count(guid)) return {400, json{{"error", "unknown vehicle guid"}}};

        auto& st = vehicles_.at(guid);
        if (!st.inMove) return {400, json{{"error", "vehicle is not in move"}}};
        if (st.moveFrom != from || st.moveTo != to) return {400, json{{"error", "arrived mismatch with active move"}}};

        // прибытие в node
        if (nodes_.count(to)) {
            auto& node = nodes_.at(to);

            // найти reserved запись и превратить в action
            bool reservedFound = false;
            for (auto& v : node.vehicles) {
                if (v.guid == guid && v.status == "reserved") {
                    v.status = "action";
                    v.vehicleType = vtype;
                    reservedFound = true;
                    break;
                }
            }
            if (!reservedFound) {
                return {400, json{{"error", "node reservation not found"}}};
            }

            // убрать с ребра from (машина ехала по from-edge)
            if (!edges_.count(from)) return {400, json{{"error", "from edge not found"}}};
            auto& e = edges_.at(from);
            e.vehicles.erase(
                std::remove_if(e.vehicles.begin(), e.vehicles.end(),
                               [&](const VehicleOnEdge& v){ return v.guid == guid; }),
                e.vehicles.end()
            );

            st.location = to;
            st.locationIsNode = true;
            st.inMove = false;
            st.reservedTo.reset();
            st.reservedToIsNode.reset();

            return {200, json{{"success", true}}};
        }

        // прибытие на edge (остановились/докатились до ребра) — ставим waiting
        if (edges_.count(to)) {
            auto& e = edges_.at(to);

            bool found = false;
            for (auto& v : e.vehicles) {
                if (v.guid == guid) {
                    v.status = "waiting";
                    v.vehicleType = vtype;
                    found = true;
                    break;
                }
            }
            if (!found) return {400, json{{"error", "vehicle not found on edge"}}};

            st.location = to;
            st.locationIsNode = false;
            st.inMove = false;
            st.reservedTo.reset();
            st.reservedToIsNode.reset();

            return {200, json{{"success", true}}};
        }

        return {400, json{{"error", "unknown to"}}};
    }

    // -----------------------------
    // Самолеты (упрощенно)
    // -----------------------------

    // GET /v1/vehicles/planes/land_permission?guid=...&runway=...
    std::pair<int, json> plane_land_permission(const std::string& guid, const std::string& runway) {
        std::scoped_lock lk(mu_);

        if (guid.empty() || runway.empty()) return {400, json{{"error", "guid and runway are required"}}};
        if (!nodes_.count(runway)) return {200, json{{"allowed", false}, {"error", "runway not found"}, {"retryAfter", 5000}}};

        // runway должен быть свободен
        auto& rw = nodes_.at(runway);
        if (!rw.vehicles.empty()) {
            return {200, json{{"allowed", false}, {"error", "runway busy"}, {"retryAfter", 5000}}};
        }

        // найти свободный planeParking
        std::string parking;
        for (auto& [name, n] : nodes_) {
            if (n.type == "planeParking" && (int)n.vehicles.size() < n.capacity) {
                parking = name;
                break;
            }
        }
        if (parking.empty()) {
            return {200, json{{"allowed", false}, {"error", "no free planeParking"}, {"retryAfter", 5000}}};
        }

        // создадим/обновим самолет в системе, зарезервируем runway (как node action) и parking (reserved, как “куда потом”)
        if (!vehicles_.count(guid)) {
            vehicles_[guid] = VehicleState{guid, "plane", runway, true};
        }
        auto& st = vehicles_.at(guid);
        st.vehicleType = "plane";
        st.location = runway;
        st.locationIsNode = true;

        // поставить самолет на runway как action (мы считаем, что при разрешении посадки runway закреплен)
        rw.vehicles.push_back(VehicleOnNode{guid, "plane", "action"});

        // parking резервируем (как будущую цель, чтобы визуализатор/табло знали)
        auto& pk = nodes_.at(parking);
        pk.vehicles.push_back(VehicleOnNode{guid, "plane", "reserved"});

        return {200, json{{"allowed", true}, {"planeParking", parking}}};
    }

    // POST /v1/vehicles/planes/land { guid, runway }
    std::pair<int, json> plane_land(const json& body) {
        std::scoped_lock lk(mu_);

        auto guid = body.value("guid", "");
        auto runway = body.value("runway", "");
        if (guid.empty() || runway.empty()) return {400, json{{"success", false}, {"error", "guid and runway required"}}};
        if (!vehicles_.count(guid)) return {400, json{{"success", false}, {"error", "no land_permission"}}};
        if (!nodes_.count(runway)) return {400, json{{"success", false}, {"error", "runway not found"}}};

        // если самолет не на runway (по нашей модели — должен быть action на runway после land_permission)
        auto& rw = nodes_.at(runway);
        bool onRunway = false;
        for (const auto& v : rw.vehicles) if (v.guid == guid) onRunway = true;
        if (!onRunway) return {400, json{{"success", false}, {"error", "runway not reserved for this plane"}}};

        return {200, json{{"success", true}}};
    }

    // GET /v1/vehicles/planes/takeoff_permission?guid=...&runway=...
    std::pair<int, json> plane_takeoff_permission(const std::string& guid, const std::string& runway) {
        std::scoped_lock lk(mu_);

        if (guid.empty() || runway.empty()) return {400, json{{"error", "guid and runway are required"}}};
        if (!vehicles_.count(guid)) return {200, json{{"allowed", false}, {"error", "unknown plane"}, {"retryAfter", 5000}}};
        if (!nodes_.count(runway)) return {200, json{{"allowed", false}, {"error", "runway not found"}, {"retryAfter", 5000}}};

        // разрешаем взлет если самолет сейчас action на runway
        auto& rw = nodes_.at(runway);
        bool ok = false;
        for (const auto& v : rw.vehicles) {
            if (v.guid == guid && v.status == "action") ok = true;
        }
        if (!ok) return {200, json{{"allowed", false}, {"error", "plane not on runway"}, {"retryAfter", 5000}}};

        return {200, json{{"allowed", true}}};
    }

    // POST /v1/vehicles/planes/takeoff { guid, runway }
    std::pair<int, json> plane_takeoff(const json& body) {
        std::scoped_lock lk(mu_);

        auto guid = body.value("guid", "");
        auto runway = body.value("runway", "");
        if (guid.empty() || runway.empty()) return {400, json{{"error", "guid and runway required"}}};
        if (!vehicles_.count(guid)) return {400, json{{"error", "unknown plane"}}};
        if (!nodes_.count(runway)) return {400, json{{"error", "runway not found"}}};

        auto& rw = nodes_.at(runway);
        bool ok = false;
        for (const auto& v : rw.vehicles) if (v.guid == guid) ok = true;
        if (!ok) return {400, json{{"error", "plane not on runway"}}};

        // удалить самолет с runway и вообще из системы (взлетел)
        rw.vehicles.erase(
            std::remove_if(rw.vehicles.begin(), rw.vehicles.end(),
                           [&](const VehicleOnNode& v){ return v.guid == guid; }),
            rw.vehicles.end()
        );
        // убрать из planeParking reserved (если есть)
        for (auto& [_, n] : nodes_) {
            n.vehicles.erase(
                std::remove_if(n.vehicles.begin(), n.vehicles.end(),
                               [&](const VehicleOnNode& v){ return v.guid == guid && v.status == "reserved"; }),
                n.vehicles.end()
            );
        }
        vehicles_.erase(guid);

        return {200, json{{"success", true}}};
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, Node> nodes_;
    std::unordered_map<std::string, Edge> edges_;
    std::unordered_map<std::string, VehicleState> vehicles_;
};

// -----------------------------
// Визуализатор (best-effort)
// -----------------------------
static void notify_visualizer_renderMove(const std::string& baseUrl, const json& payload) {
    // baseUrl вида: http://localhost:9000
    if (baseUrl.empty()) return;
    // очень простой парсер (без https). Для учебного сервиса норм.
    std::string url = baseUrl;
    bool https = false;
    if (url.rfind("https://", 0) == 0) { https = true; url = url.substr(8); }
    else if (url.rfind("http://", 0) == 0) { url = url.substr(7); }

    std::string host = url;
    int port = https ? 443 : 80;
    auto pos = url.find(':');
    if (pos != std::string::npos) {
        host = url.substr(0, pos);
        port = std::stoi(url.substr(pos + 1));
    }

    try {
        std::unique_ptr<httplib::Client> cli;
// #ifdef CPPHTTPLIB_OPENSSL_SUPPORT
//         if (https) cli = std::make_unique<httplib::SSLClient>(host, port);
//         else
// #endif
        cli = std::make_unique<httplib::Client>(host, port);

        cli->set_connection_timeout(0, 500000);
        cli->set_read_timeout(0, 500000);
        cli->set_write_timeout(0, 500000);

        cli->Post("/renderMove", payload.dump(), "application/json");
    } catch (...) {
        // игнорируем ошибки (best-effort)
    }
}

// -----------------------------
// main: HTTP server
// -----------------------------
int main(int argc, char** argv) {
    int port = 8001;
    std::string mapFile = "/home/user/dir/programming/C++/Yaroslava/Airport/GroundControl/GroundMap.json";

    GroundControl gc;

    if (!mapFile.empty()) {
        std::ifstream in(mapFile);
        if (!in) {
            std::cerr << "Cannot open map file: " << mapFile << "\n";
            return 1;
        }
        json j;
        in >> j;
        gc.load_map_from_json(j);
    }

    const std::string renderBaseUrl = getenv_str("GC_RENDER_BASE_URL"); // пример: http://localhost:9000

    httplib::Server srv;

    srv.Get("/health", [](const httplib::Request&, httplib::Response& res){
        res.set_content(R"({"ok":true})", "application/json");
        res.status = 200;
    });

    // GET /v1/map
    srv.Get("/v1/map", [&](const httplib::Request&, httplib::Response& res){
        auto out = gc.get_map();
        res.set_content(out.dump(2), "application/json");
        res.status = 200;
    });

    // GET /v1/map/path/?from=...&to=...
    srv.Get("/v1/map/path/", [&](const httplib::Request& req, httplib::Response& res){
        auto from = req.get_param_value("from");
        auto to = req.get_param_value("to");
        auto out = gc.get_path(from, to);
        res.set_content(out.dump(2), "application/json");
        res.status = 200;
    });

    // GET /v1/map/nodes/{nodeName}
    srv.Get(R"(/v1/map/nodes/([^/]+))", [&](const httplib::Request& req, httplib::Response& res){
        auto name = req.matches[1].str();
        auto n = gc.get_node_copy(name);
        if (!n) { res.status = 404; res.set_content(R"({"error":"not found"})", "application/json"); return; }
        res.status = 200;
        res.set_content(node_to_json(*n).dump(2), "application/json");
    });

    // GET /v1/map/edges/{edgeName}
    srv.Get(R"(/v1/map/edges/([^/]+))", [&](const httplib::Request& req, httplib::Response& res){
        auto name = req.matches[1].str();
        auto e = gc.get_edge_copy(name);
        if (!e) { res.status = 404; res.set_content(R"({"error":"not found"})", "application/json"); return; }
        res.status = 200;
        res.set_content(edge_to_json(*e).dump(2), "application/json");
    });

    // POST /v1/vehicles/init
    srv.Post("/v1/vehicles/init", [&](const httplib::Request& req, httplib::Response& res){
        try {
            auto body = json::parse(req.body);
            auto [code, out] = gc.init_vehicles(body);
            res.status = code;
            res.set_content(out.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(2), "application/json");
        }
    });

    // DELETE /v1/vehicles/clear?vehicleType=BUS
    srv.Delete("/v1/vehicles/clear", [&](const httplib::Request& req, httplib::Response& res){
        auto vt = req.get_param_value("vehicleType");
        auto [code, out] = gc.clear_vehicles_of_type(vt);
        res.status = code;
        res.set_content(out.dump(2), "application/json");
    });

    // GET /v1/vehicles/move_permission?guid=...&from=...&to=...
    srv.Get("/v1/vehicles/move_permission", [&](const httplib::Request& req, httplib::Response& res){
        auto guid = req.get_param_value("guid");
        auto from = req.get_param_value("from");
        auto to = req.get_param_value("to");

        auto [code, out] = gc.move_permission(guid, from, to);
        res.status = code;
        res.set_content(out.dump(2), "application/json");
    });

    // POST /v1/vehicles/move
    srv.Post("/v1/vehicles/move", [&](const httplib::Request& req, httplib::Response& res){
        try {
            auto body = json::parse(req.body);
            auto [code, out] = gc.move_start(body);
            res.status = code;
            res.set_content(out.dump(2), "application/json");

            if (code == 200) {
                // best-effort визуализатору
                json payload = {
                    {"guid", body.value("guid","")},
                    {"from", body.value("from","")},
                    {"to", body.value("to","")}
                };
                notify_visualizer_renderMove(renderBaseUrl, payload);
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(2), "application/json");
        }
    });

    // POST /v1/vehicles/arrived
    srv.Post("/v1/vehicles/arrived", [&](const httplib::Request& req, httplib::Response& res){
        try {
            auto body = json::parse(req.body);
            auto [code, out] = gc.arrived(body);
            res.status = code;
            res.set_content(out.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(2), "application/json");
        }
    });

    // ---- Plane endpoints ----

    // GET /v1/vehicles/planes/land_permission?guid=...&runway=...
    srv.Get("/v1/vehicles/planes/land_permission", [&](const httplib::Request& req, httplib::Response& res){
        auto guid = req.get_param_value("guid");
        auto runway = req.get_param_value("runway");
        auto [code, out] = gc.plane_land_permission(guid, runway);
        res.status = code;
        res.set_content(out.dump(2), "application/json");
    });

    // POST /v1/vehicles/planes/land
    srv.Post("/v1/vehicles/planes/land", [&](const httplib::Request& req, httplib::Response& res){
        try {
            auto body = json::parse(req.body);
            auto [code, out] = gc.plane_land(body);
            res.status = code;
            res.set_content(out.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"success", false}, {"error", e.what()}}.dump(2), "application/json");
        }
    });

    // GET /v1/vehicles/planes/takeoff_permission?guid=...&runway=...
    srv.Get("/v1/vehicles/planes/takeoff_permission", [&](const httplib::Request& req, httplib::Response& res){
        auto guid = req.get_param_value("guid");
        auto runway = req.get_param_value("runway");
        auto [code, out] = gc.plane_takeoff_permission(guid, runway);
        res.status = code;
        res.set_content(out.dump(2), "application/json");
    });

    // POST /v1/vehicles/planes/takeoff
    srv.Post("/v1/vehicles/planes/takeoff", [&](const httplib::Request& req, httplib::Response& res){
        try {
            auto body = json::parse(req.body);
            auto [code, out] = gc.plane_takeoff(body);
            res.status = code;
            res.set_content(out.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(2), "application/json");
        }
    });

    std::cout << "GroundControl listening on port " << port << "\n";
    std::cout << "Visualizer base URL: " << (renderBaseUrl.empty() ? "(disabled)" : renderBaseUrl) << "\n";

    srv.listen("0.0.0.0", port);
    return 0;
}
