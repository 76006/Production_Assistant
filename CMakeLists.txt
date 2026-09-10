cmake_minimum_required(VERSION 3.16)

project(ProductionAssistant VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets SerialPort)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets SerialPort)

add_executable(ProductionAssistant WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/serialprotocol.cpp
    src/serialprotocol.h
)

target_link_libraries(ProductionAssistant PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::SerialPort
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(ProductionAssistant)
endif()

