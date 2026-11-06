#pragma once

//
// TBOX-TIME-DSN-CR-002：system-observe 保守不确定度与真实测量新鲜度公式（设计 §5）。
//
// 纯函数（无 IO、无 D-Bus/adjtimex 依赖），全平台可编译、可单测。
// 输入来自观测一致性边界（ObserverFacts）；时间统一毫秒、usec 按需换算。
//
// 公式（设计 §5.1/§5.2）：
//   root_distance_us = RootDelay_us/2 + RootDispersion_us + NTPMessage.Jitter_us
//   Δ_ms(t)         = BOOTTIME_now_ms - BOOTTIME_observed_ms (B0)
//   age_ms(t)       = age0_ms + Δ_ms(t)                       （真实测量年龄）
//   maxerror_term   = maxerror_us + tolerance_ppm * Δ_ms / 1000
//   ntp_term        = root_distance_us + tolerance_ppm * age_ms / 1000
//   uncertainty_ms  = ceil(max(maxerror_term, ntp_term) / 1000)
// maxerror 在内核中已反映截至观测时刻的增长，因此只追加观测后的 Δ；
// 根距离属于 DestinationTimestamp 时刻，因此追加完整真实年龄。
// 任何输入不可靠/溢出 → 返回 -1（unknown），不改用零或实测平均 offset。
//

#include <cstdint>

namespace tbox {
namespace time {

/// 根距离（usec）：RootDelay/2 + RootDispersion + Jitter。溢出 → -1。
inline int64_t observerRootDistanceUs(int64_t root_delay_us,
                                      int64_t root_dispersion_us,
                                      int64_t jitter_us) {
    if (root_delay_us < 0 || root_dispersion_us < 0 || jitter_us < 0) return -1;
    const int64_t half = root_delay_us / 2;  // 下取整：保守方向偏大（不低估误差）
    if (root_dispersion_us > INT64_MAX - half) return -1;
    if (jitter_us > INT64_MAX - (half + root_dispersion_us)) return -1;
    return half + root_dispersion_us + jitter_us;
}

/// 内核 timex.tolerance 换算 ppm：内核刻度为 ppm * 2^16（实测 32768000 = 500ppm）。
/// 有符号/溢出检查；非法（负值/换算溢出）→ -1。
inline int64_t observerTolerancePpm(int64_t tolerance_raw) {
    if (tolerance_raw < 0) return -1;
    const int64_t kScale = 65536;
    if (tolerance_raw / kScale > 1000000) return -1;  // 超出合理 ppm 上界
    return tolerance_raw / kScale;                    // 下取整：保守（增长上界偏大）
}

/// 真实测量新鲜度（ms）：age0 + (now_boot - B0)。仅 BOOTTIME 推进计龄（设计 §5.2）。
/// age0<0（报文比墙钟新）/B0 非法/溢出 → -1。
inline int64_t observerFreshnessMs(int64_t age0_ms, int64_t observed_boot_ms,
                                   int64_t now_boot_ms) {
    if (age0_ms < 0 || observed_boot_ms < 0 || now_boot_ms < observed_boot_ms) {
        return -1;
    }
    const int64_t delta = now_boot_ms - observed_boot_ms;
    if (age0_ms > INT64_MAX - delta) return -1;
    return age0_ms + delta;
}

/// 保守不确定度（ms，向上取整）。任一输入不可靠 → -1（unknown）。
/// tolerance_ppm 不可靠（<0）时无法界定增长 → unknown（不改用零）。
inline int64_t observerUncertaintyMs(int64_t age0_ms, int64_t observed_boot_ms,
                                     int64_t now_boot_ms, int64_t maxerror_us,
                                     int64_t root_distance_us,
                                     int64_t tolerance_ppm) {
    const int64_t delta_ms = now_boot_ms - observed_boot_ms;
    const int64_t age_ms = observerFreshnessMs(age0_ms, observed_boot_ms,
                                               now_boot_ms);
    if (delta_ms < 0 || age_ms < 0 || maxerror_us < 0 || root_distance_us < 0 ||
        tolerance_ppm < 0) {
        return -1;
    }
    // tolerance 增长项：tolerance_ppm * X_ms / 1000（usec）。
    const int64_t max_grow = (delta_ms > 1000000) ? (tolerance_ppm * (delta_ms / 1000))
                                                  : ((tolerance_ppm * delta_ms) / 1000);
    const int64_t ntp_grow = (age_ms > 1000000) ? (tolerance_ppm * (age_ms / 1000))
                                                : ((tolerance_ppm * age_ms) / 1000);
    // 溢出保护：growth 超 1e12 usec（= 1e6 ms）时已远超任何合理门限 → unknown。
    if (max_grow > 1000000000000LL || ntp_grow > 1000000000000LL) return -1;
    if (maxerror_us > INT64_MAX - max_grow) return -1;
    if (root_distance_us > INT64_MAX - ntp_grow) return -1;
    const int64_t maxerror_term = maxerror_us + max_grow;
    const int64_t ntp_term = root_distance_us + ntp_grow;
    const int64_t worst_us = maxerror_term > ntp_term ? maxerror_term : ntp_term;
    // 向上取整到毫秒：ceil(worst_us / 1000)
    return (worst_us + 999) / 1000;
}

/// 观测偏差对齐误差（ms）：ceil(abs(offset_us)/1000)，不写钟也不称读回验证。
inline int64_t observerAlignmentErrorMs(int64_t offset_us) {
    const int64_t abs_us = offset_us < 0 ? -offset_us : offset_us;
    return (abs_us + 999) / 1000;
}

}  // namespace time
}  // namespace tbox
