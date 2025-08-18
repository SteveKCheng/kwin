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
#include <drm_fourcc.h>
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
    // Connect to the copy request signal to handle actual copying
    connect(frame, &ScreencopyFrameV1Interface::copyRequested,
            this, [this, frame](wl_resource *buffer, bool withDamage) {
                if (withDamage) {
                    // For copy_with_damage, only set up damage tracking - don't copy immediately
                    // Use SingleShotConnection to automatically disconnect after first damage event
                    connect(frame->output()->handle(), &Output::outputChange,
                            frame, [frame, buffer](const QRegion &damage) {
                                // Compute bounding rectangle of all damage
                                QRect boundingRect = damage.boundingRect();
                                
                                // Send single "damage" event for the bounding rectangle.
                                // The spec says this "damage" event occurs "immediately before"
                                // the final "ready" event.  Taken literally, the spec
                                // is ambiguous as to whether the client buffer is updated before
                                // the "damage" event is received, but arguably it is okay
                                // to update the buffer after, as long as all updates occur
                                // before the "ready" event.  Using this interpretation saves
                                // us from having to split the performCopyRegion into two parts,
                                // the first part to copy the buffer and the second part to
                                // mark the frame as ready.
                                frame->sendDamage(boundingRect.x(), boundingRect.y(),
                                                 boundingRect.width(), boundingRect.height());

                                // Perform optimized copy of just the bounding rectangle
                                performCopyRegion(frame, buffer, &boundingRect);
                            }, Qt::SingleShotConnection);
                } else {
                    // For regular copy, do immediate copy
                    performCopy(frame, buffer);
                }
            });
}

} // namespace KWin

#include "screencopymanager.moc"
