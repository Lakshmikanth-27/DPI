#ifndef QUOTA_MANAGER_H
#define QUOTA_MANAGER_H

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>

class QuotaManager {
public:
    struct QuotaRow {
        std::string app;
        uint64_t limit_bytes;
        uint64_t used_bytes;
        bool exceeded;
    };

    // Load quota limits from quota_rules.json.
    // Returns false if file missing or malformed — caller disables quota feature.
    bool loadRules(const std::string& rules_file);

    // Load daily usage from quota_state.json.
    // Resets counters if stored date != today. Silent if file missing.
    void loadState(const std::string& state_file);

    // Persist current usage to quota_state.json. Prints warning on failure.
    void saveState(const std::string& state_file) const;

    // Record bytes for app_name and return true if quota now exceeded.
    // Thread-safe. No-op (returns false) if app has no quota.
    bool recordAndCheck(const std::string& app_name, uint64_t bytes);

    // One row per configured quota, for the report section.
    std::vector<QuotaRow> getReport() const;

    // True after a successful loadRules() call. Safe to call from any thread.
    bool hasQuotas() const { return has_quotas_; }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, uint64_t> limits_;  // app -> limit bytes
    std::unordered_map<std::string, uint64_t> usage_;   // app -> bytes used today
    std::atomic<bool> has_quotas_{false};

    static std::string todayString();                         // "YYYY-MM-DD"
    static uint64_t safeUint64(const std::string& s, size_t pos); // parse without throw
};

#endif // QUOTA_MANAGER_H
