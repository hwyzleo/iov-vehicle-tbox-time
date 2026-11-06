// TBOX-SEC-DSN-CR-024 依赖装配：installed-consumer 冒烟。
//
// 只使用安装后的公开 SDK（tbox::time_client / tbox/time/*.h）编译、链接并运行，
// 证明 install/export 闭包完整（TboxTimeClient package + tbox::time_client target
// + find_dependency(TboxFramework/nlohmann_json)）。
//
// 运行时不做真实授时断言（daemon 未启动）：验证公开类型可用、
// 与不存在 socket 的连接按契约失败（返回 false，不崩溃）。

#include <tbox/time/time_client.h>
#include <tbox/time/time_snapshot.h>
#include <tbox/time/types.h>

#include <cstdio>

int main() {
    tbox::time::TimeSnapshot snap;  // 默认构造（SDK 类型可用）
    // 默认快照非合法发布快照（contract_version==0），补全最小契约字段后必须可校验
    snap.contract_version = tbox::time::kContractVersion;
    snap.daemon_epoch = 1;
    snap.event_sequence = 1;
    snap.state = tbox::time::TimeState::kUnsynch;
    if (!snap.validate()) {
        std::fprintf(stderr, "TimeSnapshot::validate() failed on minimal snapshot\n");
        return 1;
    }
    if (snap.utc_valid) {
        std::fprintf(stderr, "default snapshot must be utc_invalid\n");
        return 1;
    }

    // 与不存在 socket 的连接：按契约失败而非崩溃（ClientError 分类属查询层）。
    tbox::time::TimeClient client("/tmp/tbox-time-installed-consumer-nonexistent.sock");
    if (client.connect()) {
        std::fprintf(stderr, "connect to nonexistent socket must fail\n");
        return 1;
    }

    std::printf("installed-consumer smoke OK: state=%s auth=%s\n",
                tbox::time::timeStateName(snap.state),
                tbox::time::authStatusName(snap.auth_status));
    return 0;
}
