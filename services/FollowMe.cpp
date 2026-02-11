#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <iostream>
#include <vector>

#include "common.h"

using app::json;

struct Vehicle {
    std::string vehicleId;
    std::string status = "empty"; // empty, reserved, moveToLandingPosition, movingWithPlane, returning
    std::string currentNode = "FS-1";
    std::string flightId;
};

class FollowMeService {
public:
    FollowMeService(std::string gcHost, int gcPort)
        : gcHost_(std::move(gcHost)), gcPort_(gcPort) {}

    void run(int port) {
        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "FollowMe"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        svr.Post("/v1/vehicles/init", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            auto body = *bodyOpt;
            if (!body.contains("vehicles") || !body["vehicles"].is_array()) {
                app::reply_json(res, 400, {{"error", "'vehicles' array required"}});
                return;
            }

            int initialized = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& v : body["vehicles"]) {
                    Vehicle vv;
                    vv.vehicleId = app::s_or(v, "vehicleId");
                    vv.currentNode = v.value("currentNode", std::string("FS-1"));
                    vv.status = v.value("status", std::string("empty"));
                    vv.flightId.clear();

                    if (vv.vehicleId.empty()) continue;
                    vehicles_[vv.vehicleId] = vv;
                    ++initialized;
                }
            }

            // sync to GroundControl
            (void)app::http_post_json(gcHost_, gcPort_, "/v1/vehicles/init", body);

            app::reply_json(res, 200, {
                {"ok", true},
                {"initialized", initialized}
            });
        });

        svr.Get("/v1/vehicles/hasEmpty", [&](const httplib::Request&, httplib::Response& res) {
            int cnt = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& [id, v] : vehicles_) {
                    if (v.status == "empty") ++cnt;
                }
            }
            app::reply_json(res, 200, {
                {"hasEmpty", cnt > 0},
                {"emptyCount", cnt}
            });
        });

        // Reserve one empty FollowMe for flight and start mission worker
        svr.Post("/v1/vehicles/reserve", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }

            const auto& body = *bodyOpt;
            const std::string flightId = app::s_or(body, "flightId");
            if (flightId.empty()) {
                app::reply_json(res, 400, {{"error", "flightId required"}});
                return;
            }

            std::string picked;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& [id, v] : vehicles_) {
                    if (v.status == "empty") {
                        v.status = "reserved";
                        v.flightId = flightId;
                        picked = id;
                        break;
                    }
                }
            }

            if (picked.empty()) {
                app::reply_json(res, 409, {
                    {"error", "no empty vehicle"}
                });
                return;
            }

            // start background mission worker
            std::thread([this, picked, flightId]() {
                this->run_mission_worker(picked, flightId);
            }).detach();

            app::reply_json(res, 200, {
                {"ok", true},
                {"vehicleId", picked},
                {"flightId", flightId},
                {"status", "reserved"}
            });
        });

        // Release vehicle (if GroundControl reservation rollback)
        svr.Post("/v1/vehicles/release", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;
            const std::string vehicleId = app::s_or(body, "vehicleId");
            if (vehicleId.empty()) {
                app::reply_json(res, 400, {{"error", "vehicleId required"}});
                return;
            }

            std::lock_guard<std::mutex> lk(mtx_);
            auto it = vehicles_.find(vehicleId);
            if (it == vehicles_.end()) {
                app::reply_json(res, 404, {{"error", "vehicle not found"}});
                return;
            }

            it->second.status = "empty";
            it->second.flightId.clear();

            app::reply_json(res, 200, {
                {"ok", true},
                {"vehicleId", vehicleId},
                {"status", "empty"}
            });
        });

        svr.Get("/v1/vehicles", [&](const httplib::Request&, httplib::Response& res) {
            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, v] : vehicles_) {
                arr.push_back({
                    {"vehicleId", v.vehicleId},
                    {"status", v.status},
                    {"currentNode", v.currentNode},
                    {"flightId", v.flightId}
                });
            }
            app::reply_json(res, 200, {{"vehicles", arr}});
        });

        std::cout << "[FollowMe] listening on 0.0.0.0:" << port << '\n';
        std::cout << "[FollowMe] GroundControl: " << gcHost_ << ":" << gcPort_ << '\n';
        svr.listen("0.0.0.0", port);
    }

