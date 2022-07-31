cmake_minimum_required(VERSION 3.6.2)

SET(CMAKE_CXX_STANDARD 11)

#编译警告不算错误
add_definitions(-w)

project(TinyWebServer)

MESSAGE(STATUS "Project: SERVER") #打印相关消息消息

set(SRC
${PROJECT_SOURCE_DIR}/main.cpp
${PROJECT_SOURCE_DIR}/threadpool/threadpool.h
${PROJECT_SOURCE_DIR}/http/http_conn.h
${PROJECT_SOURCE_DIR}/http/http_conn.cpp
${PROJECT_SOURCE_DIR}/lock/locker.h
${PROJECT_SOURCE_DIR}/log/log.h
${PROJECT_SOURCE_DIR}/log/log.cpp
${PROJECT_SOURCE_DIR}/log/block_queue.h
${PROJECT_SOURCE_DIR}/CGImysql/sql_connection_pool.h
${PROJECT_SOURCE_DIR}/CGImysql/sql_connection_pool.cpp
)

#LINK_DIRECTORIES一定要在add_executable的前面
LINK_DIRECTORIES(/usr/lib64/mysql/)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
add_executable(${PROJECT_NAME} ${SRC})

#target_link_libraries一定要在add_executable的后面
#这个可执行文件所需的库
target_link_libraries(${PROJECT_NAME} pthread mysqlclient dl)




