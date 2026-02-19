#include <unordered_map>
#include <mutex>
#include <vector>
#include <iostream>
#include <algorithm>

#include "common.h"

using app::json;

struct FlightRecord {
    std::string flightId;
    int64_t scheduledAt = 0;             // epoch seconds
    std::string status = "Scheduled";    // Scheduled, Delayed, LandingApproved, Landed, ArrivedParking, Parked, TakeoffApproved, Departed, Cancelled, Denied...
    std::string phase = "airborne";      // airborne | grounded | terminal
    std::string parkingNode;             // e.g. P-5 when grounded (optional)
    int64_t updatedAt = 0;
};

static bool is_terminal_status(const std::string& s) {
    return s == "Departed" || s == "Cancelled" || s == "Denied";
}

static bool is_ground_status(const std::string& s) {
    return s == "ArrivedParking" || s == "Parked" || s == "OnStand";
}

static bool is_air_status(const std::string& s) {
    return s == "Scheduled" || s == "Delayed" || s == "LandingApproved" || s == "Airborne" || s == "LandedAtRE1";
}

static std::string infer_phase(const std::string& status, const std::string& explicitPhase) {
    if (!explicitPhase.empty()) return explicitPhase;
    if (is_terminal_status(status)) return "terminal";
    if (is_ground_status(status)) return "grounded";
    if (is_air_status(status)) return "airborne";
    // default fallback:
    return "airborne";
}

static bool compute_takeoff_allowed(const FlightRecord& rec) {
    // Панель — "информтабло", поэтому критерии можно держать простыми.
    // Разрешаем запрос взлёта, если самолёт на земле и не в терминальном состоянии.
    if (rec.phase != "grounded") return false;
    if (is_terminal_status(rec.status)) return false;
    return true;
}

