cmake_minimum_required(VERSION 3.21)

project(Faraday
    VERSION 1.0.6
    DESCRIPTION "Faraday - Battery Intelligence Suite"
    LANGUAGES CXX
)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    add_compile_options(/W4 /WX /permissive-)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick Sql Widgets LinguistTools)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

# ---------------------------------------------------------------------------
# faradaylib: acquisition + core logic, shared by the app and the tests
# ---------------------------------------------------------------------------
qt_add_library(faradaylib STATIC
    src/acquisition/BatteryTypes.h
    src/acquisition/WmiClient.h
    src/acquisition/WmiClient.cpp
    src/acquisition/BatteryReader.h
    src/acquisition/BatteryReader.cpp
    src/acquisition/PowercfgReport.h
    src/acquisition/PowercfgReport.cpp
    src/core/Metrics.h
    src/core/Metrics.cpp
    src/core/HealthVerdict.h
    src/core/HealthVerdict.cpp
    src/data/Database.h
    src/data/Database.cpp
    src/data/Settings.h
    src/data/Settings.cpp
    src/app/Sampler.h
    src/app/Sampler.cpp
    src/app/BatteryModel.h
    src/app/BatteryModel.cpp
    src/app/AlertManager.h
    src/app/AlertManager.cpp
    src/app/Autostart.h
    src/app/Autostart.cpp
    src/app/ChargeCap.h
    src/app/ChargeCap.cpp
    src/app/TrayManager.h
    src/app/TrayManager.cpp
    src/app/Calibration.h
    src/app/Calibration.cpp
    src/app/Exporter.h
    src/app/Exporter.cpp
    src/app/PortableMode.h
    src/app/PortableMode.cpp
)
target_include_directories(faradaylib PUBLIC src)
target_link_libraries(faradaylib PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    Qt6::Widgets
    ole32
    oleaut32
    wbemuuid
)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
qt_add_executable(faraday
    src/main.cpp
    resources/faraday.rc
    resources/icons.qrc
)

set_source_files_properties(ui/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

# The UI is a STATIC QML module library rather than being bound to the
# executable, so the layout regression tests can import the real pages and
# assert on their actual geometry (see tests/test_layout.cpp).
qt_add_qml_module(faradayui
    STATIC
    URI Faraday
    VERSION 1.0
    QML_FILES
        ui/Main.qml
        ui/Theme.qml
        ui/components/HealthRing.qml
        ui/components/MetricCard.qml
        ui/pages/DashboardPage.qml
        ui/pages/MonitorPage.qml
        ui/pages/HistoryPage.qml
        ui/pages/AlertsPage.qml
        ui/pages/CalibrationPage.qml
        ui/pages/SettingsPage.qml
)
target_link_libraries(faradayui PRIVATE Qt6::Quick)

# i18n: English ships as the source language; additional languages only
# need new .ts files here. The .qm files are embedded under :/i18n/.
# Arabic (ar) is right-to-left: main.cpp mirrors the layout for it.
qt_add_translations(faraday
    TS_FILES i18n/faraday_en.ts i18n/faraday_ar.ts
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete
)

set_target_properties(faraday PROPERTIES
    WIN32_EXECUTABLE TRUE
    # The Qt application ships as app\faraday-core.exe; the Faraday.exe the
    # user clicks is the static stub below, which verifies the runtime first
    # (a bare Qt exe dies in the Windows loader before main() - field
    # defects D1/D2, presentation layout P2).
    OUTPUT_NAME "faraday-core"
)

# ---------------------------------------------------------------------------
# Launcher stub: system DLLs only, statically linked MinGW runtime - starts
# in ANY environment, verifies the runtime, then runs app\faraday-core.exe.
# ---------------------------------------------------------------------------
add_executable(faraday_launcher WIN32
    src/launcher/launcher_main.cpp
    resources/launcher.rc
)
set_target_properties(faraday_launcher PROPERTIES OUTPUT_NAME "Faraday")
if(NOT MSVC)
    target_link_options(faraday_launcher PRIVATE -static)
endif()
target_link_libraries(faraday_launcher PRIVATE user32)

target_link_libraries(faraday PRIVATE
    faradaylib
    faradayui
    faradayuiplugin
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
)

enable_testing()
add_subdirectory(tests)
