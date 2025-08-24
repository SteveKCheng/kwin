# Plugin
kcoreaddons_add_plugin(imagecopycapture SOURCES main.cpp imagecopycaptureplugin.cpp INSTALL_NAMESPACE kwin/plugins)

target_link_libraries(imagecopycapture kwin)
