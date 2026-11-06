#!/usr/bin/env bash
#
# TBOX-SEC-DSN-CR-024 依赖装配：installed-consumer 验收脚本。
#
# 1. 将 TIME 构建产物安装到临时前缀（time-sdk + time-runtime）；
# 2. 以 find_package(TboxTimeClient CONFIG) 配置/编译一个独立消费方工程
#    （仅使用安装后 SDK，不 add_subdirectory、不引用私有头/build-tree 库）；
# 3. 运行冒烟二进制。
#
# 用法：run_test.sh <TIME_BUILD_DIR> <CONSUMER_SRC_DIR> <SCRATCH_DIR>
set -euo pipefail

TIME_BUILD_DIR="$1"
CONSUMER_SRC="$2"
SCRATCH="$3"

PREFIX="$SCRATCH/prefix"
CONSUMER_BUILD="$SCRATCH/build"

rm -rf "$SCRATCH"
mkdir -p "$PREFIX" "$CONSUMER_BUILD"

# 安装 TIME SDK/daemon 到临时前缀
cmake --install "$TIME_BUILD_DIR" --prefix "$PREFIX" >/dev/null

# 消费方前缀 = 既有前缀（framework/nlohmann_json 等）+ 临时安装前缀。
# 注意：不允许空路径分量（空分量会被 CMake 解释为当前目录，可能命中 build-tree 内的
# 未安装 config 文件）；无既有前缀时直接赋值。
if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
    CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH}:${PREFIX}"
else
    CMAKE_PREFIX_PATH="${PREFIX}"
fi
export CMAKE_PREFIX_PATH

cmake -S "$CONSUMER_SRC" -B "$CONSUMER_BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$CONSUMER_BUILD"
"$CONSUMER_BUILD/installed_consumer_smoke"
