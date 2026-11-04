#include "config/time_config.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <set>

namespace tbox {
namespace time {

namespace {

bool parseBool(const YAML::Node& n, bool* out, std::string* error) {
    try {
        *out = n.as<bool>();
        return true;
    } catch (const std::exception&) {
        if (error) *error = "expected bool";
        return false;
    }
}

bool parseInt64(const YAML::Node& n, int64_t* out, std::string* error) {
    try {
        *out = n.as<int64_t>();
        return true;
    } catch (const std::exception&) {
        if (error) *error = "expected integer";
        return false;
    }
}

bool parseUint16(const YAML::Node& n, uint16_t* out, std::string* error) {
    int64_t v;
    if (!parseInt64(n, &v, error)) return false;
    if (v < 0 || v > 65535) {
        if (error) *error = "port out of range";
        return false;
    }
    *out = static_cast<uint16_t>(v);
    return true;
}

}  // namespace

bool validateTimeConfig(const TimeConfig& cfg, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };

    // profile 约束：production 禁止 mock 后端（AT-12 fake 隔离）
    if (cfg.profile == RunProfile::kProduction &&
        cfg.backend_kind == BackendKind::kMock) {
        return fail("production profile forbids mock backend");
    }

    // 后端
    if (cfg.backend_kind == BackendKind::kChronyObserve) {
        return fail("chrony observe backend is a registered extension point, "
                    "not implemented (see docs/implementation_notes.md)");
    }

    // 核心范围
    const TimeCoreConfig& c = cfg.core;
    if (c.max_source_age_ms <= 0) return fail("core.max_source_age_ms must be > 0");
    if (c.max_source_error_ms <= 0) return fail("core.max_source_error_ms must be > 0");
    if (c.step_threshold_ms <= 0) return fail("core.step_threshold_ms must be > 0");
    if (c.slew_threshold_ms < 0 || c.slew_threshold_ms > c.step_threshold_ms)
        return fail("core.slew_threshold_ms must be in [0, step_threshold_ms]");
    if (c.alignment_tolerance_ms < 0)
        return fail("core.alignment_tolerance_ms must be >= 0");
    if (c.holdover_max_ms <= 0) return fail("core.holdover_max_ms must be > 0");
    if (c.holdover_max_error_ms <= 0)
        return fail("core.holdover_max_error_ms must be > 0");
    if (c.drift_bound_ppm < 0 || c.drift_bound_ppm > 1000000)
        return fail("core.drift_bound_ppm out of range");
    if (c.stability_window_syncs < 1)
        return fail("core.stability_window_syncs must be >= 1");
    if (c.switch_hysteresis_ms < 0)
        return fail("core.switch_hysteresis_ms must be >= 0");
    if (c.monitor_interval_ms <= 0) return fail("core.monitor_interval_ms must be > 0");
    if (cfg.event_queue_limit <= 0) return fail("event_queue_limit must be > 0");
    if (cfg.ipc_socket_path.empty()) return fail("ipc_socket_path must not be empty");

