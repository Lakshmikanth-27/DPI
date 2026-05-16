#include "quota_manager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string QuotaManager::todayString() {
    std::time_t t = std::time(nullptr);
    std::tm tm_info{};
    localtime_s(&tm_info, &t);
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_info);
    return std::string(buf);
}

uint64_t QuotaManager::safeUint64(const std::string& s, size_t pos) {
    try { return std::stoull(s.substr(pos)); } catch (...) { return 0; }
}

// ---------------------------------------------------------------------------
// loadRules — parse quota_rules.json
// Expected format:
//   { "quotas": [ { "app": "YouTube", "limit_mb": 500 }, ... ] }
// ---------------------------------------------------------------------------
bool QuotaManager::loadRules(const std::string& rules_file) {
    std::ifstream f(rules_file);
    if (!f.is_open()) {
        std::cerr << "[QuotaManager] Cannot open rules file: " << rules_file << "\n";
        return false;
    }
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    size_t arr_start = json.find('[');
    size_t arr_end   = json.rfind(']');
    if (arr_start == std::string::npos || arr_end == std::string::npos) {
        std::cerr << "[QuotaManager] Missing JSON array in: " << rules_file << "\n";
        return false;
    }

    size_t pos = arr_start;
    while (pos < arr_end) {
        size_t obj_s = json.find('{', pos);
        if (obj_s == std::string::npos || obj_s >= arr_end) break;
        size_t obj_e = json.find('}', obj_s);
        if (obj_e == std::string::npos) break;

        std::string obj = json.substr(obj_s, obj_e - obj_s + 1);

        // Extract "app" string value
        std::string app_name;
        size_t ak = obj.find("\"app\"");
        if (ak != std::string::npos) {
            size_t q1 = obj.find('"', ak + 5);
            if (q1 != std::string::npos) {
                size_t q2 = obj.find('"', q1 + 1);
                if (q2 != std::string::npos)
                    app_name = obj.substr(q1 + 1, q2 - q1 - 1);
            }
        }

        // Extract "limit_mb" integer value
        uint64_t limit_mb = 0;
        size_t lk = obj.find("\"limit_mb\"");
        if (lk != std::string::npos) {
            size_t np = obj.find_first_of("0123456789", lk + 10);
            if (np != std::string::npos)
                limit_mb = safeUint64(obj, np);
        }

        if (!app_name.empty() && limit_mb > 0) {
            limits_[app_name] = limit_mb * 1024ULL * 1024ULL;
            usage_[app_name]  = 0;
        }

        pos = obj_e + 1;
    }

    if (limits_.empty()) {
        std::cerr << "[QuotaManager] No valid quotas in: " << rules_file << "\n";
        return false;
    }

    has_quotas_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// loadState — parse quota_state.json
// Expected format:
//   { "date": "2026-05-16", "usage_bytes": { "YouTube": 52428800, ... } }
// ---------------------------------------------------------------------------
void QuotaManager::loadState(const std::string& state_file) {
    std::ifstream f(state_file);
    if (!f.is_open()) return; // Missing = start fresh, no error

    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Extract stored date
    std::string stored_date;
    size_t dk = json.find("\"date\"");
    if (dk != std::string::npos) {
        size_t q1 = json.find('"', dk + 6);
        if (q1 != std::string::npos) {
            size_t q2 = json.find('"', q1 + 1);
            if (q2 != std::string::npos)
                stored_date = json.substr(q1 + 1, q2 - q1 - 1);
        }
    }

    if (stored_date != todayString()) {
        std::cout << "[QuotaManager] New day — resetting quota counters.\n";
        return; // usage_ already zeroed in loadRules
    }

    // Find usage_bytes object
    size_t ubk = json.find("\"usage_bytes\"");
    if (ubk == std::string::npos) return;
    size_t os = json.find('{', ubk);
    size_t oe = json.rfind('}');
    if (os == std::string::npos || oe == std::string::npos || oe <= os) return;
    // find the closing brace of usage_bytes object (second-to-last '}' in file)
    oe = json.rfind('}', oe - 1);
    if (oe == std::string::npos || oe <= os) return;

    std::string obj = json.substr(os + 1, oe - os - 1);

    // Parse "AppName": bytes pairs
    size_t p = 0;
    while (p < obj.size()) {
        size_t q1 = obj.find('"', p);
        if (q1 == std::string::npos) break;
        size_t q2 = obj.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string app = obj.substr(q1 + 1, q2 - q1 - 1);

        size_t colon = obj.find(':', q2);
        if (colon == std::string::npos) break;
        size_t np = obj.find_first_of("0123456789", colon);
        if (np == std::string::npos) break;
        uint64_t bytes = safeUint64(obj, np);

        if (usage_.count(app))
            usage_[app] = bytes;

        size_t comma = obj.find(',', np);
        p = (comma != std::string::npos) ? comma + 1 : obj.size();
    }

    std::cout << "[QuotaManager] Loaded quota state from " << state_file << "\n";
}

// ---------------------------------------------------------------------------
// saveState — write quota_state.json
// ---------------------------------------------------------------------------
void QuotaManager::saveState(const std::string& state_file) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream f(state_file);
    if (!f.is_open()) {
        std::cerr << "[QuotaManager] Cannot write state: " << state_file << "\n";
        return;
    }
    f << "{\n  \"date\": \"" << todayString() << "\",\n  \"usage_bytes\": {\n";
    bool first = true;
    for (const auto& [app, bytes] : usage_) {
        if (!first) f << ",\n";
        f << "    \"" << app << "\": " << bytes;
        first = false;
    }
    f << "\n  }\n}\n";
    std::cout << "[QuotaManager] Saved quota state to " << state_file << "\n";
}

// ---------------------------------------------------------------------------
// recordAndCheck — called per-packet from FastPath threads
// ---------------------------------------------------------------------------
bool QuotaManager::recordAndCheck(const std::string& app_name, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto lit = limits_.find(app_name);
    if (lit == limits_.end()) return false;
    usage_[app_name] += bytes;
    return usage_[app_name] > lit->second;
}

// ---------------------------------------------------------------------------
// getReport — called once at end from main thread
// ---------------------------------------------------------------------------
std::vector<QuotaManager::QuotaRow> QuotaManager::getReport() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QuotaRow> rows;
    for (const auto& [app, limit] : limits_) {
        uint64_t used = 0;
        auto it = usage_.find(app);
        if (it != usage_.end()) used = it->second;
        rows.push_back({app, limit, used, used > limit});
    }
    std::sort(rows.begin(), rows.end(),
              [](const QuotaRow& a, const QuotaRow& b) { return a.app < b.app; });
    return rows;
}
