/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "kwin_export.h"
#include <QObject>

struct wl_resource;

namespace KWin
{

class Display;
class Output;
class ImageCaptureSourceV1InterfacePrivate;
class OutputImageCaptureSourceManagerV1InterfacePrivate;

/**
 * @brief Represents a capture source for the Wayland ext-image-capture-source protocol.
 *
 * This is a concrete class that wraps the @c ext_image_capture_source_v1 interface.
 *
 * As the current implementation only supports capturing Output (@c wl_output),
 * this general representation of a capture source is only used during the
 * internal operation of @c OutputImageCaptureSourceManagerV1Interface,
 * but that could change.
 */
class ImageCaptureSourceV1Interface final : public QObject
{
    Q_OBJECT

public:
    ~ImageCaptureSourceV1Interface() override;

    /**
     * @brief Enumerates the possible types of capture sources.
     */
    enum class SourceType {
        Output,
        ForeignToplevel
    };

    /**
     * @brief Get the type of this capture source.
     */
    SourceType sourceType() const;

    /**
     * @brief Get the output if this is an output source, nullptr otherwise.
     */
    Output *output() const;

    /**
     * @brief Get ImageCaptureSourceV1Interface instance from wl_resource.
     */
    static ImageCaptureSourceV1Interface *get(struct wl_resource *resource);

private:
    explicit ImageCaptureSourceV1Interface(QObject *parent);

    friend class OutputImageCaptureSourceManagerV1InterfacePrivate;
    const std::unique_ptr<ImageCaptureSourceV1InterfacePrivate> d;
};

/**
 * @brief Represents the manager object for the Wayland ext-image-capture-source protocol.
 *
 * This is a concrete class that wraps the @c ext_output_image_capture_source_manager_v1 interface.
 *
 * This class must be instantiated too (as a singleton) for ImageCopyCaptureManagerV1Interface
 * (@c ext_image_copy_capture_manager_v1) to be usable for Wayland clients.
 */
class KWIN_EXPORT OutputImageCaptureSourceManagerV1Interface final : public QObject
{
    Q_OBJECT

public:
    explicit OutputImageCaptureSourceManagerV1Interface(Display *display, QObject *parent = nullptr);
    ~OutputImageCaptureSourceManagerV1Interface() override;

private:
    const std::unique_ptr<OutputImageCaptureSourceManagerV1InterfacePrivate> d;
};

} // namespace KWin
