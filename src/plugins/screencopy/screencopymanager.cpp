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
#include "opengl/gltexture.h"
#include "wayland/shmclientbuffer_p.h"
#include "../screencast/screencastutils.h"

#include <QImage>
#include <QObject>
#include <chrono>
#include "opengl/glutils.h"
#include "opengl/eglcontext.h"

// Include the generated protocol header for flag definitions
#include "qwayland-server-wlr-screencopy-unstable-v1.h"

namespace KWin
{

// Forward declaration
static void performCopyRegion(ScreencopyFrameV1Interface *frame, wl_resource *buffer_resource, const QRect *region);

// Helper function to grab a rectangular region from a texture
static void grabTextureRegion(GLTexture *texture, QImage *target, const QRect &region)
{
    const auto context = EglContext::currentContext();
    const bool invertNeeded = context->isOpenGLES() ^ (texture->contentTransform() != OutputTransform::FlipY);
    const bool invertNeededAndSupported = invertNeeded && context->supportsPackInvert();
    GLboolean prev;
    if (invertNeededAndSupported) {
        glGetBooleanv(GL_PACK_INVERT_MESA, &prev);
        glPixelStorei(GL_PACK_INVERT_MESA, GL_TRUE);
    }

    GLint oldRowLength;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &oldRowLength);
    glPixelStorei(GL_PACK_ROW_LENGTH,target->bytesPerLine() / 4);

    texture->bind();

    {
        // Bind a framebuffer to texture, then read the desired region
        // from the framebuffer.
        GLFramebuffer fbo(texture);
        GLFramebuffer::pushFramebuffer(&fbo);
        // Read only the specified region

        auto y = region.y();
        if (invertNeeded && !invertNeededAndSupported)
            y = target->height() - y;

        context->glReadnPixels(region.x(), y, region.width(), region.height(),
                              GL_BGRA, GL_UNSIGNED_BYTE,
                              target->sizeInBytes(), target->bits());
        GLFramebuffer::popFramebuffer();
    }

    glPixelStorei(GL_PACK_ROW_LENGTH, oldRowLength);

    if (invertNeededAndSupported) {
        if (!prev) {
            glPixelStorei(GL_PACK_INVERT_MESA, prev);
        }
    } else if (invertNeeded) {
        mirrorVertically(static_cast<uchar *>(target->bits()), target->height(), target->bytesPerLine());
    }
}

static void performCopy(ScreencopyFrameV1Interface *frame, wl_resource *buffer_resource)
{
    performCopyRegion(frame, buffer_resource, nullptr);
}

static void performCopyRegion(ScreencopyFrameV1Interface *frame, wl_resource *buffer_resource, const QRect *region)
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

    const QSize bufferSize = clientBuffer->size();
    const QRect copyRegion = region != nullptr
                                ? *region
                                : QRect(0, 0, bufferSize.width(), bufferSize.height());
    
    // Map the client buffer to get direct access to its memory
    auto mapping = clientBuffer->map(GraphicsBuffer::Read | GraphicsBuffer::Write);
    if (!mapping.data) {
        clientBuffer->unmap();
        frame->sendFailed();
        return;
    }

    if (copyRegion == QRect(0, 0, bufferSize.width(), bufferSize.height())) {
        // Full copy - use the existing optimized path
        QImage targetImage(static_cast<uchar*>(mapping.data), 
                          bufferSize.width(), bufferSize.height(),
                          static_cast<qsizetype>(mapping.stride),
                          QImage::Format_ARGB32_Premultiplied);
        grabTexture(texture.get(), &targetImage);
    } else {
        // Partial copy - copy only the bounding rectangle directly to client buffer
        const int bytesPerPixel = 4; // ARGB32
        const int dstStride = static_cast<int>(mapping.stride);
        uchar *regionStart = static_cast<uchar*>(mapping.data) + 
                            (copyRegion.y() * dstStride) + (copyRegion.x() * bytesPerPixel);
        
        // Create QImage wrapper directly around the region in client buffer
        // This avoids the extra memcpy by writing directly to the right location
        QImage regionImage(regionStart, 
                          copyRegion.width(), copyRegion.height(),
                          dstStride,  // Use client buffer stride
                          QImage::Format_ARGB32_Premultiplied);
        
        grabTextureRegion(texture.get(), &regionImage, copyRegion);
    }

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
        
        // Send damage event for the bounding rectangle
        frame->sendDamage(boundingRect.x(), boundingRect.y(),
                         boundingRect.width(), boundingRect.height());
        
        // Perform the copy with the accumulated damage region
        // Using the boundingRect of the damage for efficiency
        performCopyRegion(frame.data(), buffer, &boundingRect);

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
