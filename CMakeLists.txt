cmake_minimum_required(VERSION 3.10)
project(mail_system)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找Boost库
find_package(Boost 1.66.0 REQUIRED COMPONENTS system msm)

# 包含目录
include_directories(
    include
)

# 添加邮件服务器状态机库
add_library(mail_server_fsm
    src/back/mailServer/fsm/imaps/imaps_fsm.cpp
    src/back/mailServer/fsm/imaps/imaps_fsm_context.cpp
)

# 设置目标属性
set_target_properties(mail_server_fsm
    PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# 链接Boost库
target_link_libraries(mail_server_fsm
    PRIVATE
    Boost::system
    Boost::msm
)