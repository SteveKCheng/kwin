/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include <QPainter>
#include <QTimer>
#include <chrono>

#include <drm_fourcc.h>

#include "imagecopyaptureplugin.h"
#include "wayland/output.h"
#include "wayland/image_copy_capture_v1.h"
#include "wayland/image_capture_source_v1.h"
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
     * @brief Implementation of ImageCopyCaptureSessionV1Interface::prepareFrame for this output.
     */
    void prepareFrame(ImageCopyCaptureSessionV1InterfaceBase* session);

    /**
     * @brief Implementation of ImageCopyCaptureSessionV1Interface::captureFrame for this output.
     */
    void captureFrame(ImageCopyCaptureFrameV1Interface* frame, bool waitForDamage, bool overlayCursor);

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

    QPointer<ImageCopyCaptureFrameV1Interface> m_pendingFrame;
    bool m_pendingOverlayCursor = false;

    /**
     * @brief Set up connections to the underlying Output to watch for changes.
     *
     * This method is called once from the constructor.  The connection must last
     * for the lifetime of this frame, since the client can call capture
     * at any time in the future.
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
    void renderFrame(ImageCopyCaptureFrameV1Interface & frame, QRectF cursorRect, bool overlayCursor);

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
    ImageCopyCaptureSessionV1InterfaceBase::BufferFormat getBufferFormat() const;

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

void OutputTracking::renderFrame(ImageCopyCaptureFrameV1Interface & frame, QRectF cursorRect, bool overlayCursor)
{
    Output* output = m_output.get();

    // Get the compositor texture for the output
    auto [texture, color] = Compositor::self()->textureForOutput(output);
    if (!texture) {
        frame.sendFailed(QtWaylandServer::ext_image_copy_capture_frame_v1::failure_reason_unknown);
        return;
    }

    // Map the client buffer to get direct access to its memory
    auto* clientBuffer = frame.takeShmClientBuffer();
    if (!clientBuffer) {
        frame.sendFailed(QtWaylandServer::ext_image_copy_capture_frame_v1::failure_reason_buffer_constraints);
        return;
    }

    // Validate buffer parameters.
    const ShmAttributes* bufferAttributes = clientBuffer->shmAttributes();
    auto bufferFormat = ImageCopyCaptureSessionV1InterfaceBase::BufferFormat{
        drmFormatToShmFormat(bufferAttributes->format),
        bufferAttributes->size,
        bufferAttributes->stride,
    };
    if (bufferFormat != getBufferFormat()) {
        qWarning() << "Buffer passed for image copy capture has the wrong format; failing the request";
        frame.sendFailed(QtWaylandServer::ext_image_copy_capture_frame_v1::failure_reason_buffer_constraints);
        return;
    }

    auto mapping = clientBuffer->map(GraphicsBuffer::Write);
    if (!mapping.data) {
        clientBuffer->unmap();
        frame.sendFailed(QtWaylandServer::ext_image_copy_capture_frame_v1::failure_reason_unknown);
        return;
    }

    QImage frameImage{static_cast<uchar*>(mapping.data),
                      bufferFormat.bufferSize.width(),
                      bufferFormat.bufferSize.height(),
                      mapping.stride,
                      QImage::Format_ARGB32_Premultiplied};

    grabTexture(texture.get(), &frameImage);

    if (overlayCursor && cursorRect.isValid()) {
        const QImage cursorImage = kwinApp()->cursorImage().image();
        if (!cursorImage.isNull()) {
            QPainter painter(&frameImage);
            painter.drawImage(cursorRect, cursorImage);
        }
    }

    // Unmap the buffer
    clientBuffer->unmap();

    // Send metadata events
    frame.sendTransform(WL_OUTPUT_TRANSFORM_NORMAL); // No transform applied
    
    // Send damage - for the new protocol we always send full damage for simplicity
    frame.sendDamage(QRect(0, 0, bufferFormat.bufferSize.width(), bufferFormat.bufferSize.height()));

    // Send presentation time using the output's presentation timestamp
    auto timestamp = m_output->renderLoop()->lastPresentationTimestamp();
    frame.sendPresentationTime(timestamp);

    // Send ready event to indicate successful capture
    frame.sendReady();
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

    // Render the full frame (similar to screencopy behavior)
    renderFrame(*frame, cursorRect, m_pendingOverlayCursor);

    finishUpdate(newCursorBox);
}

void OutputTracking::captureFrame(ImageCopyCaptureFrameV1Interface* frame, bool waitForDamage, bool overlayCursor)
{
    // Fail if output has already gone away.
    if (getOutput() == nullptr) {
        qWarning() << "output has gone away";
        frame->sendFailed(QtWaylandServer::ext_image_copy_capture_frame_v1::failure_reason_stopped);
        return;
    }

    if (waitForDamage) {
        m_pendingFrame = frame;
        m_pendingOverlayCursor = overlayCursor;
        sendUpdatedContents();
    } else {
        auto cursorRect = getCursorRect();
        renderFrame(*frame, cursorRect, overlayCursor);
        finishUpdate(cursorRect.toAlignedRect());
    }
}

void OutputTracking::prepareFrame(ImageCopyCaptureSessionV1InterfaceBase* session)
{
    // Send buffer constraints
    auto bufferFormat = getBufferFormat();
    session->sendBufferSize(bufferFormat.bufferSize);
    session->sendShmFormat(bufferFormat.pixelFormat);
    session->sendConstraintsDone();
}

ImageCopyCaptureSessionV1InterfaceBase::BufferFormat OutputTracking::getBufferFormat() const
{
    Q_ASSERT(m_output != nullptr);

    uint32_t pixelFormat = WL_SHM_FORMAT_ARGB8888;
    QSize bufferSize = m_output->pixelSize();
    int rowStride = bufferSize.width() * 4;

    return { pixelFormat, bufferSize, rowStride };
}

class ImageCopyCaptureSessionImpl : public ImageCopyCaptureSessionV1Interface
{
    Q_OBJECT

public:
    ImageCopyCaptureSessionImpl(ImageCopyCaptureManagerV1Interface *manager, Output *output, bool overlayCursor)
        : ImageCopyCaptureSessionV1Interface(manager)
        , m_output(output)
        , m_overlayCursor(overlayCursor)
        , m_outputTracking(std::make_unique<OutputTracking>(output))
    {
    }

    void prepareFrame() override
    {
        m_outputTracking->prepareFrame(this);
    }

    void captureFrame(ImageCopyCaptureFrameV1Interface* frame, bool waitForDamage) override
    {
        if (!m_outputTracking) {
            // Output may have gone away
            frame->sendFailed(QtWaylandServer::ext_image_copy_capture_frame_v1::failure_reason_stopped);
            return;
        }

        m_outputTracking->captureFrame(frame, waitForDamage, m_overlayCursor);
    }

    Output* getOutput() const override
    {
        return m_output.get();
    }

    bool shouldOverlayCursor() const override
    {
        return m_overlayCursor;
    }

private:
    QPointer<Output> m_output;
    bool m_overlayCursor;
    std::unique_ptr<OutputTracking> m_outputTracking;
};

class ImageCopyCaptureManagerImpl final : public ImageCopyCaptureManagerV1Interface
{
    Q_OBJECT

public:
    ImageCopyCaptureManagerImpl(Display *display, ImageCopyapturePlugin* parent)
        : ImageCopyCaptureManagerV1Interface(display, parent)
    {
    }

protected:
    ImageCopyCaptureSessionV1Interface* createSession(Output *output, bool overlayCursor) override
    {
        return new ImageCopyCaptureSessionImpl(this, output, overlayCursor);
    }
};

} // anonymous namespace

//
// Implementation of ImageCopyapturePlugin
//

ImageCopyapturePlugin::ImageCopyapturePlugin()
    : m_imageCopyCaptureManager(std::make_unique<ImageCopyCaptureManagerImpl>(waylandServer()->display(), this))
    , m_outputSourceManager(std::make_unique<OutputImageCaptureSourceManagerV1Interface>(waylandServer()->display(), this))
{
}

ImageCopyapturePlugin::~ImageCopyapturePlugin() = default;

} // namespace KWin

#include "imagecopyaptureplugin.moc"
