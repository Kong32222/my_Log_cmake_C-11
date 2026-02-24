cmake_minimum_required(VERSION 3.10)
project(text)

#添加头文件
include_directories(${PROJECT_SOURCE_DIR}/include)

aux_source_directory(${PROJECT_SOURCE_DIR}/src SRC_LIST)
add_executable(text ${SRC_LIST}  )
# 链接 pthread 库  
target_link_libraries(text pthread)
