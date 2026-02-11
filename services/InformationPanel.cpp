#include <unordered_map>
#include <mutex>
#include <vector>
#include <iostream>

#include "common.h"

using app::json;

struct FlightRecord {
    std::string flightId;
    int64_t scheduledAt = 0; // epoch seconds
    std::string status = "Scheduled";
    int64_t updatedAt = 0;
};

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

        std::lock_guard<std::mutex> lk(mtx);
        int count = 0;
        for (const auto& f : body["flights"]) {
            const std::string flightId = app::s_or(f, "flightId");
            if (flightId.empty()) {
                continue;
            }

            FlightRecord rec;
            rec.flightId = flightId;
            rec.scheduledAt = f.value("scheduledAt", app::now_sec());
            rec.status = f.value("status", std::string("Scheduled"));
            rec.updatedAt = app::now_sec();

            flights[flightId] = rec;
            ++count;
        }

        app::reply_json(res, 200, {
            {"ok", true},
            {"initialized", count}
        });
    });

    // Query single flight
    svr.Get(R"(/v1/flights/([A-Za-z0-9\-_]+))", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string flightId = req.matches[1];

        int64_t ts = app::now_sec();
        if (req.has_param("ts")) {
            try {
                ts = std::stoll(req.get_param_value("ts"));
            } catch (...) {
                ts = app::now_sec();
            }
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
            {"expectedNowOrPast", expectedNowOrPast},
            {"updatedAt", rec.updatedAt}
        });
    });

    // Update status from GroundControl
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
            rec.updatedAt = app::now_sec();
            flights[flightId] = rec;
        } else {
            it->second.status = status;
            it->second.updatedAt = app::now_sec();
        }

        app::reply_json(res, 200, {
            {"ok", true},
            {"flightId", flightId},
            {"status", status},
            {"reason", reason}
        });
    });

    svr.Get("/v1/flights", [&](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto& [id, rec] : flights) {
            arr.push_back({
                {"flightId", rec.flightId},
                {"scheduledAt", rec.scheduledAt},
                {"status", rec.status},
                {"updatedAt", rec.updatedAt}
            });
        }
        app::reply_json(res, 200, {{"flights", arr}});
    });

    std::cout << "[InformationPanel] listening on 0.0.0.0:" << port << '\n';
    svr.listen("0.0.0.0", port);
    return 0;
}