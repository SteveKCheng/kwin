# Plugin
kcoreaddons_add_plugin(imagecopyapture SOURCES main.cpp imagecopyaptureplugin.cpp INSTALL_NAMESPACE kwin/plugins)

target_link_libraries(imagecopyapture kwin)
