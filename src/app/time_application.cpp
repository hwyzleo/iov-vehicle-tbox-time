#include "app/time_application.h"

#include <chrono>
#include <fstream>
#include <sstream>

#include "log.h"  // tbox::fw::log（framework SDK）

#include "sources/source_manager.h"

namespace tbox {
namespace time {

namespace {

tbox::fw::log::Logger log() { return tbox::fw::log::Logger::get("time"); }

bool readFile(const std::string& path, std::string* out) {
    std::ifstream in(path);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

bool fileExists(const std::string& path) {
    std::ifstream in(path);
    return in.good();
}

}  // namespace

TimeApplication::TimeApplication() = default;

bool TimeApplication::loadConfig() {
    // 配置根：默认 {"/etc/tbox/", "./config/"}（FW Application 约定）。
    //
    // 候选文件顺序与 framework PathResolver 的分层优先级保持一致
    // （conf.d/<svc> < roots[i]/<svc>，见 framework src/config/path_resolver.h）：
    // 根级 time.yaml 优先于 conf.d/time.yaml，首个命中即生效。
    //   * conf.d/time.yaml        发布正式契约路径，BUILD release-managed
    //                             （config-deployment: /etc/tbox/conf.d/*.yaml → replace），
    //                             随 release-set 更新；
    //   * time.yaml/tbox-time.yaml 设备本地覆盖与开发 bring-up 路径，BUILD 无部署
    //                             规则（默认 preserve），不随发布更新。
    static constexpr const char* kCandidates[] = {
        "time.yaml",
        "tbox-time.yaml",
        "conf.d/time.yaml",
    };
    for (const auto& root : getConfigRoots()) {
        const std::string managed = root + "conf.d/time.yaml";
        for (const auto* name : kCandidates) {
            const std::string path = root + name;
            std::string text;
            if (!readFile(path, &text)) continue;
            // 设备本地文件遮蔽了 release-managed 配置：此时发布更新
            // conf.d/time.yaml 不会生效，必须可诊断而非静默降级。
            if (path != managed && fileExists(managed)) {
                log().warn("config.shadowed",
                           "device-local time config shadows the "
                           "release-managed conf.d config; release updates to "
                           "conf.d/time.yaml will not take effect",
                           {{"path", tbox::fw::log::FieldValue::makeString(path)},
                            {"shadowed", tbox::fw::log::FieldValue::makeString(managed)}});
            }
            {
                std::string err;
                if (!loadTimeConfig(text, &config_, &err)) {
                    log().error("config.load_failed", "config parse failed",
                                {{"path", tbox::fw::log::FieldValue::makeString(path)},
                                    {"error", tbox::fw::log::FieldValue::makeString(err)}});
                    return false;
                }
                log().info("config.loaded", "time config loaded",
                           {{"path", tbox::fw::log::FieldValue::makeString(path)}});
                return true;
            }
        }
    }
    log().error("config.missing",
                "no time config found in config roots "
                "(tried conf.d/time.yaml, time.yaml, tbox-time.yaml)");
    return false;
}

bool TimeApplication::initialize() {
    if (!loadConfig()) return false;

    // ---- 后端（system-observe：只读观测；linux：唯一写入方；mock：测试）----
    switch (config_.backend_kind) {
        case BackendKind::kLinuxSystemClock:
            backend_ = std::make_unique<LinuxSystemClockBackend>(
                config_.linux_backend);
            break;
        case BackendKind::kSystemObserve:
            backend_ = std::make_unique<SystemObserverBackend>();
            break;
        case BackendKind::kMock:
            backend_ = std::make_unique<MockBackend>(0);
            break;
        default:
            log().error("backend.unsupported", "backend kind not available");
            return false;
    }
    // 观测形态：TIME 是系统时钟同步状态的观测方与事实分发方，不写钟（CR-002）。
    config_.core.observe_mode =
        config_.backend_kind == BackendKind::kSystemObserve;
    backend_ready_ = backend_->capability() == BackendCapability::kReady;
    if (!backend_ready_) {
        log().warn("backend.unavailable",
                   "backend not ready; service will report unavailable time "
                   "(platform capability detection, ORIN pending)",
                   {{"capability", tbox::fw::log::FieldValue::makeString([this]() {
                        switch (backend_->capability()) {
                            case BackendCapability::kCompeting:
                                return std::string("competing_writer");
                            case BackendCapability::kFault:
                                return std::string("fault");
                            default:
                                return std::string("unsupported");
                        }
                    }())}});
    }

    // ---- 核心 ----
    core_ = std::make_unique<TimeCore>(
        backend_.get(), config_.core,
        []() { return NtpClient::defaultBootMs(); });

    // ---- 来源 ----
    source_mgr_ = std::make_unique<SourceManager>(core_.get());
    std::vector<SourceDetectInfo> infos;
    source_mgr_->setupConfiguredSources(config_.cellular, config_.gnss,
                                        config_.rtc, &infos);
    for (const auto& i : infos) {
        log().info("source.capability", "source capability detected",
                   {{"enabled", tbox::fw::log::FieldValue::makeBool(i.config_enabled)},
                    {"capability", tbox::fw::log::FieldValue::makeString(
                     [&i]() {
                         switch (i.capability) {
                             case SourceCapability::kDisabled:
                                 return std::string("disabled");
                             case SourceCapability::kUnsupported:
                                 return std::string("unsupported");
                             case SourceCapability::kAvailable:
                                 return std::string("available");
                             case SourceCapability::kDegraded:
                                 return std::string("degraded");
                         }
                         return std::string("unknown");
                     }())}});
    }

    // NTP（写钟形态：本次实际落地来源；观测形态下自采源被配置校验拒绝）
    for (const auto& ntp : config_.ntp_servers) {
        auto adapter = std::make_unique<NtpSourceAdapter>(ntp);
        core_->registerSource(adapter.get(), ntp.priority);
        ntp_adapters_.push_back(std::move(adapter));
    }

    // system-observe（CR-002）：timesyncd 观测来源（只读 adjtimex + D-Bus）。
    // 非 Linux/无 libsystemd 平台 capability=Unsupported：daemon 仍正常启动，
    // 明确报告时间不可用（不伪造观测）。
    if (config_.backend_kind == BackendKind::kSystemObserve) {
        auto obs = std::make_unique<SystemObserverSource>(config_.observer);
        core_->registerSource(obs.get(), 10);
        observer_source_ = std::move(obs);
    }

    // ---- IPC ----
    ipc_ = std::make_unique<TimeIpcServer>(config_.ipc_socket_path,
                                           config_.event_queue_limit);

    // ---- 事件接线（有界投递）----
    core_->setStateChangedCallback(
        [ipc = ipc_.get()](const TimeStateEvent& ev) { ipc->enqueueEvent(ev); });

    if (!ipc_->start(core_.get())) {
        log().error("ipc.start_failed", "ipc server failed to start",
                    {{"socket", tbox::fw::log::FieldValue::makeString(config_.ipc_socket_path)}});
        return false;
    }

    core_->start();
    source_mgr_->startAll();
    for (auto& a : ntp_adapters_) {
        auto gen = std::make_shared<std::atomic<uint32_t>>(0);
        auto cb = SourceManager::wrapGeneration(
            [core = core_.get()](const SourceSample& s) { core->onSourceSample(s); },
            gen);
        a->start(std::move(cb));
    }
    if (observer_source_) {
        auto gen = std::make_shared<std::atomic<uint32_t>>(0);
        auto cb = SourceManager::wrapGeneration(
            [core = core_.get()](const SourceSample& s) { core->onSourceSample(s); },
            gen);
        observer_source_->start(std::move(cb));
    }

    log().info("service.ready", "tbox-time service ready",
               {{"backend", tbox::fw::log::FieldValue::makeString(backend_ready_ ? "ready" : "unavailable")},
                {"ntp_servers", tbox::fw::log::FieldValue::makeInt(
                    static_cast<int64_t>(config_.ntp_servers.size()))}});
    return true;
}

void TimeApplication::startScheduler() {
    scheduler_running_ = true;
    scheduler_thread_ = std::thread([this]() {
        const auto interval =
            std::chrono::milliseconds(config_.scheduler_interval_ms);
        while (scheduler_running_.load() && !isShutdownRequested()) {
            core_->tick();
            std::this_thread::sleep_for(interval);
        }
    });
}

int TimeApplication::execute() {
    startScheduler();
    log().info("service.running", "tbox-time running (read-only IPC)");
    waitForShutdown(std::chrono::milliseconds{100});
    return 0;
}

void TimeApplication::cleanup() {
    // 停机：停止新任务、撤销状态、取消计时/回调、释放后端（设计 §8）
    scheduler_running_ = false;
    if (scheduler_thread_.joinable()) scheduler_thread_.join();

    core_->stop();

    for (auto& a : ntp_adapters_) a->stop();
    ntp_adapters_.clear();
    if (observer_source_) observer_source_->stop();
    observer_source_.reset();
    source_mgr_->stopAll();

    ipc_->stop();
    ipc_.reset();

    backend_.reset();
    core_.reset();
    source_mgr_.reset();

    log().info("service.stopped", "tbox-time stopped cleanly");
    tbox::fw::log::Logger::shutdown();
}

}  // namespace time
}  // namespace tbox
