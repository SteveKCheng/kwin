/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include <QPainter>
#include <QTimer>
#include <chrono>

#include <drm_fourcc.h>

#include "screencopyplugin.h"
#include "wayland/output.h"
#include "wayland/screencopy_v1.h"
#include "wayland/shmclientbuffer_p.h"
#include "wayland_server.h"

#include "core/output.h"
#include "core/graphicsbuffer.h"

#include "compositor.h"
#include "cursor.h"
#include "main.h"

#include "../screencast/screencastutils.h"

namespace KWin
{

namespace
{
class OutputTracking : public QObject
{
    Q_OBJECT

public:
    explicit OutputTracking(Output* output);

    /**
     * @brief Get the output that this object is tracking.
     */
    Output * getOutput() const { return m_output.get(); }

    /**
     * @brief Implementation of ScreencopySession::prepareFrame for this output.
     */
    void prepareFrame(ScreencopyFrameV1Interface* frame);

    /**
     * @brief Implementation of ScreencopySession::copyFrame for this output.
     */
    void copyFrame(ScreencopyFrameV1Interface* frame, bool waitForDamage);

private Q_SLOTS:
    void handleOutputChange(const QRegion &damageLogical);
    void handleCursorChanged(Cursor* cursor);
    void handleCursorMoved(Cursor *cursor, const QPointF &position);
    void handleCursorHidden();

private:
    /**
     * @brief The display output that is being captured by this frame.
     */
    const QPointer<Output> m_output;

    /**
     * @brief True if the cursor has been invalidated or has moved since the last
     *        "ready" event was fired.
     */
    bool m_cursorHasChanged = false;

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
     * @brief The position and extents of the cursor when
     *        it was rendered last and sent to the client.
     *
     * This region is expressed in physical (scaled) coordinates.
     */
    QRect m_lastCursorBox;

    QPointer<ScreencopyFrameV1Interface> m_pendingFrame;

    //QList<QPointer<ScreencopyFrameImpl>> m_pendingFrames;

    /**
     * @brief Set up connections to the underlying Output to watch for changes.
     *
     * This method is called once from the constructor.  The connection must last
     * for the lifetime of this frame, since the client can call \c copy_with_damage
     * an any time in the future.
     */
    void trackOutput();

    /**
     * @brief Render the contents of the output into the frame's buffer.
     *
     * All of the frame will be rendered; this method does not clip the output
     * to (the bounding box of) m_acccumulatedDamage.
     *
     * This method also updates m_lastCursorBox as part of rendering the
     * cursor as part of the frame.
     */
    void renderFrame(ScreencopyFrameV1Interface & frame, QRectF cursorRect);

    /**
     * @brief Update internal tracking variables after an update has been
     *        sent successfully.
     */
    void finishUpdate(const QRect & cursorBox);

    /**
     * @brief Send to the client the contents of the damaged region.
     *
     * The damaged region will be cleared afterwards.
     */
    void sendUpdatedContents();

    /**
     * @brief Get the sole buffer format supported by this implementation,
     *        for the targeted output.
     *
     * This method is factored out for validating the client's buffer's parameters.
     */
    ScreencopyFrameV1Interface::BufferFormat getBufferFormat() const;

