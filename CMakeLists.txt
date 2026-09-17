cmake_minimum_required(VERSION 3.16)

project(LutherTools VERSION 0.4.1 LANGUAGES CXX)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# version.h (macros C++, ex: fenêtre) et version.rc (propriétés Windows de
# l'exe + icône embarquée dans l'exécutable) sont générés à partir de la
# VERSION ci-dessus : changer le numéro de version se fait uniquement ici.
configure_file(version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)
if(WIN32)
    # Chemin absolu : version.rc est généré dans le dossier de build, tandis
    # que l'icône reste dans le dossier source.
    set(LUTHERTOOLS_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/LutherTools.ico")
    configure_file(version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network WebSockets Multimedia LinguistTools TextToSpeech)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network WebSockets Multimedia LinguistTools TextToSpeech)

set(TS_FILES LutherTools_fr_FR.ts)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        tts.h
        tts.cpp
        chat.h
        chat.cpp
        edgettsclient.h
        edgettsclient.cpp
        twitchauth.h
        twitchauth.cpp
        twitchchannel.h
        twitchchannel.cpp
        twitchconstants.h
        appsettings.h
        appsettings.cpp
        flowlayout.h
        flowlayout.cpp
        third_party/dr_mp3.h
        third_party/dr_mp3_impl.cpp
        third_party/logger.h
        third_party/logger.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/version.h
        ${TS_FILES}
)

if(WIN32)
    list(APPEND PROJECT_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
endif()

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(LutherTools
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        ressources.qrc
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET LutherTools APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation

    qt_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})
else()
    if(ANDROID)
        add_library(LutherTools SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(LutherTools
            ${PROJECT_SOURCES}
        )
    endif()

    qt5_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})
endif()

target_link_libraries(LutherTools PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::WebSockets
    Qt${QT_VERSION_MAJOR}::Multimedia
    Qt${QT_VERSION_MAJOR}::TextToSpeech
)

target_include_directories(LutherTools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

if(WIN32)
    # TTS : SAPI (voix Windows) + COM.
    target_link_libraries(LutherTools PRIVATE sapi ole32 oleaut32)
endif()

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.LutherTools)
endif()
set_target_properties(LutherTools PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

if(WIN32)
    set_target_properties(LutherTools PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    set_target_properties(LutherTools PROPERTIES MACOSX_BUNDLE TRUE)
endif()

include(GNUInstallDirs)
install(TARGETS LutherTools
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(LutherTools)
endif()