private:
    void set_vehicle_status(const std::string& vehicleId, const std::string& status) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = vehicles_.find(vehicleId);
        if (it != vehicles_.end()) {
            it->second.status = status;
        }
    }

    void set_vehicle_node(const std::string& vehicleId, const std::string& node) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = vehicles_.find(vehicleId);
        if (it != vehicles_.end()) {
            it->second.currentNode = node;
        }
    }

    std::vector<std::string> json_to_nodes(const json& j, const std::string& key) {
        std::vector<std::string> out;
        if (!j.contains(key) || !j.at(key).is_array()) return out;
        for (const auto& x : j.at(key)) {
            if (x.is_string()) out.push_back(x.get<std::string>());
        }
        return out;
    }

    bool drive_route(const std::string& vehicleId, const std::string& flightId, const std::vector<std::string>& routeNodes) {
        if (routeNodes.size() < 2) {
            return true;
        }

        for (size_t i = 0; i + 1 < routeNodes.size(); ++i) {
            const std::string from = routeNodes[i];
            const std::string to = routeNodes[i + 1];

            // Wait until allowed to enter edge
            while (true) {
                auto enterRes = app::http_post_json(gcHost_, gcPort_, "/v1/map/traffic/enter-edge", {
                    {"vehicleId", vehicleId},
                    {"flightId", flightId},
                    {"from", from},
                    {"to", to}
                });

                if (enterRes.ok() && enterRes.body.value("granted", false)) {
                    break;
                }

                // wait in node
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            // simulate travel on edge
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Wait until allowed to leave edge to target node
            while (true) {
                auto leaveRes = app::http_post_json(gcHost_, gcPort_, "/v1/map/traffic/leave-edge", {
                    {"vehicleId", vehicleId},
                    {"flightId", flightId},
                    {"to", to}
                });

                if (leaveRes.ok() && leaveRes.body.value("granted", false)) {
                    set_vehicle_node(vehicleId, to);
                    break;
                }

                // wait on edge
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        return true;
    }

    void run_mission_worker(const std::string& vehicleId, const std::string& flightId) {
        // 1) Get mission path
        auto missionRes = app::http_get_json(gcHost_, gcPort_, "/v1/map/followme/path?flightId=" + flightId);
        if (!missionRes.ok()) {
            // release vehicle
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = vehicles_.find(vehicleId);
            if (it != vehicles_.end()) {
                it->second.status = "empty";
                it->second.flightId.clear();
            }
            return;
        }

        auto routeToRunway = json_to_nodes(missionRes.body, "routeToRunway");
        auto routeWithPlane = json_to_nodes(missionRes.body, "routeWithPlane");
        auto routeReturn = json_to_nodes(missionRes.body, "routeReturn");

        // 2) poll start permission every 5 sec
        while (true) {
            auto permRes = app::http_get_json(gcHost_, gcPort_, "/v1/map/followme/permission?flightId=" + flightId);
            if (permRes.ok() && permRes.body.value("allowed", false)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        // 3) move to RE-1
        set_vehicle_status(vehicleId, "moveToLandingPosition");
        if (!drive_route(vehicleId, flightId, routeToRunway)) {
            return;
        }

        // 4) move with plane to parking
        set_vehicle_status(vehicleId, "movingWithPlane");
        if (!drive_route(vehicleId, flightId, routeWithPlane)) {
            return;
        }

        // 5) inform GroundControl parking reached
        (void)app::http_post_json(gcHost_, gcPort_, "/v1/vehicles/followme", {
            {"vehicleId", vehicleId},
            {"flightId", flightId},
            {"status", "arrivedParking"}
        });

        // 6) return to FS-1
        set_vehicle_status(vehicleId, "returning");
        (void)drive_route(vehicleId, flightId, routeReturn);

        // 7) done => empty
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = vehicles_.find(vehicleId);
            if (it != vehicles_.end()) {
                it->second.status = "empty";
                it->second.flightId.clear();
            }
        }

        (void)app::http_post_json(gcHost_, gcPort_, "/v1/followme/mission/completed", {
            {"vehicleId", vehicleId},
            {"flightId", flightId}
        });
    }

private:
    std::unordered_map<std::string, Vehicle> vehicles_;
    std::mutex mtx_;

    std::string gcHost_;
    int gcPort_;
};

int main(int argc, char** argv) {
    int port = 8083;
    std::string gcHost = "localhost";
    int gcPort = 8081;

    if (argc > 1) port = std::stoi(argv[1]);

    FollowMeService s(gcHost, gcPort);
    s.run(port);
    return 0;
}