int main(int argc, char** argv) {
    int port = 8082;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    std::unordered_map<std::string, FlightRecord> flights;
    std::mutex mtx;

    httplib::Server svr;

    svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
        app::reply_json(res, 200, {
            {"service", "InformationPanel"},
            {"status", "ok"},
            {"time", app::now_sec()}
        });
    });

    // INIT flights
    // body:
    // {
    //   "flights":[
    //      {"flightId":"SU100","scheduledAt":..., "status":"Scheduled", "phase":"airborne"},
    //      {"flightId":"SU200","status":"Parked","phase":"grounded","parkingNode":"P-3"}
    //   ]
    // }
    svr.Post("/v1/flights/init", [&](const httplib::Request& req, httplib::Response& res) {
        auto bodyOpt = app::parse_json_body(req);
        if (!bodyOpt) {
            app::reply_json(res, 400, {{"error", "invalid json"}});
            return;
        }
        const auto& body = *bodyOpt;

        if (!body.contains("flights") || !body["flights"].is_array()) {
            app::reply_json(res, 400, {{"error", "field 'flights' array is required"}});
            return;
        }

        int count = 0;
        json rejected = json::array();

        std::lock_guard<std::mutex> lk(mtx);
        for (const auto& f : body["flights"]) {
            const std::string flightId = app::s_or(f, "flightId");
            if (flightId.empty()) {
                rejected.push_back({{"reason", "empty_flightId"}});
                continue;
            }

            FlightRecord rec;
            rec.flightId = flightId;
            rec.scheduledAt = f.value("scheduledAt", app::now_sec());
            rec.status = f.value("status", std::string("Scheduled"));

            const std::string explicitPhase = f.value("phase", std::string(""));
            rec.phase = infer_phase(rec.status, explicitPhase);

            rec.parkingNode = f.value("parkingNode", std::string(""));

            // если явно grounded, но parkingNode не задан — это не фатально, но GC не сможет посадить plane на карту
            // поэтому отметим warning в rejected (но запись создадим).
            if (rec.phase == "grounded" && rec.parkingNode.empty()) {
                rejected.push_back({{"flightId", flightId}, {"reason", "grounded_without_parkingNode"}});
            }

            rec.updatedAt = app::now_sec();
            flights[flightId] = rec;
            ++count;
        }

        app::reply_json(res, 200, {
            {"ok", true},
            {"initialized", count},
            {"warnings", rejected}
        });
    });

    // GET single flight
    svr.Get(R"(/v1/flights/([A-Za-z0-9\-_]+))", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string flightId = req.matches[1];

        int64_t ts = app::now_sec();
        if (req.has_param("ts")) {
            try { ts = std::stoll(req.get_param_value("ts")); } catch (...) { ts = app::now_sec(); }
        }

        std::lock_guard<std::mutex> lk(mtx);
        auto it = flights.find(flightId);
        if (it == flights.end()) {
            app::reply_json(res, 404, {
                {"flightId", flightId},
                {"known", false},
                {"expectedNowOrPast", false},
                {"reason", "unknown_flight"}
            });
            return;
        }

        const auto& rec = it->second;
        const bool expectedNowOrPast = rec.scheduledAt <= ts;

        app::reply_json(res, 200, {
            {"flightId", rec.flightId},
            {"known", true},
            {"scheduledAt", rec.scheduledAt},
            {"status", rec.status},
            {"phase", rec.phase},
            {"parkingNode", rec.parkingNode},
            {"expectedNowOrPast", expectedNowOrPast},
            {"takeoffAllowed", compute_takeoff_allowed(rec)},
            {"updatedAt", rec.updatedAt}
        });
    });

    // Update status (from GroundControl or others)
    // body: { "flightId":"SU100", "status":"ArrivedParking", "reason":"...", "phase":"grounded"?, "parkingNode":"P-5"? }
    svr.Post("/v1/flights/status", [&](const httplib::Request& req, httplib::Response& res) {
        auto bodyOpt = app::parse_json_body(req);
        if (!bodyOpt) {
            app::reply_json(res, 400, {{"error", "invalid json"}});
            return;
        }

        const auto& body = *bodyOpt;
        const std::string flightId = app::s_or(body, "flightId");
        const std::string status = app::s_or(body, "status");
        const std::string reason = app::s_or(body, "reason");

        // новые поля
        const std::string explicitPhase = body.value("phase", std::string(""));
        const std::string parkingNode = body.value("parkingNode", std::string(""));

        if (flightId.empty() || status.empty()) {
            app::reply_json(res, 400, {{"error", "flightId and status required"}});
            return;
        }

        std::lock_guard<std::mutex> lk(mtx);
        auto it = flights.find(flightId);
        if (it == flights.end()) {
            FlightRecord rec;
            rec.flightId = flightId;
            rec.scheduledAt = app::now_sec();
            rec.status = status;
            rec.phase = infer_phase(status, explicitPhase);
            rec.parkingNode = parkingNode;
            rec.updatedAt = app::now_sec();
            flights[flightId] = rec;
        } else {
            it->second.status = status;
            it->second.phase = infer_phase(status, explicitPhase);

            // parkingNode обновляем только если пришло (иначе сохраняем старое)
            if (!parkingNode.empty()) {
                it->second.parkingNode = parkingNode;
            } else {
                // если статус явно "на земле" и parkingNode пуст — оставим старый,
                // но это сигнал, что GC лучше присылать parkingNode для ArrivedParking/Parked.
            }

            // если терминальный статус — parkingNode можно оставить (полезно для истории), но phase станет terminal
            it->second.updatedAt = app::now_sec();
        }

        app::reply_json(res, 200, {
            {"ok", true},
            {"flightId", flightId},
            {"status", status},
            {"phase", flights[flightId].phase},
            {"parkingNode", flights[flightId].parkingNode},
            {"reason", reason}
        });
    });

    // List flights
    // GET /v1/flights?ts=...
    svr.Get("/v1/flights", [&](const httplib::Request& req, httplib::Response& res) {
        int64_t ts = app::now_sec();
        if (req.has_param("ts")) {
            try { ts = std::stoll(req.get_param_value("ts")); } catch (...) { ts = app::now_sec(); }
        }

        json arr = json::array();
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto& [id, rec] : flights) {
            const bool expectedNowOrPast = rec.scheduledAt <= ts;
            arr.push_back({
                {"flightId", rec.flightId},
                {"scheduledAt", rec.scheduledAt},
                {"status", rec.status},
                {"phase", rec.phase},
                {"parkingNode", rec.parkingNode},
                {"expectedNowOrPast", expectedNowOrPast},
                {"takeoffAllowed", compute_takeoff_allowed(rec)},
                {"updatedAt", rec.updatedAt}
            });
        }
        app::reply_json(res, 200, {{"flights", arr}});
    });

    std::cout << "[InformationPanel] listening on 0.0.0.0:" << port << '\n';
    svr.listen("0.0.0.0", port);
    return 0;
}
