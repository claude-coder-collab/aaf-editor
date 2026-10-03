if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(AAF_WEBKIT QUIET webkit2gtk-4.1)
    endif()
    if(NOT AAF_WEBKIT_FOUND)
        message(WARNING "WebKitGTK 4.1 not found: the editor is not built (install libwebkit2gtk-4.1-dev)")
        return()
    endif()
endif()

find_program(AAF_NPM npm)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(AAF_UI_DIR ${PROJECT_SOURCE_DIR}/ui)
set(AAF_UI_DIST "" CACHE PATH "Use a prebuilt UI (a directory containing index.html) instead of building it with npm")

if(AAF_UI_DIST)
    set(ui_html ${AAF_UI_DIST}/index.html)
else()
    if(NOT AAF_NPM)
        message(WARNING "npm not found: the editor is not built (set AAF_UI_DIST to use a prebuilt UI)")
        return()
    endif()
    file(GLOB_RECURSE ui_sources CONFIGURE_DEPENDS ${AAF_UI_DIR}/src/*)
    set(ui_html ${CMAKE_CURRENT_BINARY_DIR}/ui/index.html)
    add_custom_command(
        OUTPUT ${ui_html}
        COMMAND ${AAF_NPM} ci --no-audit --no-fund --loglevel=error
        COMMAND ${AAF_NPM} run build -- --outDir ${CMAKE_CURRENT_BINARY_DIR}/ui --emptyOutDir
        WORKING_DIRECTORY ${AAF_UI_DIR}
        DEPENDS ${ui_sources} ${AAF_UI_DIR}/index.html ${AAF_UI_DIR}/package-lock.json ${AAF_UI_DIR}/vite.config.ts
        COMMENT "Building the editor UI"
        VERBATIM
    )
endif()

set(ui_cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/ui_html.cpp)
add_custom_command(
    OUTPUT ${ui_cpp}
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/embed_file.py ${ui_html} ${ui_cpp} kIndexHtml
    DEPENDS ${ui_html} ${PROJECT_SOURCE_DIR}/tools/embed_file.py
    COMMENT "Embedding the editor UI"
    VERBATIM
)

include(FetchContent)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_CHECKS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG 0.12.0
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_Declare(portable_file_dialogs
    GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
    GIT_TAG c12ea8c9a727f5320a2b4570aee863bbede2a204
    SYSTEM
)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(webview portable_file_dialogs)
unset(CMAKE_POLICY_VERSION_MINIMUM)
set_target_properties(webview_core_static PROPERTIES CXX_STANDARD 17)

add_executable(aafedit WIN32 MACOSX_BUNDLE main.cpp view.cpp worker.cpp ${ui_cpp})
if(APPLE)
    enable_language(OBJCXX)
    target_sources(aafedit PRIVATE drop_mac.mm)
    set_source_files_properties(drop_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(aafedit PRIVATE "-framework WebKit")
elseif(WIN32)
    target_sources(aafedit PRIVATE drop_win.cpp)
else()
    target_sources(aafedit PRIVATE drop_gtk.cpp)
    target_include_directories(aafedit PRIVATE ${AAF_WEBKIT_INCLUDE_DIRS})
endif()
target_link_libraries(aafedit PRIVATE aaf::rpc aaf::project_options webview::core_static portable_file_dialogs)
target_compile_definitions(aafedit PRIVATE AAF_VERSION="${AAF_VERSION}")
if(MSVC)
    target_link_options(aafedit PRIVATE /ENTRY:mainCRTStartup)
endif()
set_target_properties(aafedit PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "AAF Editor"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.claude-coder-collab.aaf-editor"
    MACOSX_BUNDLE_BUNDLE_VERSION ${AAF_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${AAF_VERSION}
)
set(AAF_ICON_DIR ${PROJECT_SOURCE_DIR}/packaging/icons)
if(WIN32)
    set(AAF_ICON_ICO ${AAF_ICON_DIR}/aafedit.ico)
    configure_file(aafedit.rc.in ${CMAKE_CURRENT_BINARY_DIR}/aafedit.rc @ONLY)
    target_sources(aafedit PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/aafedit.rc)
endif()
if(APPLE)
    set(icon ${AAF_ICON_DIR}/aafedit.icns)
    target_sources(aafedit PRIVATE ${icon})
    set_source_files_properties(${icon} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(aafedit PROPERTIES MACOSX_BUNDLE_ICON_FILE aafedit.icns)
endif()

install(TARGETS aafedit RUNTIME DESTINATION bin BUNDLE DESTINATION .)

if(APPLE AND TARGET aafquicklook)
    # The Quick Look extension lives in aafedit.app/Contents/PlugIns. Extensions must be sandboxed and signed,
    # and signing the app afterwards seals the extension into it. Ad-hoc ("-") by default.
    set(AAF_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity for aafedit.app and its Quick Look extension")
    set(app $<TARGET_BUNDLE_DIR:aafedit>)
    set(appex ${app}/Contents/PlugIns/AAFPreview.appex)
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/plugins.stamp)
    add_custom_command(
        OUTPUT ${stamp}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${appex}
        COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:aafquicklook> ${appex}
        COMMAND codesign --force --sign ${AAF_CODESIGN_IDENTITY} --entitlements ${AAF_QUICKLOOK_ENTITLEMENTS} ${appex}
        COMMAND codesign --force --sign ${AAF_CODESIGN_IDENTITY} ${app}
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        DEPENDS aafedit aafquicklook ${AAF_QUICKLOOK_ENTITLEMENTS}
        COMMENT "Embedding and signing the Quick Look extension"
        VERBATIM
    )
    add_custom_target(aafedit_bundle ALL DEPENDS ${stamp})
    # Installing strips the executables, which invalidates the signatures, so sign the installed bundle again.
    install(CODE "
        set(app \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/aafedit.app\")
        execute_process(COMMAND codesign --force --sign \"${AAF_CODESIGN_IDENTITY}\" --entitlements \"${AAF_QUICKLOOK_ENTITLEMENTS}\" \"\${app}/Contents/PlugIns/AAFPreview.appex\" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND codesign --force --sign \"${AAF_CODESIGN_IDENTITY}\" \"\${app}\" COMMAND_ERROR_IS_FATAL ANY)
    ")
endif()
