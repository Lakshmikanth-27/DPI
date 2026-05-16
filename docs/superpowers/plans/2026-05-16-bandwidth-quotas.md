# Bandwidth Quotas Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add persistent daily bandwidth quotas — track bytes per app across runs, auto-block when limit exceeded, reset at midnight.

**Architecture:** A new `QuotaManager` class (header + cpp) owns two JSON files: `quota_rules.json` (user-edited limits) and `quota_state.json` (machine-written daily usage). DPIEngine loads both on startup, FastPath threads call `recordAndCheck()` after each packet is classified, and DPIEngine saves updated state on exit.

**Tech Stack:** C++17, standard library only (no external JSON lib — custom fixed-schema parser), MinGW g++ with `-static`

---

### Task 1: QuotaManager class

**Files:**
- Create: `include/quota_manager.h`
- Create: `src/quota_manager.cpp`

- [ ] **Step 1: Create `include/quota_manager.h`**

```cpp
#ifndef QUOTA_MANAGER_H
#define QUOTA_MANAGER_H

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
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
    bool has_quotas_ = false;

    static std::string todayString();                         // "YYYY-MM-DD"
    static uint64_t safeUint64(const std::string& s, size_t pos); // parse without throw
};

#endif // QUOTA_MANAGER_H
```

- [ ] **Step 2: Create `src/quota_manager.cpp`**

```cpp
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
    std::tm* tm_info = std::localtime(&t);
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm_info);
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
    size_t oe = json.find('}', os);
    if (os == std::string::npos || oe == std::string::npos) return;

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
```

- [ ] **Step 3: Compile quota_manager.cpp in isolation to verify no errors**

Run from `Packet_analyzer/` directory (with MSYS2 ucrt64 on PATH):
```powershell
$env:Path = "C:\msys64\ucrt64\bin;" + $env:Path
g++ -std=c++17 -Wall -Wextra -I include -c src/quota_manager.cpp -o quota_manager.o
```

Expected: no output, exit 0, `quota_manager.o` created.  
If errors appear, fix them before proceeding.

- [ ] **Step 4: Commit**

```powershell
git add include/quota_manager.h src/quota_manager.cpp
git commit -m "feat: add QuotaManager class for daily bandwidth quota tracking"
```

---

### Task 2: Integrate QuotaManager into dpi_mt.cpp

**Files:**
- Modify: `src/dpi_mt.cpp`

Context: `dpi_mt.cpp` is one self-contained file with all engine code. It has:
- `struct Packet` (line ~83) — packet data
- `class FastPath` (line ~191) — per-thread packet processor, has `Rules* rules_` member
- `class DPIEngine` (line ~354) — orchestrator, has `Config`, `Rules rules_`, `Stats stats_`, constructor, `process()`, `printReport()`
- `int main()` (line ~609) — CLI parsing

Make these changes in order:

- [ ] **Step 5: Add `#include "quota_manager.h"` at the top of `src/dpi_mt.cpp`**

Add after the existing includes (after `#include "types.h"`):
```cpp
#include "quota_manager.h"
```

- [ ] **Step 6: Add `quota_rules_file` to `DPIEngine::Config`**

Find this block (around line 356):
```cpp
    struct Config {
        int num_lbs = 2;
        int fps_per_lb = 2;
    };
```

Replace with:
```cpp
    struct Config {
        int num_lbs = 2;
        int fps_per_lb = 2;
        std::string quota_rules_file;  // empty = quota feature disabled
    };
```

- [ ] **Step 7: Add `quota_manager_` member and `stateFilePath()` helper to `DPIEngine`**

Find the private section of `DPIEngine` (around line 520):
```cpp
    Config config_;
    Rules rules_;
    Stats stats_;
    TSQueue<Packet> output_queue_;
```

Replace with:
```cpp
    Config config_;
    Rules rules_;
    Stats stats_;
    QuotaManager quota_manager_;
    TSQueue<Packet> output_queue_;

    static std::string stateFilePath(const std::string& rules_file) {
        size_t sep = rules_file.find_last_of("/\\");
        if (sep != std::string::npos)
            return rules_file.substr(0, sep + 1) + "quota_state.json";
        return "quota_state.json";
    }
```

- [ ] **Step 8: Add `QuotaManager*` parameter to `FastPath`**

Find the `FastPath` constructor (around line 196):
```cpp
    FastPath(int id, Rules* rules, Stats* stats, TSQueue<Packet>* output_queue)
        : id_(id), rules_(rules), stats_(stats), output_queue_(output_queue) {}
```

Replace with:
```cpp
    FastPath(int id, Rules* rules, Stats* stats, TSQueue<Packet>* output_queue, QuotaManager* quota)
        : id_(id), rules_(rules), stats_(stats), output_queue_(output_queue), quota_(quota) {}
```

Find the `FastPath` private members (around line 215):
```cpp
    int id_;
    Rules* rules_;
    Stats* stats_;
    TSQueue<Packet>* output_queue_;
    TSQueue<Packet> input_queue_;
```

