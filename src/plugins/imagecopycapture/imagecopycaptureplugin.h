/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "plugin.h"

namespace KWin
{

class ImageCopyCaptureManagerV1Interface;
class OutputImageCaptureSourceManagerV1Interface;

class ImageCopyCapturePlugin final : public Plugin
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