    // 来源
    std::set<std::string> ids;
    for (const auto& ntp : cfg.ntp_servers) {
        if (ntp.host.empty()) return fail("ntp server host must not be empty");
        if (ntp.timeout_ms <= 0) return fail("ntp timeout_ms must be > 0");
        if (ntp.poll_interval_ms <= 0) return fail("ntp poll_interval_ms must be > 0");
        if (!ids.insert(ntp.source_id).second)
            return fail("duplicate source_id: " + ntp.source_id);
        if (ntp.auth_mode != "none") {
            return fail("ntp auth_mode '" + ntp.auth_mode +
                        "' is a registered extension point, not implemented "
                        "(see docs/implementation_notes.md)");
        }
        if (ntp.require_authenticated) {
            return fail("ntp require_authenticated=true requires auth_mode "
                        "implemented (extension point)");
        }
    }
    for (const auto& id : {cfg.cellular.source_id, cfg.gnss.source_id,
                           cfg.rtc.source_id}) {
        if (!ids.insert(id).second) return fail("duplicate source_id: " + id);
    }
    return true;
}

bool loadTimeConfig(const std::string& yaml_text, TimeConfig* out,
                    std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };

    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const std::exception& e) {
        return fail(std::string("yaml parse error: ") + e.what());
    }
    if (!root || !root.IsMap()) return fail("root must be a map");

    TimeConfig cfg;
    const YAML::Node& time = root["time"];
    if (!time || !time.IsMap()) return fail("missing top-level 'time' map");

    // --- profile ---
    if (time["profile"]) {
        const std::string p = time["profile"].as<std::string>("");
        if (p == "production") cfg.profile = RunProfile::kProduction;
        else if (p == "test") cfg.profile = RunProfile::kTest;
        else return fail("profile must be 'production' or 'test'");
    }

    // --- 后端 ---
    const YAML::Node& backend = time["backend"];
    if (backend && backend.IsMap()) {
        if (backend["mode"]) {
            const std::string m = backend["mode"].as<std::string>("");
            if (m == "linux") cfg.backend_kind = BackendKind::kLinuxSystemClock;
            else if (m == "mock") cfg.backend_kind = BackendKind::kMock;
            else if (m == "chrony") cfg.backend_kind = BackendKind::kChronyObserve;
            else return fail("backend.mode must be 'linux' | 'mock' | 'chrony'");
        }
        if (backend["allow_competing_writers"]) {
            if (!parseBool(backend["allow_competing_writers"],
                           &cfg.linux_backend.allow_competing_writers, error))
                return fail("backend.allow_competing_writers " + *error);
        }
        if (backend["competing_processes"]) {
            cfg.linux_backend.competing_processes =
                backend["competing_processes"].as<std::string>();
        }
    }

    // --- 核心 ---
    const YAML::Node& core = time["core"];
    if (core && core.IsMap()) {
        auto set_i64 = [&](const char* key, auto* dst) {
            if (core[key]) {
                int64_t v;
                if (!parseInt64(core[key], &v, error)) {
                    *error = std::string("core.") + key + " " + *error;
                    return false;
                }
                *dst = static_cast<std::remove_reference_t<decltype(*dst)>>(v);
            }
            return true;
        };
        auto set_bool = [&](const char* key, bool* dst) {
            if (core[key]) {
                if (!parseBool(core[key], dst, error)) {
                    *error = std::string("core.") + key + " " + *error;
                    return false;
                }
            }
            return true;
        };
        if (!set_i64("max_source_age_ms", &cfg.core.max_source_age_ms)) return false;
        if (!set_i64("max_source_error_ms", &cfg.core.max_source_error_ms)) return false;
        if (!set_bool("require_authenticated_sources",
                      &cfg.core.require_authenticated_sources))
            return false;
        if (!set_i64("stability_window_syncs", &cfg.core.stability_window_syncs))
            return false;
        if (!set_i64("switch_hysteresis_ms", &cfg.core.switch_hysteresis_ms))
            return false;
        if (!set_i64("step_threshold_ms", &cfg.core.step_threshold_ms)) return false;
        if (!set_i64("slew_threshold_ms", &cfg.core.slew_threshold_ms)) return false;
        if (!set_i64("alignment_tolerance_ms", &cfg.core.alignment_tolerance_ms))
            return false;
        if (!set_i64("calibration_retry_limit", &cfg.core.calibration_retry_limit))
            return false;
        if (!set_i64("holdover_max_ms", &cfg.core.holdover_max_ms)) return false;
        if (!set_i64("holdover_max_error_ms", &cfg.core.holdover_max_error_ms))
            return false;
        if (!set_i64("drift_bound_ppm", &cfg.core.drift_bound_ppm)) return false;
        if (!set_i64("monitor_interval_ms", &cfg.core.monitor_interval_ms))
            return false;
        if (!set_i64("external_change_threshold_ms",
                     &cfg.core.external_change_threshold_ms))
            return false;
        if (!set_i64("fault_probe_interval_ms", &cfg.core.fault_probe_interval_ms))
            return false;
        if (!set_i64("synchronizing_timeout_ms", &cfg.core.synchronizing_timeout_ms))
            return false;
        if (!set_i64("source_lost_age_ms", &cfg.core.source_lost_age_ms))
            return false;
        if (!set_i64("holdover_enter_min_age_ms",
                     &cfg.core.holdover_enter_min_age_ms))
            return false;
        if (!set_bool("holdover_enabled", &cfg.core.holdover_enabled)) return false;
        if (!set_i64("poll_interval_ms", &cfg.core.poll_interval_ms)) return false;
    }

    // --- IPC ---
    if (time["ipc"]) {
        const YAML::Node& ipc = time["ipc"];
        if (ipc["socket_path"]) cfg.ipc_socket_path = ipc["socket_path"].as<std::string>();
        if (ipc["event_queue_limit"]) {
            int64_t v;
            if (!parseInt64(ipc["event_queue_limit"], &v, error))
                return fail("ipc.event_queue_limit " + *error);
            cfg.event_queue_limit = v;
        }
    }
    if (time["scheduler_interval_ms"]) {
        int64_t v;
        if (!parseInt64(time["scheduler_interval_ms"], &v, error))
            return fail("scheduler_interval_ms " + *error);
        cfg.scheduler_interval_ms = v;
    }
    if (time["policy_version"]) {
        cfg.policy_version = time["policy_version"].as<std::string>();
    }

    // --- 来源 ---
    const YAML::Node& sources = time["sources"];
    if (sources && sources.IsMap()) {
        const YAML::Node& ntp = sources["ntp"];
        if (ntp && ntp.IsMap()) {
            bool enabled = true;
            if (ntp["enabled"] && !parseBool(ntp["enabled"], &enabled, error))
                return fail("sources.ntp.enabled " + *error);
            if (enabled) {
                const YAML::Node& servers = ntp["servers"];
                if (servers && servers.IsSequence()) {
                    for (const auto& s : servers) {
                        NtpSourceConfig n;
                        n.source_id = s["id"] ? s["id"].as<std::string>()
                                              : ("ntp" + std::to_string(
                                                     cfg.ntp_servers.size()));
                        n.host = s["host"] ? s["host"].as<std::string>() : "";
                        if (s["priority"]) {
                            int64_t p;
                            if (!parseInt64(s["priority"], &p, error))
                                return fail("sources.ntp.servers[].priority " + *error);
                            n.priority = static_cast<int>(p);
                        }
                        if (s["port"]) {
                            if (!parseUint16(s["port"], &n.port, error))
                                return fail("sources.ntp.servers[].port " + *error);
                        }
                        if (s["timeout_ms"]) {
                            if (!parseInt64(s["timeout_ms"], &n.timeout_ms, error))
                                return fail("sources.ntp.servers[].timeout_ms " + *error);
                        }
                        if (s["poll_interval_ms"]) {
                            if (!parseInt64(s["poll_interval_ms"],
                                            &n.poll_interval_ms, error))
                                return fail("sources.ntp.servers[].poll_interval_ms " + *error);
                        }
                        if (s["queueing_budget_ms"]) {
                            if (!parseInt64(s["queueing_budget_ms"],
                                            &n.queueing_budget_ms, error))
                                return fail("sources.ntp.servers[].queueing_budget_ms " + *error);
                        }
                        if (s["require_authenticated"]) {
                            if (!parseBool(s["require_authenticated"],
                                           &n.require_authenticated, error))
                                return fail("sources.ntp.servers[].require_authenticated " + *error);
                        }
                        if (s["auth_mode"]) {
                            n.auth_mode = s["auth_mode"].as<std::string>("none");
                        }
                        if (s["drift_upper_ppm"]) {
                            if (!parseInt64(s["drift_upper_ppm"],
                                            &n.drift_upper_ppm, error))
                                return fail("sources.ntp.servers[].drift_upper_ppm " + *error);
                        }
                        cfg.ntp_servers.push_back(n);
                    }
                }
            }
        }

        const YAML::Node& cell = sources["cellular"];
        if (cell && cell.IsMap()) {
            if (cell["enabled"]) {
                if (!parseBool(cell["enabled"], &cfg.cellular.enabled, error))
                    return fail("sources.cellular.enabled " + *error);
            }
            if (cell["source_id"]) cfg.cellular.source_id = cell["source_id"].as<std::string>();
            if (cell["device"]) cfg.cellular.device = cell["device"].as<std::string>();
            if (cell["priority"]) {
                int64_t v;
                if (!parseInt64(cell["priority"], &v, error))
                    return fail("sources.cellular.priority " + *error);
                cfg.cellular.priority = static_cast<int>(v);
            }
        }

        const YAML::Node& gnssn = sources["gnss"];
        if (gnssn && gnssn.IsMap()) {
            if (gnssn["enabled"]) {
                if (!parseBool(gnssn["enabled"], &cfg.gnss.enabled, error))
                    return fail("sources.gnss.enabled " + *error);
            }
            if (gnssn["source_id"]) cfg.gnss.source_id = gnssn["source_id"].as<std::string>();
            if (gnssn["device"]) cfg.gnss.device = gnssn["device"].as<std::string>();
            if (gnssn["priority"]) {
                int64_t v;
                if (!parseInt64(gnssn["priority"], &v, error))
                    return fail("sources.gnss.priority " + *error);
                cfg.gnss.priority = static_cast<int>(v);
            }
        }

        const YAML::Node& rtc = sources["rtc"];
        if (rtc && rtc.IsMap()) {
            if (rtc["enabled"]) {
                if (!parseBool(rtc["enabled"], &cfg.rtc.enabled, error))
                    return fail("sources.rtc.enabled " + *error);
            }
            if (rtc["source_id"]) cfg.rtc.source_id = rtc["source_id"].as<std::string>();
            if (rtc["device"]) cfg.rtc.device = rtc["device"].as<std::string>();
            if (rtc["priority"]) {
                int64_t v;
                if (!parseInt64(rtc["priority"], &v, error))
                    return fail("sources.rtc.priority " + *error);
                cfg.rtc.priority = static_cast<int>(v);
            }
            if (rtc["require_provisioned"]) {
                if (!parseBool(rtc["require_provisioned"],
                               &cfg.rtc.require_provisioned, error))
                    return fail("sources.rtc.require_provisioned " + *error);
            }
        }
    }

    *out = cfg;
    return validateTimeConfig(cfg, error);
}

}  // namespace time
}  // namespace tbox
