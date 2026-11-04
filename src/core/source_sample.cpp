#include "tbox/time/source_sample.h"

namespace tbox {
namespace time {

bool SourceSample::validate() const noexcept {
    // 结构/数值合法性校验。健康失败样本（utc_valid=false / health != kOk）
    // 是合法的状态传达（用于触发 HOLDOVER/UNSYNCHRONIZED），不在此拒绝。
    if (source_id.empty() || source_id.size() > 128) {
        return false;
    }
    if (utc_valid) {
        // UTC epoch ms 合理范围：2000-01-01 .. 2100-01-01
        if (utc_epoch_ms < 946684800000LL || utc_epoch_ms > 3471298560000LL) {
            return false;
        }
    }
    if (age_valid && (sampled_boot_ms < 0 || age_ms < 0)) {
        return false;
    }
    if (error_bounded && (error_upper_ms < 0 || error_upper_ms > 86400000)) {
        return false;
    }
    if (drift_bounded && (drift_upper_ppm < 0 || drift_upper_ppm > 1000000)) {
        return false;
    }
    // healthy 由构造方（适配器）按 health 设置；此处不覆盖。
    return true;
}

}  // namespace time
}  // namespace tbox
