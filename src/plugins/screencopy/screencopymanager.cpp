/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "screencopymanager.h"
#include "wayland/screencopy_v1.h"
#include "wayland/output.h"

#include "wayland_server.h"
#include "compositor.h"
#include "core/output.h"
#include "core/renderloop.h"
#include "core/graphicsbuffer.h"
#include "wayland/shmclientbuffer_p.h"
#include "../screencast/screencastutils.h"

#include <QImage>
#include <QObject>
#include <chrono>
#include "opengl/glutils.h"

// Include the generated protocol header for flag definitions
#include "qwayland-server-wlr-screencopy-unstable-v1.h"

namespace KWin
{

static void performCopy(ScreencopyFrameV1Interface *frame, wl_resource *buffer_resource)
{
    auto *clientBuffer = ShmClientBuffer::get(buffer_resource);
    if (!clientBuffer) {
        frame->sendFailed();
        return;
    }

    // Get the compositor texture for the output
    auto [texture, color] = Compositor::self()->textureForOutput(frame->output()->handle());
    if (!texture) {
        frame->sendFailed();
        return;
    }

    // Map the client buffer to get direct access to its memory
    auto mapping = clientBuffer->map(GraphicsBuffer::Read | GraphicsBuffer::Write);
    if (!mapping.data) {
        clientBuffer->unmap();
        frame->sendFailed();
        return;
    }

    auto frameBox = clientBuffer->size();

    QImage frameImage{static_cast<uchar*>(mapping.data),
                      frameBox.width(), frameBox.height(),
                      mapping.stride,
                      QImage::Format_ARGB32_Premultiplied};

    grabTexture(texture.get(), &frameImage);

    // Unmap the buffer
    clientBuffer->unmap();

    // Send completion events - no Y_INVERT flag needed since grabTexture handles it
    frame->sendFlags(0);

    // Send timestamp using the output's presentation timestamp
    auto timestamp = frame->output()->handle()->renderLoop()->lastPresentationTimestamp();
    frame->sendReady(timestamp);
}

ScreencopyManager::ScreencopyManager()
    : Plugin()
{
    m_screencopyManager = new ScreencopyManagerV1Interface(waylandServer()->display(), this);
    
    connect(m_screencopyManager, &ScreencopyManagerV1Interface::frameRequested,
            this, &ScreencopyManager::handleFrameRequested);
}

ScreencopyManager::~ScreencopyManager()
{
    delete m_screencopyManager;
}

void ScreencopyManager::handleFrameRequested(ScreencopyFrameV1Interface *frame)
{
    // Set up tracking for frame destruction
    connect(frame, &ScreencopyFrameV1Interface::destroyed,
            this, [this, frame]() {
                handleFrameDestroyed(frame);
            });
    
    // Connect to the copy request signal to handle actual copying
    connect(frame, &ScreencopyFrameV1Interface::copyRequested,
            this, [this, frame](wl_resource *buffer, bool withDamage) {
                if (withDamage) {
                    // Set up output tracking if not already done
                    setupOutputTracking(frame->output());
                    
                    Output *output = frame->output()->handle();
                    OutputState &state = m_outputStates[output];
                    
                    // Store the frame and buffer for later processing
                    frame->setProperty("buffer", QVariant::fromValue(reinterpret_cast<qintptr>(buffer)));
                    state.pendingFrames.append(QPointer<ScreencopyFrameV1Interface>(frame));
                    
                    // If we already have accumulated damage, process it immediately
                    if (!state.accumulatedDamage.isEmpty()) {
                        processFramesForOutput(output);
                    }
                    // If no accumulated damage, the frame will be processed when next damage occurs
                } else {
                    // For regular copy, do immediate copy
                    performCopy(frame, buffer);
                }
            });
}

void ScreencopyManager::setupOutputTracking(OutputInterface *outputInterface)
{
    Output *output = outputInterface->handle();
    if (!output || m_outputStates[output].connected) {
        return; // Already connected or invalid output
    }
    
    // Connect to output change signals - this connection persists for the lifetime of the plugin
    connect(output, &Output::outputChange,
            this, [this, output](const QRegion &damage) {
                handleOutputChange(output, damage);
            });
    
    m_outputStates[output].connected = true;
}

void ScreencopyManager::handleOutputChange(Output *output, const QRegion &damage)
{
    if (!m_outputStates.contains(output)) {
        return; // No state for this output
    }
    
    OutputState &state = m_outputStates[output];
    
    // Scale damage from logical to physical coordinates
    // Output::outputChange signal provides damage in logical coordinates
    // but we need physical coordinates for the screencopy buffers
    QRegion scaledDamage = scaleRegion(damage, output->scale());
    
    // Accumulate damage since last ready event
    state.accumulatedDamage = state.accumulatedDamage.united(scaledDamage);
    
    // Process all pending frames for this output
    if (!state.pendingFrames.isEmpty()) {
        processFramesForOutput(output);
    }
}

void ScreencopyManager::processFramesForOutput(Output *output)
{
    if (!m_outputStates.contains(output)) {
        return;
    }

    OutputState &state = m_outputStates[output];
    QRect boundingRect = state.accumulatedDamage.boundingRect();

    if (boundingRect.isEmpty())
        return;

    // Process all pending frames
    auto it = state.pendingFrames.begin();
    while (it != state.pendingFrames.end()) {
        QPointer<ScreencopyFrameV1Interface> frame = *it;
        if (!frame) {
            // Frame was destroyed, remove from list
            it = state.pendingFrames.erase(it);
            continue;
        }
        
        // Get the buffer that was stored when the frame was requested
        QVariant bufferVariant = frame->property("buffer");
        if (!bufferVariant.isValid()) {
            // No buffer stored, skip this frame
            ++it;
            continue;
        }
        
        auto *buffer = reinterpret_cast<wl_resource*>(bufferVariant.value<qintptr>());
        
        // Send damage event(s).  The protocol specification says these events
        // must come "right before" the "ready" event, but really the client
        // cannot read the frame until we send the "ready" event so it should
        // be okay to send them earlier, before we start writing to the client's
        // buffer.  This avoids having to split the code for "mark frame as ready"
        // away from the helper function performCopy, which is shared for
        // full-frame copies.
        frame->sendDamage(boundingRect.x(), boundingRect.y(),
                          boundingRect.width(), boundingRect.height());

        // One would be tempted to think that copy_with_damage allows the server
        // to optimize out the copying of pixels that are not contained in the
        // damaged area, but at least wayvnc assumes that copy_with_damage
        // fills the buffer with the full frame.  wayvnc does double-buffering,
        // so the buffer it passes to us is not guaranteed to have the contents
        // as of the last update, and it apparently just displays whatever ends up
        // in the buffer after we mark it as "ready".
        //
        // The protocol specification, if read very textually, supports wayvnc's
        // interpretation: it merely says that "copy_with_damage" is the "same"
        // as "copy" except that it waits for "damage" occurring.
        //
        // So we must copy out the whole frame on every update despite the
        // inefficiency: e.g. updating a single character in an interative
        // terminal turns into a ~8 MB CPU memory transfer, depending on the
        // size of the screen.
        performCopy(frame.data(), buffer);

        // Remove processed frame from pending list
        it = state.pendingFrames.erase(it);
    }
    
    // Clear accumulated damage since we've processed all frames
    state.accumulatedDamage = QRegion();
}

void ScreencopyManager::handleFrameDestroyed(ScreencopyFrameV1Interface *frame)
{
    // Remove the frame from all pending frame lists
    for (auto &state : m_outputStates) {
        state.pendingFrames.removeAll(QPointer<ScreencopyFrameV1Interface>(frame));
    }
}

} // namespace KWin

#include "screencopymanager.moc"
