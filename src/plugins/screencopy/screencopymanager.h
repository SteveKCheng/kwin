/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include "plugin.h"
#include <QHash>
#include <QPointer>
#include <QRegion>
#include <QVector>

namespace KWin
{
class ScreencopyManagerV1Interface;
class ScreencopyFrameV1Interface;
class Output;
class OutputInterface;

class ScreencopyManager : public Plugin
{
    Q_OBJECT

public:
    explicit ScreencopyManager();
    ~ScreencopyManager() override;

private Q_SLOTS:
    void handleFrameRequested(ScreencopyFrameV1Interface *frame);
    void handleOutputChange(Output *output, const QRegion &damage);
    void handleFrameDestroyed(ScreencopyFrameV1Interface *frame);

private:
    void processFramesForOutput(Output *output);
    void setupOutputTracking(OutputInterface *outputInterface);

    ScreencopyManagerV1Interface *m_screencopyManager = nullptr;
    
    // Per-output damage tracking
    struct OutputState {
        QRegion accumulatedDamage;                          // Damage accumulated since last ready event
        QVector<QPointer<ScreencopyFrameV1Interface>> pendingFrames;  // Frames waiting for damage
        bool connected = false;                            // Whether we're connected to damage signals
    };
    
    QHash<Output*, OutputState> m_outputStates;           // Track state for each output
};

} // namespace KWin
