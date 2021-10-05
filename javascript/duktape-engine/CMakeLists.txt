cmake_minimum_required(VERSION 3.10)

project(js)

add_library(duktape SHARED duktape/duktape.c)
target_include_directories(duktape PUBLIC duktape)

add_executable(js src/main.c src/log.c src/eventloop.c src/xmlhttprequest.c src/window.c src/console.c)
target_link_libraries(js pthread duktape readline ncursesw curl)
