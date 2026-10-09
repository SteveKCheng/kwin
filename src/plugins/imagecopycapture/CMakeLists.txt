# Plugin
kcoreaddons_add_plugin(imagecopycapture INSTALL_NAMESPACE kwin/plugins)
target_sources(imagecopycapture PRIVATE
    imagecopycaptureplugin.cpp
    main.cpp
    outputcapturelayer.cpp
    outputrenderer.cpp
    rendertexture.cpp
)

target_link_libraries(imagecopycapture kwin)
