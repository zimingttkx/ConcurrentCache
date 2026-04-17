
cmake_minimum_required(VERSION 3.31)
# 2. 项目名称
project(ConcurrentCache LANGUAGES CXX)

# 3. 设置C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # 关闭编译器扩展，保持标准一致性


include_directories(${PROJECT_SOURCE_DIR}/src)

# 5. 搜集所有.cpp源文件

file(GLOB_RECURSE SOURCES
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/main.cpp" # 把main.cpp也加入源文件列表
)

# 6. 生成可执行文件
add_executable(concurrentcache-server ${SOURCES})

# 7. 链接库 Windows平台下pthread不存在，需要条件编译
if(UNIX)
    target_link_libraries(concurrentcache-server pthread)
endif()

# 8. 可选：设置编译选项
if(MSVC)
    # Windows MSVC编译器选项
    target_compile_options(concurrentcache-server PRIVATE /W4 /WX)
else()
    # GCC/Clang编译器选项
    target_compile_options(concurrentcache-server PRIVATE -Wall -Wextra -Werror)
endif()