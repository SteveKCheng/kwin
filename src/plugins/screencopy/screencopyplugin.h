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

class ScreencopyPlugin : public Plugin
{
    Q_OBJECT

public:
    explicit ScreencopyPlugin();
    ~ScreencopyPlugin() override;

    void handleFrameRequested(ScreencopyFrameV1Interface *frame);

private Q_SLOTS:
    void handleOutputChange(Output *output, const QRegion &damageLogical);

private:
    const std::unique_ptr<ScreencopyManagerV1Interface> m_screencopyManager;

public:
    struct CopyRequest;

    // Per-output damage tracking
    struct OutputState {
        QRegion accumulatedDamage;     ///< Damage accumulated since last "ready" event
        QVector<CopyRequest> pending;  ///< Frames waiting for damage; usually there is at most one
        bool connected = false;        // Whether we're connected to damage signals
    };

    QHash<Output*, OutputState> m_outputStates;           // Track state for each output

    void processFramesForOutput(OutputState &state);
    void setupOutputTracking(OutputInterface *outputInterface);
};

} // namespace KWin
