/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include "plugin.h"

namespace KWin
{
class ScreencopyManagerV1Interface;

class ScreencopyPlugin : public Plugin
{
    Q_OBJECT

public:
    explicit ScreencopyPlugin();
    ~ScreencopyPlugin() override;

private:
    const std::unique_ptr<ScreencopyManagerV1Interface> m_screencopyManager;
};

} // namespace KWin
