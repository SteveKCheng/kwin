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
#include <chrono>
#include <drm_fourcc.h>

// Include the generated protocol header for flag definitions
#include "qwayland-server-wlr-screencopy-unstable-v1.h"

namespace KWin
{

static void performCopy(ScreencopyFrameV1Interface *frame, wl_resource *buffer_resource, bool withDamage)
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

    // Create a QImage wrapper around the client's buffer (no copy!)
    QImage targetImage(static_cast<uchar*>(mapping.data), 
                       clientBuffer->size().width(), 
                       clientBuffer->size().height(),
                       static_cast<qsizetype>(mapping.stride),
                       QImage::Format_ARGB32_Premultiplied);

    // Single copy: GPU texture directly to client buffer via QImage wrapper
    // The grabTexture function handles coordinate system conversion automatically
    grabTexture(texture.get(), &targetImage);

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
            this, [frame](wl_resource *buffer, bool withDamage) {
                performCopy(frame, buffer, withDamage);
            });
}

} // namespace KWin

#include "screencopymanager.moc"
