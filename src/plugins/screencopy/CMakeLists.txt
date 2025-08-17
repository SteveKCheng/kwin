kcoreaddons_add_plugin(screencopy INSTALL_NAMESPACE "kwin/plugins")
target_sources(screencopy PRIVATE
    main.cpp
    screencopymanager.cpp
)

ecm_qt_declare_logging_category(screencopy
    HEADER kwinscreencopy_logging.h
    IDENTIFIER KWIN_SCREENCOPY
    CATEGORY_NAME kwin_screencopy
    DEFAULT_SEVERITY Warning
)

target_link_libraries(screencopy kwin KF6::I18n Libdrm::Libdrm)
