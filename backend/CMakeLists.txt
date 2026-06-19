cmake_minimum_required(VERSION 3.16)
project(ai_food_planner C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MICROHTTPD REQUIRED libmicrohttpd)
pkg_check_modules(CJSON REQUIRED libcjson)
find_package(Threads REQUIRED)

enable_testing()

add_library(ai_food_planner_core STATIC
    src/nutrition.c
    src/ai_client.c
    src/json_parser.c
    src/db_sqlite.c
    src/db_supabase.c
)

target_include_directories(ai_food_planner_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MICROHTTPD_INCLUDE_DIRS}
        ${CJSON_INCLUDE_DIRS}
)

target_link_directories(ai_food_planner_core PUBLIC ${MICROHTTPD_LIBRARY_DIRS} ${CJSON_LIBRARY_DIRS})

target_link_libraries(ai_food_planner_core
    PUBLIC
        SQLite::SQLite3
        CURL::libcurl
        Threads::Threads
        ${MICROHTTPD_LIBRARIES}
        ${CJSON_LIBRARIES}
)

add_executable(ai_food_planner src/main.c)
target_link_libraries(ai_food_planner PRIVATE ai_food_planner_core)

add_executable(test_nutrition tests/test_nutrition.c)
target_link_libraries(test_nutrition PRIVATE ai_food_planner_core)
add_test(NAME test_nutrition COMMAND test_nutrition)

add_executable(test_json_parser tests/test_json_parser.c)
target_link_libraries(test_json_parser PRIVATE ai_food_planner_core)
add_test(NAME test_json_parser COMMAND test_json_parser)

add_executable(test_db tests/test_db.c)
target_link_libraries(test_db PRIVATE ai_food_planner_core)
add_test(NAME test_db COMMAND test_db)
