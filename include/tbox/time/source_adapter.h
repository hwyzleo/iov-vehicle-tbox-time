#pragma once

//
// TBOX-TIME-DSN-CR-001：来源适配器契约（设计 §3）。
//
// 适配器输出 SourceSample；异步采样完成回调必须携带适配器 generation，
// 服务端以当前来源代次拒绝旧回调（AT-06）。
//

#include <functional>

#include "tbox/time/source_sample.h"

namespace tbox {
namespace time {

/// 异步采样结果回调。sample.utc_valid == false 表示本次采样失败。
using SampleCallback = std::function<void(const SourceSample& sample)>;

/// 来源适配器抽象。
class SourceAdapter {
public:
    virtual ~SourceAdapter() = default;

    /// 适配器标识（配置登记的 source_id）。
    virtual const std::string& sourceId() const = 0;
    virtual SourceKind kind() const = 0;

    /// 启动/停止采样。start 成功后适配器按自身轮询节奏回调。
    /// 回调必须携带递增 generation；stop 后不得再回调。
    virtual bool start(SampleCallback cb) = 0;
    virtual void stop() = 0;

    /// 当前能力状态（运行时检测结果）。
    virtual SourceCapability capability() const = 0;
};

}  // namespace time
}  // namespace tbox