Replace with:
```cpp
    int id_;
    Rules* rules_;
    Stats* stats_;
    TSQueue<Packet>* output_queue_;
    QuotaManager* quota_;
    TSQueue<Packet> input_queue_;
```

- [ ] **Step 9: Add quota check inside `FastPath::run()`**

Find this block inside `FastPath::run()` (around line 245):
```cpp
            // Check blocking
            if (!flow.blocked) {
                flow.blocked = rules_->isBlocked(pkt.tuple.src_ip, flow.app_type, flow.sni);
            }
```

Replace with:
```cpp
            // Check blocking
            if (!flow.blocked) {
                flow.blocked = rules_->isBlocked(pkt.tuple.src_ip, flow.app_type, flow.sni);
            }

            // Check quota — only for classified flows with a known app
            if (!flow.blocked && quota_ && quota_->hasQuotas() && flow.classified) {
                flow.blocked = quota_->recordAndCheck(
                    appTypeToString(flow.app_type), pkt.data.size());
            }
```

- [ ] **Step 10: Pass `&quota_manager_` when constructing FastPath threads**

Find this line in `DPIEngine` constructor (around line 374):
```cpp
            fps_.push_back(std::make_unique<FastPath>(i, &rules_, &stats_, &output_queue_));
```

Replace with:
```cpp
            fps_.push_back(std::make_unique<FastPath>(i, &rules_, &stats_, &output_queue_, &quota_manager_));
```

- [ ] **Step 11: Load quota rules and state in `DPIEngine::process()`**

Find this line in `DPIEngine::process()` (around line 393):
```cpp
        // Open input
        PcapReader reader;
        if (!reader.open(input_file)) return false;
```

Add quota loading immediately after:
```cpp
        // Open input
        PcapReader reader;
        if (!reader.open(input_file)) return false;

        // Load quota rules and state (if configured)
        if (!config_.quota_rules_file.empty()) {
            if (quota_manager_.loadRules(config_.quota_rules_file)) {
                quota_manager_.loadState(stateFilePath(config_.quota_rules_file));
            }
        }
```

- [ ] **Step 12: Save quota state at the end of `DPIEngine::process()`**

Find this line near the end of `DPIEngine::process()` (after `printReport()`):
```cpp
        printReport();
        
        return true;
```

Replace with:
```cpp
        printReport();

        // Save quota state for next run
        if (!config_.quota_rules_file.empty() && quota_manager_.hasQuotas()) {
            quota_manager_.saveState(stateFilePath(config_.quota_rules_file));
        }

        return true;
```

- [ ] **Step 13: Add quota report section to `printReport()`**

Find the end of `printReport()` (after the detected SNIs block, before closing `}`):
```cpp
        // Detected SNIs
        if (!stats_.detected_snis.empty()) {
            std::cout << "\n[Detected Domains/SNIs]\n";
            for (const auto& [sni, app] : stats_.detected_snis) {
                std::cout << "  - " << sni << " -> " << appTypeToString(app) << "\n";
            }
        }
    }
```

Replace with:
```cpp
        // Detected SNIs
        if (!stats_.detected_snis.empty()) {
            std::cout << "\n[Detected Domains/SNIs]\n";
            for (const auto& [sni, app] : stats_.detected_snis) {
                std::cout << "  - " << sni << " -> " << appTypeToString(app) << "\n";
            }
        }

        // Quota report
        if (quota_manager_.hasQuotas()) {
            std::cout << "\n+----------------------------------------------------------------+\n";
            std::cout << "|                     BANDWIDTH QUOTAS                          |\n";
            std::cout << "+----------------------------------------------------------------+\n";
            for (const auto& row : quota_manager_.getReport()) {
                double used_mb  = row.used_bytes  / (1024.0 * 1024.0);
                double limit_mb = row.limit_bytes / (1024.0 * 1024.0);
                std::string status = row.exceeded ? "[EXCEEDED]" : "[ OK     ]";
                std::cout << "| " << std::setw(15) << std::left  << row.app
                          << std::setw(7)  << std::right << std::fixed << std::setprecision(1) << limit_mb << " MB limit"
                          << std::setw(8)  << used_mb << " MB used"
                          << "  " << status << " |\n";
            }
            std::cout << "+----------------------------------------------------------------+\n";
        }
    }
```

- [ ] **Step 14: Add `--quota-rules` CLI arg to `main()`**

Find the option parsing block in `main()` (around line 621):
```cpp
        else if (arg == "--lbs" && i + 1 < argc) cfg.num_lbs = std::stoi(argv[++i]);
        else if (arg == "--fps" && i + 1 < argc) cfg.fps_per_lb = std::stoi(argv[++i]);
```

Add after:
```cpp
        else if (arg == "--lbs" && i + 1 < argc) cfg.num_lbs = std::stoi(argv[++i]);
        else if (arg == "--fps" && i + 1 < argc) cfg.fps_per_lb = std::stoi(argv[++i]);
        else if (arg == "--quota-rules" && i + 1 < argc) cfg.quota_rules_file = argv[++i];
```

- [ ] **Step 15: Build the full project**

