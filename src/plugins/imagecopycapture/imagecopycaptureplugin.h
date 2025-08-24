/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include "plugin.h"

namespace KWin
{
class ImageCopyCaptureManagerV1Interface;
class OutputImageCaptureSourceManagerV1Interface;

class ImageCopyCapturePlugin : public Plugin
{
    Q_OBJECT

public:
    explicit ImageCopyCapturePlugin();
    ~ImageCopyCapturePlugin() override;

private:
    const std::unique_ptr<ImageCopyCaptureManagerV1Interface> m_imageCopyCaptureManager;
    const std::unique_ptr<OutputImageCaptureSourceManagerV1Interface> m_outputSourceManager;
};

} // namespace KWin
