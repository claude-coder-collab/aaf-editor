if(NOT APPLE)
    return()
endif()

enable_language(OBJCXX)

if(CMAKE_OSX_DEPLOYMENT_TARGET)
    set(AAF_QUICKLOOK_MIN_SYSTEM ${CMAKE_OSX_DEPLOYMENT_TARGET})
else()
    set(AAF_QUICKLOOK_MIN_SYSTEM 13.0)
endif()

add_executable(aafquicklook MACOSX_BUNDLE PreviewProvider.mm)
target_link_libraries(aafquicklook PRIVATE aaf::preview aaf::project_options "-framework Foundation" "-framework QuickLookUI" "-framework UniformTypeIdentifiers")
target_compile_options(aafquicklook PRIVATE -fobjc-arc -fapplication-extension)
target_link_options(aafquicklook PRIVATE -fapplication-extension -e _NSExtensionMain)
set_target_properties(aafquicklook PROPERTIES
    OUTPUT_NAME AAFPreview
    BUNDLE_EXTENSION appex
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.claude-coder-collab.aaf-editor.quicklook"
    MACOSX_BUNDLE_BUNDLE_VERSION ${AAF_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${AAF_VERSION}
)
set(AAF_QUICKLOOK_ENTITLEMENTS ${CMAKE_CURRENT_SOURCE_DIR}/AAFPreview.entitlements CACHE INTERNAL "")
