#include <atomic>
#include <chrono>
#include <cctype>      // <-- isalnum
#include <iomanip>
#include <iostream>
#include <memory>      // <-- unique_ptr, make_unique
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include "common.h"

using app::json;

namespace {

std::string url_encode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c : s) {
        if (std::isalnum(static_cast<int>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

struct Agent {
    std::string flightId;
    std::string kind;   // "airborne" | "grounded"
    std::string state;  // human-readable
    int64_t startedAt = 0;

    std::atomic<bool> stop{false};
    std::thread th;

    // config
    std::string gcHost = "localhost";
    int gcPort = 8081;
    int pollSec = 30;
    int actionDelaySec = 2;      // touchdown or takeoff delay
    int handlingSec = 15;        // for grounded
    std::string parkingNode;     // for grounded

    std::string lastError;
};

class BoardService {
public:
    void run(int port) {
        httplib::Server svr;

        svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
            app::reply_json(res, 200, {
                {"service", "Board"},
                {"status", "ok"},
                {"time", app::now_sec()}
            });
        });

        // Create airborne plane agent
        // body: { "flightId":"SU100", "gcHost":"localhost", "gcPort":8081, "pollSec":30, "touchdownDelaySec":2 }
        svr.Post("/v1/planes/airborne", [&](const httplib::Request& req, httplib::Response& res) {
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

            auto a = std::make_unique<Agent>();
            a->flightId = flightId;
            a->kind = "airborne";
            a->state = "created";
            a->startedAt = app::now_sec();
            a->gcHost = body.value("gcHost", std::string("localhost"));
            a->gcPort = body.value("gcPort", 8081);
            a->pollSec = body.value("pollSec", 30);
            a->actionDelaySec = body.value("touchdownDelaySec", 2);

            const bool ok = start_or_replace_agent(std::move(a));
            app::reply_json(res, ok ? 200 : 500, {
                {"ok", ok},
                {"flightId", flightId},
                {"kind", "airborne"}
            });
        });

        // Create grounded plane agent
        // body: { "flightId":"SU100", "parkingNode":"P-5", "gcHost":"localhost", "gcPort":8081,
        //         "pollSec":10, "handlingSec":15, "takeoffDelaySec":2 }
        svr.Post("/v1/planes/grounded", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const auto& body = *bodyOpt;

            const std::string flightId = app::s_or(body, "flightId");
            const std::string parkingNode = app::s_or(body, "parkingNode");

            if (flightId.empty() || parkingNode.empty()) {
                app::reply_json(res, 400, {{"error", "flightId and parkingNode required"}});
                return;
            }

            auto a = std::make_unique<Agent>();
            a->flightId = flightId;
            a->kind = "grounded";
            a->state = "created";
            a->startedAt = app::now_sec();
            a->gcHost = body.value("gcHost", std::string("localhost"));
            a->gcPort = body.value("gcPort", 8081);
            a->pollSec = body.value("pollSec", 10);
            a->handlingSec = body.value("handlingSec", 15);
            a->actionDelaySec = body.value("takeoffDelaySec", 2);
            a->parkingNode = parkingNode;

            const bool ok = start_or_replace_agent(std::move(a));
            app::reply_json(res, ok ? 200 : 500, {
                {"ok", ok},
                {"flightId", flightId},
                {"kind", "grounded"}
            });
        });

        // List agents
        svr.Get("/v1/planes", [&](const httplib::Request&, httplib::Response& res) {
            json arr = json::array();
            std::lock_guard<std::mutex> lk(mtx_);
            for (const auto& [id, a] : agents_) {
                arr.push_back({
                    {"flightId", a->flightId},
                    {"kind", a->kind},
                    {"state", a->state},
                    {"parkingNode", a->parkingNode},
                    {"gcHost", a->gcHost},
                    {"gcPort", a->gcPort},
                    {"pollSec", a->pollSec},
                    {"startedAt", a->startedAt},
                    {"lastError", a->lastError}
                });
            }
            app::reply_json(res, 200, {{"planes", arr}});
        });

        // Stop agent
        // POST /v1/planes/stop { "flightId":"SU100" }
        svr.Post("/v1/planes/stop", [&](const httplib::Request& req, httplib::Response& res) {
            auto bodyOpt = app::parse_json_body(req);
            if (!bodyOpt) {
                app::reply_json(res, 400, {{"error", "invalid json"}});
                return;
            }
            const std::string flightId = app::s_or(*bodyOpt, "flightId");
            if (flightId.empty()) {
                app::reply_json(res, 400, {{"error", "flightId required"}});
                return;
            }
            const bool ok = stop_agent(flightId);
            app::reply_json(res, 200, {{"ok", ok}, {"flightId", flightId}});
        });

        std::cout << "[Board] listening on 0.0.0.0:" << port << "\n";
        svr.listen("0.0.0.0", port);

        stop_all();
    }

private:
    bool start_or_replace_agent(std::unique_ptr<Agent> ptr) {
        if (!ptr || ptr->flightId.empty()) return false;

        const std::string flightId = ptr->flightId;

        // stop existing
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = agents_.find(flightId);
            if (it != agents_.end()) {
                it->second->stop.store(true);
            }
        }
        join_and_erase(flightId);

        // insert new
        Agent* ap = ptr.get();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            agents_[flightId] = std::move(ptr);
        }

        // start thread (agent object already owned by map)
        ap->th = std::thread([this, ap]() {
            if (ap->kind == "airborne") {
                run_airborne(*ap);
            } else {
                run_grounded(*ap);
            }
        });

        return true;
    }

    void run_airborne(Agent& a) {
        a.state = "airborne.polling_land_permission";

        while (!a.stop.load()) {
            const std::string path = "/v1/land_permission?flightId=" + url_encode(a.flightId);
            auto r = app::http_get_json(a.gcHost, a.gcPort, path);

            if (!r.ok()) {
                a.lastError = "land_permission http error";
                std::this_thread::sleep_for(std::chrono::seconds(a.pollSec));
                continue;
            }

            const bool allowed = r.body.value("allowed", false);
            if (!allowed) {
                a.state = std::string("airborne.denied_or_delayed:") + r.body.value("reason", "");
                std::this_thread::sleep_for(std::chrono::seconds(a.pollSec));
                continue;
            }

            a.state = "airborne.landing_approved";
            std::this_thread::sleep_for(std::chrono::seconds(a.actionDelaySec));

            const std::string landedPath = "/v1/flights/" + url_encode(a.flightId) + "/landed";
            auto rr = app::http_post_json(a.gcHost, a.gcPort, landedPath, json::object());
            if (!rr.ok()) {
                a.lastError = "failed to POST landed";
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }

            a.state = "airborne.landed_notified";
            return;
        }

        a.state = "stopped";
    }

    void run_grounded(Agent& a) {
        a.state = "grounded.request_handling_stub";

        // HandlingSupervisor stub (если сервиса нет — это не ошибка)
        (void)app::http_post_json("localhost", 8085, "/v1/handling/request", {
            {"flightId", a.flightId},
            {"parkingNode", a.parkingNode},
            {"services", json::array({"fuel", "catering", "boarding"})}
        });

        a.state = "grounded.handling_in_progress";
        for (int i = 0; i < a.handlingSec && !a.stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (a.stop.load()) {
            a.state = "stopped";
            return;
        }

        a.state = "grounded.handling_done_poll_takeoff_permission";

        while (!a.stop.load()) {
            const std::string path = "/v1/takeoff_permission?flightId=" + url_encode(a.flightId);
            auto r = app::http_get_json(a.gcHost, a.gcPort, path);

            if (!r.ok()) {
                a.lastError = "takeoff_permission http error";
                std::this_thread::sleep_for(std::chrono::seconds(a.pollSec));
                continue;
            }

            const bool allowed = r.body.value("allowed", false);
            if (!allowed) {
                a.state = std::string("grounded.takeoff_denied:") + r.body.value("reason", "");
                std::this_thread::sleep_for(std::chrono::seconds(a.pollSec));
                continue;
            }

            // разрешили взлет — ПОКА НИЧЕГО НЕ ДЕЛАЕМ (как ты попросила)
            a.state = "grounded.takeoff_approved_noop";
            a.lastError.clear();

            // просто остаёмся живыми до stop (или можешь return, если хочешь авто-завершение)
            while (!a.stop.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            a.state = "stopped";
            return;

            // const bool allowed = r.body.value("allowed", false);
            // if (!allowed) {
            //     a.state = std::string("grounded.takeoff_denied:") + r.body.value("reason", "");
            //     std::this_thread::sleep_for(std::chrono::seconds(a.pollSec));
            //     continue;
            // }
            //
            // a.state = "grounded.takeoff_approved";
            // std::this_thread::sleep_for(std::chrono::seconds(a.actionDelaySec));
            //
            // const std::string tookoffPath = "/v1/flights/" + url_encode(a.flightId) + "/tookoff";
            // auto rr = app::http_post_json(a.gcHost, a.gcPort, tookoffPath, json::object());
            // if (!rr.ok()) {
            //     a.lastError = "failed to POST tookoff";
            //     std::this_thread::sleep_for(std::chrono::seconds(2));
            //     continue;
            // }
            //
            // a.state = "grounded.tookoff_notified";
            // return;
        }

        a.state = "stopped";
    }

    bool stop_agent(const std::string& flightId) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = agents_.find(flightId);
            if (it == agents_.end()) {
                return false;
            }
            it->second->stop.store(true);
        }
        join_and_erase(flightId);
        return true;
    }

    void join_and_erase(const std::string& flightId) {
        std::unique_ptr<Agent> victim;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = agents_.find(flightId);
            if (it == agents_.end()) return;
            victim = std::move(it->second); // забираем владение
            agents_.erase(it);
        }

        if (victim && victim->th.joinable()) {
            victim->th.join();
        }
    }

    void stop_all() {
        std::unordered_map<std::string, std::unique_ptr<Agent>> tmp;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& [id, a] : agents_) {
                a->stop.store(true);
            }
            tmp = std::move(agents_);
        }
        for (auto& [id, a] : tmp) {
            if (a->th.joinable()) a->th.join();
        }
    }

private:
    std::mutex mtx_;
    std::unordered_map<std::string, std::unique_ptr<Agent>> agents_;
};

} // namespace

int main(int argc, char** argv) {
    int port = 8084;
    if (argc > 1) port = std::stoi(argv[1]);

    BoardService s;
    s.run(port);
    return 0;
}
