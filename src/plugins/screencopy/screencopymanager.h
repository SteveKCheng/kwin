/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include "plugin.h"

namespace KWin
{
class ScreencopyManagerV1Interface;
class ScreencopyFrameV1Interface;

class ScreencopyManager : public Plugin
{
    Q_OBJECT

public:
    explicit ScreencopyManager();
    ~ScreencopyManager() override;

private Q_SLOTS:
    void handleFrameRequested(ScreencopyFrameV1Interface *frame);

private:
    ScreencopyManagerV1Interface *m_screencopyManager = nullptr;
};

} // namespace KWin
