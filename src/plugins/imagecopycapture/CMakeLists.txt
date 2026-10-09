# Plugin
kcoreaddons_add_plugin(imagecopycapture INSTALL_NAMESPACE kwin/plugins)
target_sources(imagecopycapture PRIVATE
    imagecopycaptureplugin.cpp
    main.cpp
    outputcapturelayer.cpp
    outputrenderer.cpp
    rendertexture.cpp
)

ecm_qt_declare_logging_category(imagecopycapture
    HEADER imagecopycapture_logging.h
    IDENTIFIER KWIN_IMAGECOPYCAPTURE
    CATEGORY_NAME kwin_imagecopycapture
    DEFAULT_SEVERITY Warning
)

target_link_libraries(imagecopycapture kwin)