```powershell
$env:Path = "C:\msys64\ucrt64\bin;" + $env:Path
cd "d:\projects\DPI\Packet_analyzer"
g++ -std=c++17 -pthread -O2 -Wall -Wextra -static -I include -o dpi_engine.exe `
    src/dpi_mt.cpp `
    src/pcap_reader.cpp `
    src/packet_parser.cpp `
    src/sni_extractor.cpp `
    src/types.cpp `
    src/quota_manager.cpp
```

Expected: exit 0, `dpi_engine.exe` updated. Pre-existing warnings about `payload_length` and unused vars in `sni_extractor.cpp` are fine — no new errors.

- [ ] **Step 16: Commit**

```powershell
git add src/dpi_mt.cpp
git commit -m "feat: integrate QuotaManager into DPIEngine and FastPath"
```

---

### Task 3: Example config + end-to-end verification

**Files:**
- Create: `quota_rules.json`

- [ ] **Step 17: Create `quota_rules.json`**

```json
{
  "quotas": [
    { "app": "YouTube",  "limit_mb": 1 },
    { "app": "TikTok",   "limit_mb": 1 },
    { "app": "Facebook", "limit_mb": 1 }
  ]
}
```

Note: limits set to 1 MB so `test_dpi.pcap` (5738 bytes total) won't trigger them. We'll test the exceeded path by setting a tiny limit (1 byte) in the next step.

- [ ] **Step 18: Run without `--quota-rules` — verify existing behavior unchanged**

```powershell
.\dpi_engine.exe test_dpi.pcap output.pcap
```

Expected: normal report prints, **no quota section**, no mention of quota_state.json. Exit 0.

- [ ] **Step 19: Run with `--quota-rules` at 1 MB — verify quota loads and state saves**

```powershell
.\dpi_engine.exe test_dpi.pcap output.pcap --quota-rules quota_rules.json
```

Expected output includes:
```
+----------------------------------------------------------------+
|                     BANDWIDTH QUOTAS                          |
+----------------------------------------------------------------+
| Facebook       1.0 MB limit     0.0 MB used  [ OK     ]      |
| TikTok         1.0 MB limit     0.0 MB used  [ OK     ]      |
| YouTube        1.0 MB limit     0.0 MB used  [ OK     ]      |
+----------------------------------------------------------------+
```
And `quota_state.json` created in project directory. Verify it exists:
```powershell
Get-Content quota_state.json
```

- [ ] **Step 20: Test exceeded path — set YouTube limit to 0 bytes**

Edit `quota_rules.json` temporarily:
```json
{
  "quotas": [
    { "app": "YouTube",  "limit_mb": 0 },
    { "app": "TikTok",   "limit_mb": 1 },
    { "app": "Facebook", "limit_mb": 1 }
  ]
}
```

Wait — `limit_mb: 0` is filtered out by the parser (`limit_mb > 0` check). Instead, delete `quota_state.json`, then set YouTube to a tiny limit that the test PCAP will exceed. The test PCAP has 1 YouTube packet (~100 bytes payload). Set limit to 0.0001 MB won't work (integer). 

Use this approach instead — run twice. First run accumulates usage. Edit `quota_rules.json` to a limit smaller than recorded usage, delete `quota_state.json`, run again. But that resets. 

Simpler: set limit_mb to 1 (= 1,048,576 bytes) — test_dpi.pcap is only 5738 bytes total so YouTube's share won't hit 1 MB. 

To force EXCEEDED, manually create a `quota_state.json` with YouTube already over quota:

```powershell
Set-Content quota_state.json '{"date":"2026-05-16","usage_bytes":{"YouTube":2097152,"TikTok":0,"Facebook":0}}'
```
(Replace `2026-05-16` with today's actual date from `Get-Date -Format "yyyy-MM-dd"`)

Then run:
```powershell
.\dpi_engine.exe test_dpi.pcap output.pcap --quota-rules quota_rules.json
```

Expected quota section shows YouTube as EXCEEDED and Dropped count increases:
```
| YouTube        1.0 MB limit  2048.0 MB used  [EXCEEDED]      |
```
And the Processing Report shows `Dropped: 1` (the YouTube packet is blocked).

- [ ] **Step 21: Restore `quota_rules.json` to sensible defaults and delete test state**

```powershell
Remove-Item quota_state.json -ErrorAction SilentlyContinue
```

Restore `quota_rules.json`:
```json
{
  "quotas": [
    { "app": "YouTube",  "limit_mb": 500 },
    { "app": "TikTok",   "limit_mb": 100 },
    { "app": "Facebook", "limit_mb": 200 }
  ]
}
```

- [ ] **Step 22: Final build and smoke test**

```powershell
.\dpi_engine.exe test_dpi.pcap output.pcap --quota-rules quota_rules.json
```

Expected: clean run, quota section shows all `[ OK ]`, `quota_state.json` written.

- [ ] **Step 23: Commit and push**

```powershell
git add quota_rules.json
git commit -m "feat: add example quota_rules.json and complete bandwidth quota feature"
git push origin main
```
