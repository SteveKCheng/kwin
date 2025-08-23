/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include <QPainter>
#include <QTimer>
#include <chrono>

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
    void prepareFrame(ScreencopyFrameV1Interface* frame,
                      QObject *clientState) override;

    QObject* createClientState() override;

    void copyFrame(ScreencopyFrameV1Interface* frame,
                   bool waitForDamage,
                   QObject* clientState) override;
};

namespace
{
class OutputTracking : public QObject
{
    Q_OBJECT

public:
    explicit OutputTracking(Output* output);

    Output* getOutput() const { return m_output.get(); }
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
    void renderFrame(ScreencopyFrameV1Interface & frame);

    /**
     * @brief Mark the buffer as ready, and update internal tracking variables.
     */
    void finishFrame(ScreencopyFrameV1Interface & frame);

    /**
     * @brief Send to the client the contents of the damaged region.
     *
     * The damaged region will be cleared afterwards.
     */
    void sendUpdatedContents();
};

class ClientState : public QObject
{
    Q_OBJECT

private:
    std::vector<std::unique_ptr<OutputTracking>> allOutputs;

public:
    OutputTracking& getOrCreateOutputTracking(Output* output)
    {
        auto iter = std::find_if(
            allOutputs.begin(),
            allOutputs.end(),
            [output](std::unique_ptr<OutputTracking> & item) {
                return item->getOutput() == output;
            });

        if (iter == allOutputs.end()) {
            allOutputs.push_back(std::make_unique<OutputTracking>(output));
            iter = allOutputs.end() - 1;
        }

        return *iter->get();
    }
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

void OutputTracking::renderFrame(ScreencopyFrameV1Interface & frame)
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

    auto mapping = clientBuffer->map(GraphicsBuffer::Write);
    if (!mapping.data) {
        clientBuffer->unmap();
        frame.sendFailed();
        return;
    }

    auto frameBox = clientBuffer->size();

    QImage frameImage{static_cast<uchar*>(mapping.data),
                      frameBox.width(), frameBox.height(),
                      mapping.stride,
                      QImage::Format_ARGB32_Premultiplied};

    grabTexture(texture.get(), &frameImage);

    if (frame.shouldOverlayCursor()) {
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
    clientBuffer->unmap();
}

void OutputTracking::finishFrame(ScreencopyFrameV1Interface & frame)
{
    m_accumulatedDamage.setRects(QSpan<QRect>());   // clear
    m_cursorHasChanged = false;

    // Send completion events - no Y_INVERT flag needed since grabTexture handles it
    frame.sendFlags(0);

    // Send timestamp using the output's presentation timestamp
    auto timestamp = m_output->renderLoop()->lastPresentationTimestamp();
    frame.sendReady(timestamp);
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

    // Save old location of cursor
    auto prevCursorBox = m_lastCursorBox;

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
    renderFrame(*frame);

    // Accumulate damage for the rendered cursor.
    if (frame->shouldOverlayCursor()) {
        if (prevCursorBox.isValid())
            m_accumulatedDamage += prevCursorBox;
        if (m_lastCursorBox.isValid())
            m_accumulatedDamage += m_lastCursorBox;
    }

    // Send damage event(s).
    if (m_accumulatedDamage.rectCount() <= 8) {
        for (const QRect & rect : m_accumulatedDamage)
            frame->sendDamage(rect);
    } else {
        frame->sendDamage(m_accumulatedDamage.boundingRect());
    }

    finishFrame(*frame);
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
        renderFrame(*frame);
        finishFrame(*frame);
    }
}

} // anonymous namespace

void
ScreencopyManagerImpl::prepareFrame(ScreencopyFrameV1Interface* frame,
                                    QObject* clientStateObj)
{
    // Send buffer format information
    Output *output = frame->getOutput();
    QSize outputSize = output->pixelSize();
    uint32_t format = WL_SHM_FORMAT_ARGB8888;
    uint32_t stride = outputSize.width() * 4; // 4 bytes per pixel for ARGB

    frame->sendBuffer(format, outputSize.width(), outputSize.height(), stride);
    frame->sendBufferDone();

    auto* clientState = static_cast<ClientState*>(clientStateObj);
    clientState->getOrCreateOutputTracking(output);
}

QObject*
ScreencopyManagerImpl::createClientState()
{
    return new ClientState();
}

void ScreencopyManagerImpl::copyFrame(ScreencopyFrameV1Interface* frame,
                                      bool waitForDamage,
                                      QObject* clientStateObj)
{
    auto* clientState = static_cast<ClientState*>(clientStateObj);
    auto& outputTracking = clientState->getOrCreateOutputTracking(frame->getOutput());
    outputTracking.copyFrame(frame, waitForDamage);
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
