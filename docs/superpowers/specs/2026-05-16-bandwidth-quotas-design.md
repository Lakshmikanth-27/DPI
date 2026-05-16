# Bandwidth Quotas Implementation Design

**Date:** 2026-05-16
**Status:** Approved

## Goal

Add persistent daily bandwidth quotas to the DPI engine. When an app exceeds its configured data limit, the engine blocks it mid-stream for the rest of that day's processing. Quota usage accumulates across multiple runs and resets automatically at midnight.

## Architecture

```
quota_rules.json          quota_state.json
(user edits this)         (engine writes this)
       |                         |
       +----------+  +-----------+
                  |  |
            [ QuotaManager ]
                  |
         reads/writes both files on startup/exit
                  |
           DPIEngine (dpi_mt.cpp)
                  |
            FastPath threads
                  |
    recordAndCheck(app, bytes) → block if exceeded
```

Three components:
- **`QuotaManager`** — new class. Owns file I/O, byte accumulation, daily reset, quota enforcement.
- **`dpi_mt.cpp`** — DPIEngine gets a `QuotaManager` member. FastPath calls it alongside existing `Rules`.
- **JSON parser** — custom minimal parser (no external deps). Both files are fixed-schema and small.

## File Formats

### `quota_rules.json` — user-editable quota config

```json
{
  "quotas": [
    { "app": "YouTube",  "limit_mb": 500 },
    { "app": "TikTok",   "limit_mb": 100 },
    { "app": "Facebook", "limit_mb": 200 }
  ]
}
```

- `app` must match an app name returned by `appTypeToString()` (e.g. "YouTube", "TikTok", "Facebook")
- `limit_mb` is an integer, megabytes (1 MB = 1,048,576 bytes)

### `quota_state.json` — machine-written usage tracking

```json
{
  "date": "2026-05-16",
  "usage_bytes": {
    "YouTube":  52428800,
    "TikTok":   1048576,
    "Facebook": 0
  }
}
```

- `date` format: `YYYY-MM-DD` (local date)
- `usage_bytes` maps app name → cumulative bytes transferred today
- Engine writes this file on every successful run completion

### Daily Reset Logic

On startup:
1. Load `quota_state.json` (if it exists)
2. Compare stored `date` to today's local date
3. If different → zero all `usage_bytes`, set `date` to today
4. If same → carry forward existing usage

## New Files

| File | Purpose |
|------|---------|
| `include/quota_manager.h` | QuotaManager class declaration |
| `src/quota_manager.cpp` | QuotaManager implementation |
| `quota_rules.json` | Example quota config (committed to repo) |

## Modified Files

| File | Change |
|------|--------|
| `src/dpi_mt.cpp` | Add QuotaManager member to DPIEngine; call `recordAndCheck()` in FastPath; add quota section to `printReport()`; add `--quota-rules` CLI arg |

## QuotaManager Interface

```cpp
class QuotaManager {
public:
    // Load quota_rules.json. Returns false if file missing or malformed.
    bool loadRules(const std::string& rules_file);

    // Load quota_state.json. Resets counters if date has changed. No-op if missing.
    void loadState(const std::string& state_file);

    // Save current usage to quota_state.json.
    void saveState(const std::string& state_file) const;

    // Record bytes for an app and check if quota exceeded.
    // Thread-safe. Returns true if this app is now over quota.
    bool recordAndCheck(const std::string& app_name, uint64_t bytes);

    // Check quota without recording (used for already-exceeded flows).
    bool isExceeded(const std::string& app_name) const;

    // Returns quota report rows: {app_name, limit_bytes, used_bytes, exceeded}
    struct QuotaRow { std::string app; uint64_t limit; uint64_t used; bool exceeded; };
    std::vector<QuotaRow> getReport() const;

    bool hasQuotas() const;
};
```

## CLI

```powershell
.\dpi_engine.exe input.pcap output.pcap --quota-rules quota_rules.json
```

- `--quota-rules <file>` — path to `quota_rules.json`. State file is always `quota_state.json` in the same directory as the rules file.
- If `--quota-rules` is omitted, quota feature is disabled (no behavior change).

## Enforcement

In `FastPath::run()`, after classifying the flow:

```
if quota enabled AND flow.app_type is quota'd:
    exceeded = quota_manager->recordAndCheck(app_name, packet_bytes)
    if exceeded:
        flow.blocked = true   (same path as rules-based blocking)
        stats.dropped++
```

`recordAndCheck` is atomic per-app using a mutex — same pattern as existing `Rules` class.

## Report Output

Appended to existing `printReport()` when `--quota-rules` is active:

```
+----------------------------------------------------------------+
|                     BANDWIDTH QUOTAS                           |
+----------------------------------------------------------------+
| YouTube       500.0 MB limit   487.2 MB used   [ OK      ]    |
| TikTok        100.0 MB limit   103.4 MB used   [EXCEEDED ]    |
| Facebook      200.0 MB limit     0.0 MB used   [ OK      ]    |
+----------------------------------------------------------------+
```

## Error Handling

- `quota_rules.json` missing → print warning, disable quota feature, continue
- `quota_rules.json` malformed → print error with line hint, disable quota feature, continue
- `quota_state.json` missing → start fresh (all usage = 0), no error
- `quota_state.json` malformed → start fresh, print warning
- Unknown app name in rules → print warning, skip that rule
- Write failure on exit → print warning, do not crash

## Assumptions

- App detection accuracy depends on SNI/HTTP host extraction (existing behavior)
- "Bytes" counted = raw packet bytes (Ethernet frame size), not just payload
- Quota enforcement is best-effort: a flow already in progress when quota is hit finishes its current packet before being blocked
- State file is local to the machine; no network sync
