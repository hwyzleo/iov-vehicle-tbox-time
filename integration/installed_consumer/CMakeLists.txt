cmake_minimum_required(VERSION 3.16)
project(time_installed_consumer LANGUAGES CXX)

# 仅消费安装后的 TIME SDK：不 add_subdirectory、不引用私有头/build-tree 库。
find_package(TboxTimeClient CONFIG REQUIRED)

add_executable(installed_consumer_smoke main.cpp)
target_link_libraries(installed_consumer_smoke PRIVATE tbox::time_client)