    /**
     * @brief Get the rectangular area where the (mouse) cursor is to be painted on the framebuffer.
     *
     * If there is no cursor to paint (including the case that it is hidden),
     * this method returns the null rectangle.
     */
    QRectF getCursorRect() const;
};

OutputTracking::OutputTracking(Output* output)
    : m_output(output)
{
    trackOutput();
}

void OutputTracking::trackOutput()
{
    connect(m_output.get(), &Output::outputChange, this, &OutputTracking::handleOutputChange);

    auto* cursors = Cursors::self();
    connect(cursors, &Cursors::currentCursorChanged, this, &OutputTracking::handleCursorChanged);
    connect(cursors, &Cursors::hiddenChanged, this, &OutputTracking::handleCursorHidden);
    connect(cursors, &Cursors::positionChanged, this, &OutputTracking::handleCursorMoved);
}

void OutputTracking::handleOutputChange(const QRegion &damageLogical)
{
    auto damagePhysical = scaleRegion(damageLogical, m_output->scale());
    m_accumulatedDamage |= damagePhysical;

    sendUpdatedContents();
}

void OutputTracking::handleCursorChanged(Cursor* cursor)
{
    m_cursorHasChanged = true;
    sendUpdatedContents();
}

void OutputTracking::handleCursorHidden()
{
    m_cursorHasChanged = true;
    sendUpdatedContents();
}

void OutputTracking::handleCursorMoved(Cursor *cursor, const QPointF &position)
{
    m_cursorHasChanged = true;
    sendUpdatedContents();
}

// Reverse the transformation done on the pixel format in ShmAttributes
uint32_t drmFormatToShmFormat(uint32_t drmFormat)
{
    switch (drmFormat) {
    case DRM_FORMAT_ARGB8888: return WL_SHM_FORMAT_ARGB8888;
    case DRM_FORMAT_XRGB8888: return WL_SHM_FORMAT_XRGB8888;
    default: return drmFormat;
    }
}

void OutputTracking::renderFrame(ScreencopyFrameV1Interface & frame, QRectF cursorRect)
{
    Output* output = m_output.get();

    // Get the compositor texture for the output
    auto [texture, color] = Compositor::self()->textureForOutput(output);
    if (!texture) {
        frame.sendFailed();
        return;
    }

    // Map the client buffer to get direct access to its memory
    auto* clientBuffer = frame.takeShmClientBuffer();
    if (!clientBuffer) {
        frame.sendFailed();
        return;
    }

    // Validate buffer parameters.
    const ShmAttributes* bufferAttributes = clientBuffer->shmAttributes();
    auto bufferFormat = ScreencopyFrameV1Interface::BufferFormat{
        drmFormatToShmFormat(bufferAttributes->format),
        bufferAttributes->size,
        bufferAttributes->stride,
    };
    if (bufferFormat != getBufferFormat()) {
        qWarning() << "Buffer passed for screencopy has the wrong format; failing the request";
        frame.sendFailed();
        return;
    }

    auto mapping = clientBuffer->map(GraphicsBuffer::Write);
    if (!mapping.data) {
        clientBuffer->unmap();
        frame.sendFailed();
        return;
    }

    QImage frameImage{static_cast<uchar*>(mapping.data),
                      bufferFormat.outputSize.width(),
                      bufferFormat.outputSize.height(),
                      mapping.stride,
                      QImage::Format_ARGB32_Premultiplied};

    grabTexture(texture.get(), &frameImage);

    if (frame.shouldOverlayCursor() && cursorRect.isValid()) {
        const QImage cursorImage = kwinApp()->cursorImage().image();
        if (!cursorImage.isNull()) {
            QPainter painter(&frameImage);
            painter.drawImage(cursorRect, cursorImage);
        }
    }

    // Unmap the buffer
    clientBuffer->unmap();

    // Send completion events - no Y_INVERT flag needed since grabTexture handles it
    frame.sendFlags(0);

    // Send timestamp using the output's presentation timestamp
    auto timestamp = m_output->renderLoop()->lastPresentationTimestamp();
    frame.sendReady(timestamp);
}

QRectF OutputTracking::getCursorRect() const
{
    Q_ASSERT(m_output != nullptr);

    const auto* cursors = Cursors::self();
    const Cursor* cursor = cursors->currentCursor();
    if (!cursors->isCursorHidden() && cursor != nullptr) {
        return scaledRect(cursor->geometry(), m_output->scale());
    }

    return QRectF();
}

void OutputTracking::finishUpdate(const QRect &cursorBox)
{
    m_accumulatedDamage.setRects(QSpan<QRect>());   // clear
    m_cursorHasChanged = false;
    m_lastCursorBox = cursorBox;
}

void OutputTracking::sendUpdatedContents()
{
    // Skip if there are no updates
    if (m_accumulatedDamage.isEmpty() && !m_cursorHasChanged)
        return;

    auto* frame = m_pendingFrame.get();
    if (frame == nullptr)
        return;

    m_pendingFrame = nullptr;

    auto cursorRect = getCursorRect();

    if (m_lastCursorBox.isValid()) {
        m_accumulatedDamage += m_lastCursorBox;
    }

    QRect newCursorBox;
    if (cursorRect.isValid()) {
        newCursorBox = cursorRect.toAlignedRect();
        m_accumulatedDamage += newCursorBox;
    }

    // Send damage event(s).
    if (m_accumulatedDamage.rectCount() <= 8) {
        for (const QRect & rect : m_accumulatedDamage)
            frame->sendDamage(rect);
    } else {
        frame->sendDamage(m_accumulatedDamage.boundingRect());
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
    // inefficiency: e.g. updating a single character in an interactive
    // terminal turns into a ~8 MB CPU memory transfer, depending on the
    // size of the screen.
    renderFrame(*frame, cursorRect);

    finishUpdate(newCursorBox);
}

void OutputTracking::copyFrame(ScreencopyFrameV1Interface* frame, bool waitForDamage)
{
    // Fail if output has already gone away.
    if (getOutput() == nullptr) {
        qWarning() << "output has gone away";
        frame->sendFailed();
        return;
    }

    if (waitForDamage) {
        m_pendingFrame = frame;
        sendUpdatedContents();
    } else {
        auto cursorRect = getCursorRect();
        renderFrame(*frame, cursorRect);
        finishUpdate(cursorRect.toAlignedRect());
    }
}

void OutputTracking::prepareFrame(ScreencopyFrameV1Interface* frame)
{
    frame->sendBuffer(getBufferFormat());
    frame->sendBufferDone();
}

ScreencopyFrameV1Interface::BufferFormat OutputTracking::getBufferFormat() const
{
    Q_ASSERT(m_output != nullptr);

    uint32_t pixelFormat = WL_SHM_FORMAT_ARGB8888;
    QSize outputSize = m_output->pixelSize();
    int rowStride = outputSize.width() * 4;

    return { pixelFormat, outputSize, rowStride };
}

class ScreencopySessionImpl : public ScreencopySession
{
    Q_OBJECT

    std::vector<std::unique_ptr<OutputTracking>> m_allOutputs;

    OutputTracking* getOutputTracking(Output* output, bool createIfMissing)
    {
        auto iter = std::find_if(
            m_allOutputs.begin(),
            m_allOutputs.end(),
            [output](std::unique_ptr<OutputTracking> & item) {
                return item->getOutput() == output;
            });

        if (iter == m_allOutputs.end()) {
            if (!createIfMissing) {
                return nullptr;
            }

            m_allOutputs.push_back(std::make_unique<OutputTracking>(output));
            iter = m_allOutputs.end() - 1;
        }

        return iter->get();
    }

public:
    void prepareFrame(ScreencopyFrameV1Interface* frame) override
    {
        auto* outputTracking = getOutputTracking(frame->getOutput(), true);
        outputTracking->prepareFrame(frame);
    }

    void copyFrame(ScreencopyFrameV1Interface* frame, bool waitForDamage) override
    {
        auto* outputTracking = getOutputTracking(frame->getOutput(), false);
        if (!outputTracking) {
            // Output may have gone away
            frame->sendFailed();
            return;
        }

        outputTracking->copyFrame(frame, waitForDamage);
    }
};

class ScreencopyManagerImpl final : public ScreencopyManagerV1Interface
{
    Q_OBJECT

public:
    ScreencopyManagerImpl(Display *display, ScreencopyPlugin* parent)
        : ScreencopyManagerV1Interface(display, parent)
    {
    }

protected:
    ScreencopySession* createSession() override
    {
        return new ScreencopySessionImpl();
    }
};

} // anonymous namespace

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
