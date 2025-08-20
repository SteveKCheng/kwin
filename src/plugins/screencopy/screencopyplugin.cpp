/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "screencopyplugin.h"
#include "wayland/screencopy_v1.h"
#include "wayland/output.h"

#include "wayland_server.h"
#include "compositor.h"
#include "cursor.h"
#include "main.h"
#include "core/output.h"
#include "core/graphicsbuffer.h"
#include "wayland/shmclientbuffer_p.h"
#include "../screencast/screencastutils.h"

#include <QImage>
#include <QObject>
#include <QPainter>

#include <chrono>
#include "opengl/glutils.h"

#include "qwayland-server-wlr-screencopy-unstable-v1.h"

namespace KWin
{

class ScreencopyManagerImpl final : public ScreencopyManagerV1Interface
{
    Q_OBJECT
public:
    ScreencopyManagerImpl(Display *display, ScreencopyPlugin* parent)
        : ScreencopyManagerV1Interface(display, parent)
        , m_parent(parent)
    {
    }

    ScreencopyPlugin* const m_parent;

protected:
    ScreencopyFrameV1Interface* createFrame(bool overlayCursor,
                                            const QRect &frameBox,
                                            wl_resource* frameResource,
                                            OutputInterface* outputInterface) override;
};

class ScreencopyFrameImpl final : public ScreencopyFrameV1Interface
{
    Q_OBJECT
public:
    ScreencopyFrameImpl(bool overlayCursor,
                        const QRect &frameBox,
                        wl_resource* frameResource,
                        OutputInterface* outputInterface,
                        ScreencopyManagerImpl *parent);

private Q_SLOTS:
    void handleOutputChange(const QRegion &damageLogical);
    void handleCursorChanged(Cursor* cursor);
    void handleCursorMoved(Cursor *cursor, const QPointF &position);

private:
    /**
     * @brief The display output that is being captured by this frame.
     */
    const QPointer<Output> m_output;

    /**
     * @brief Whether the cursor should be rendered as part of the frame.
     */
    const bool m_overlayCursor;

    /**
     * @brief True if the cursor has been invalidated or has moved since the last
     *        "ready" event was fired.
     */
    bool m_cursorHasChanged = false;

    /**
     * @brief Set up connections to the underlying Output to watch for changes.
     *
     * This method is called once from the constructor.  The connection must last
     * for the lifetime of this frame, since the client can call \c copy_with_damage
     * an any time in the future.
     */
    void trackOutput();

    /**
     * @brief Send to the client the contents of the damaged region,
     *        if a buffer has been registered with this frame earlier.
     *
     * The damaged region will be cleared afterwards, and the registered client buffer
     * will be de-registered.
     */
    void sendUpdatedContents();

    /**
     * @brief Render the contents of the frame to the given buffer, and
     *        mark it as ready for the client.
     *
     * All of the frame will be rendered; this method does not clip the output
     * to (the bounding box of) acccumulatedDamage.
     *
     * This method also updates m_lastCursorBox as part of rendering the
     * cursor as part of the frame.
     */
    void renderToBuffer(ShmClientBuffer &clientBuffer);

    /**
     * @brief Mark the buffer as ready, and update internal tracking variables.
     */
    void finishUpdate();

    /**
     * @brief The region of the Output that has changed since the last "ready" event
     *        was fired.
     *
     * This region is expressed in physical (scaled) coordinates.
     *
     * This region does not include the overlaid cursor; that is tracked separately.
     */
    QRegion m_accumulatedDamage;

    /**
     * @brief The shared-memory buffer captured from the client when it calls
     *        \c copy or \c copy_with_damage for a frame.
     */
    QPointer<ShmClientBuffer> m_capturedShmBuffer;

