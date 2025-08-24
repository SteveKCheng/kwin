/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

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
 * @brief Source type enum for type-safe discrimination.
 */
enum class ImageCaptureSourceType {
    Output,
    ForeignToplevel  // Not yet implemented
};

/**
 * The ImageCaptureSourceV1Interface represents an opaque image capture source.
 * 
 * This is a concrete class that wraps the ext_image_capture_source_v1 interface.
 * It maintains information about what type of source it represents.
 */
class KWIN_EXPORT ImageCaptureSourceV1Interface final : public QObject
{
    Q_OBJECT

public:
    ~ImageCaptureSourceV1Interface() override;

    /**
     * @brief Get the type of this capture source.
     */
    ImageCaptureSourceType sourceType() const;

    /**
     * @brief Get the output if this is an output source, nullptr otherwise.
     */
    Output* output() const;

    /**
     * @brief Get ImageCaptureSourceV1Interface instance from wl_resource.
     */
    static ImageCaptureSourceV1Interface* get(struct wl_resource *resource);

private:
    explicit ImageCaptureSourceV1Interface(QObject *parent);

    friend class OutputImageCaptureSourceManagerV1InterfacePrivate;
    const std::unique_ptr<ImageCaptureSourceV1InterfacePrivate> d;
};

/**
 * The OutputImageCaptureSourceManagerV1Interface provides ext_output_image_capture_source_manager_v1 support.
 * 
 * This manages the creation of image capture source objects for wl_output objects.
 */
class KWIN_EXPORT OutputImageCaptureSourceManagerV1Interface : public QObject
{
    Q_OBJECT

public:
    explicit OutputImageCaptureSourceManagerV1Interface(Display *display, QObject *parent = nullptr);
    ~OutputImageCaptureSourceManagerV1Interface() override;

private:
    const std::unique_ptr<OutputImageCaptureSourceManagerV1InterfacePrivate> d;
};

} // namespace KWin