    /**
     * @brief The position and extents of the cursor when
     *        it was rendered last and sent to the client.
     *
     * This region is expressed in physical (scaled) coordinates.
     */
    QRect m_lastCursorBox;

protected:
    void copyRequested(ShmClientBuffer *clientBuffer, bool waitForDamage) override;
};

ScreencopyFrameV1Interface*
ScreencopyManagerImpl::createFrame(bool overlayCursor,
                                   const QRect &frameBox,
                                   wl_resource* frameResource,
                                   OutputInterface* outputInterface)
{
    return new ScreencopyFrameImpl(overlayCursor,
                                   frameBox,
                                   frameResource,
                                   outputInterface,
                                   this);
}

ScreencopyFrameImpl::ScreencopyFrameImpl(bool overlayCursor,
                                         const QRect &frameBox,
                                         wl_resource *frameResource,
                                         OutputInterface *outputInterface,
                                         ScreencopyManagerImpl *parent)
    : ScreencopyFrameV1Interface(frameResource, parent)
    , m_output(outputInterface->handle())
    , m_overlayCursor(overlayCursor)
{
    // Send buffer format information
    Output *output = m_output.get();
    QSize outputSize = output->pixelSize();
    uint32_t format = WL_SHM_FORMAT_ARGB8888;
    uint32_t stride = outputSize.width() * 4; // 4 bytes per pixel for ARGB

    sendBuffer(format, outputSize.width(), outputSize.height(), stride);
    sendBufferDone();

    trackOutput();
}

void ScreencopyFrameImpl::copyRequested(ShmClientBuffer * clientBuffer, bool waitForDamage)
{
    // Fail if output has already gone away.
    if (!m_output) {
        sendFailed();
        return;
    }

    if (m_capturedShmBuffer) {
        // zwlr_screencopy_frame_v1::error::already_used
        sendFailed();
        return;
    }

    if (waitForDamage) {
        m_capturedShmBuffer = clientBuffer;
        sendUpdatedContents();
    } else {
        renderToBuffer(*clientBuffer);
        finishUpdate();
    }
}

void ScreencopyFrameImpl::trackOutput()
{
    if (!m_output)
        return;

    connect(m_output.get(), &Output::outputChange, this, &ScreencopyFrameImpl::handleOutputChange);

    if (m_overlayCursor) {
        auto* cursors = Cursors::self();
        connect(cursors, &Cursors::currentCursorChanged, this, &ScreencopyFrameImpl::handleCursorChanged);
        connect(cursors, &Cursors::positionChanged, this, &ScreencopyFrameImpl::handleCursorMoved);
    }
}

void ScreencopyFrameImpl::handleOutputChange(const QRegion &damageLogical)
{
    auto damagePhysical = scaleRegion(damageLogical, m_output->scale());
    m_accumulatedDamage = m_accumulatedDamage.united(damagePhysical);

    sendUpdatedContents();
}

void ScreencopyFrameImpl::handleCursorChanged(Cursor* cursor)
{
    m_cursorHasChanged = true;
    sendUpdatedContents();
}

void ScreencopyFrameImpl::handleCursorMoved(Cursor *cursor, const QPointF &position)
{
    m_cursorHasChanged = true;
    sendUpdatedContents();
}

void ScreencopyFrameImpl::sendUpdatedContents()
{
    // Skip if there are no updates
    if (m_accumulatedDamage.isEmpty() && !m_cursorHasChanged)
        return;

    auto* clientBuffer = m_capturedShmBuffer.get();
    if (clientBuffer == nullptr)
        return;

    QRect boundingRect = m_accumulatedDamage.boundingRect();

    // Send damage event(s).  The protocol specification says these events
    // must come "right before" the "ready" event, but really the client
    // cannot read the frame until we send the "ready" event so it should
    // be okay to send them earlier, before we start writing to the client's
    // buffer.
    sendDamage(boundingRect);

    // Old location of cursor is damaged
    if (m_overlayCursor && m_lastCursorBox.isValid()) {
        sendDamage(m_lastCursorBox);
    }

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
    renderToBuffer(*clientBuffer);

    // New location of cursor is also damaged
    if (m_overlayCursor && m_lastCursorBox.isValid()) {
        sendDamage(m_lastCursorBox);
    }

    finishUpdate();
}

void ScreencopyFrameImpl::renderToBuffer(ShmClientBuffer & clientBuffer)
{
    Output* output = m_output.get();

    // Get the compositor texture for the output
    auto [texture, color] = Compositor::self()->textureForOutput(output);
    if (!texture) {
        sendFailed();
        return;
    }

    // Map the client buffer to get direct access to its memory
    auto mapping = clientBuffer.map(GraphicsBuffer::Write);
    if (!mapping.data) {
        clientBuffer.unmap();
        sendFailed();
        return;
    }

    auto frameBox = clientBuffer.size();

    QImage frameImage{static_cast<uchar*>(mapping.data),
                      frameBox.width(), frameBox.height(),
                      mapping.stride,
                      QImage::Format_ARGB32_Premultiplied};

    grabTexture(texture.get(), &frameImage);

    if (m_overlayCursor) {
        const Cursor* cursor = Cursors::self()->currentCursor();
        const QImage cursorImage = kwinApp()->cursorImage().image();
        if (cursor != nullptr && !cursorImage.isNull()) {
            const QRectF cursorRect = scaledRect(cursor->geometry(), output->scale());
            QPainter painter(&frameImage);
            painter.drawImage(cursorRect, cursorImage);
            m_lastCursorBox = cursorRect.toAlignedRect();
        } else {
            m_lastCursorBox = QRect();
        }
    }

    // Unmap the buffer
    clientBuffer.unmap();
}

void
ScreencopyFrameImpl::finishUpdate()
{
    m_accumulatedDamage = QRegion();
    m_cursorHasChanged = false;
    m_capturedShmBuffer = nullptr;

    // Send completion events - no Y_INVERT flag needed since grabTexture handles it
    sendFlags(0);

    // Send timestamp using the output's presentation timestamp
    auto timestamp = m_output->renderLoop()->lastPresentationTimestamp();
    sendReady(timestamp);
}

//
// Implementation of ScreencopyPlugin
//

ScreencopyPlugin::ScreencopyPlugin()
    : m_screencopyManager(std::make_unique<ScreencopyManagerImpl>(waylandServer()->display(), this))
{
}

ScreencopyPlugin::~ScreencopyPlugin() = default;

} // namespace KWin

#include "screencopyplugin.moc"
