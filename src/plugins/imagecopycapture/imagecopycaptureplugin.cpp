/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include <QPainter>
#include <QTimer>
#include <chrono>

#include <drm_fourcc.h>

#include "imagecopycaptureplugin.h"
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

class ImageCopyCaptureSessionImpl : public ImageCopyCaptureSessionV1Interface
{
    Q_OBJECT

public:
    ImageCopyCaptureSessionImpl(wl_resource* resource,
                                ImageCopyCaptureManagerV1Interface *manager,
                                Output *output,
                                bool overlayCursor);

    void captureFrame() override;
    void addClientBufferDamage(const QRect & damage) override {}

private Q_SLOTS:
    void handleOutputChange(const QRegion &damageLogical);
    void handleCursorChanged(Cursor* cursor);
    void handleCursorMoved(Cursor *cursor, const QPointF &position);
    void handleCursorHidden();

private:
    /**
     * @brief The display output that is being captured by this frame.
     *
     * Part of the parameters set by the client.
     */
    const QPointer<Output> m_output;

    /**
     * @brief Whether the cursor should be rendered as part of the frame.
     *
     * Part of the parameters set by the client.
     */
    const bool m_overlayCursor;

    /**
     * @brief True if the cursor has been invalidated or has moved since the last
     *        "ready" event was fired.
     */
    bool m_cursorHasChanged;

    /**
     * @brief The region of the Output that has changed since the last "ready" event
     *        was fired.
     *
     * This region is expressed in physical (scaled) coordinates.
     *
     * This region does not include the overlaid cursor; that is tracked separately.
     *
     * On initialize the whole output is considered damage, which makes the
     * first capture request to be satisfied immediately, and the frame
     * sent out will contain everything.
     */
    QRegion m_frameDamage;

    /**
     * @brief The position and extents of the cursor when
     *        it was rendered last and sent to the client.
     *
     * This region is expressed in physical (scaled) coordinates.
     *
     * This information is separated out from m_frameDamage for:
     *
     *   - reporting the old location of the cursor to erase (as damage)
     *     on the next frame
     *   - remembering the old location of the cursor if it has not been
     *     invalidated to re-paint again, while not damaging it unnecessarily
     */
    QRectF m_lastCursorBox;

    /**
     * @brief Send events to advertise the supported buffer formats to the client.
     *
     * Currently only one buffer format, the most straightforward one, is supported.
     */
    void advertiseBufferConstraints();

    /**
     * @brief Render the contents of the output into the frame's buffer.
     *
     * This method also sends the meta-information on the frame contents,
     * but does not mark the frame as "ready".
     *
     * Called as part of #sendCapturedFrame.
     *
     * @return True if rendering was successful; false if the frame has been marked
     *         has failed.
     */
    bool renderFrame(ImageCopyCaptureFrameV1Interface & frame, const QRectF & cursorBox);

    /**
     * @brief Update internal tracking variables after a frame update has been
     *        sent successfully.
     */
    void updateTracking(const QRectF & cursorBox);

    /**
     * @brief Send to the client the contents of the current output, if there
     *        is a frame being captured.
     *
     * This method will call #updateTracking afterwards.
     */
    void sendFrameUpdatesIfAny();

    /**
     * @brief Get the rectangular area where the (mouse) cursor is to be painted on the framebuffer.
     *
     * If there is no cursor to paint (including the case that it is hidden),
     * this method returns the null rectangle.
     */
    QRectF getCursorBox() const;
};

ImageCopyCaptureSessionImpl::ImageCopyCaptureSessionImpl(wl_resource* resource,
                                                         ImageCopyCaptureManagerV1Interface *manager,
                                                         Output *output,
                                                         bool overlayCursor)
    : ImageCopyCaptureSessionV1Interface(resource, manager)
    , m_output(output)
    , m_overlayCursor(overlayCursor)
{
    // All output contents are damaged at start
    m_frameDamage += QRect(QPoint(0,0), output->pixelSize());
    m_cursorHasChanged = true;

    connect(output, &Output::outputChange, this, &ImageCopyCaptureSessionImpl::handleOutputChange);

    if (overlayCursor) {
        auto* cursors = Cursors::self();
        connect(cursors, &Cursors::currentCursorChanged, this, &ImageCopyCaptureSessionImpl::handleCursorChanged);
        connect(cursors, &Cursors::hiddenChanged, this, &ImageCopyCaptureSessionImpl::handleCursorHidden);
        connect(cursors, &Cursors::positionChanged, this, &ImageCopyCaptureSessionImpl::handleCursorMoved);
    }

    advertiseBufferConstraints();
}

void ImageCopyCaptureSessionImpl::handleOutputChange(const QRegion &damageLogical)
{
    auto outputScale = m_output->scale();
    for (const auto & rectLogical : damageLogical) {
        auto rectPhysical = scaledRect(rectLogical.toRectF(), outputScale);
        m_frameDamage += rectPhysical.toAlignedRect();
    }

    sendFrameUpdatesIfAny();
}

void ImageCopyCaptureSessionImpl::handleCursorChanged(Cursor* cursor)
{
    m_cursorHasChanged = true;
    sendFrameUpdatesIfAny();
}

void ImageCopyCaptureSessionImpl::handleCursorHidden()
{
    m_cursorHasChanged = true;
    sendFrameUpdatesIfAny();
}

void ImageCopyCaptureSessionImpl::handleCursorMoved(Cursor *cursor, const QPointF &position)
{
    m_cursorHasChanged = true;
    sendFrameUpdatesIfAny();
}

bool ImageCopyCaptureSessionImpl::renderFrame(ImageCopyCaptureFrameV1Interface & frame, const QRectF & cursorBox)
{
    Output* output = m_output.get();
    Q_ASSERT(output != nullptr);

    // Get the compositor texture for the output
    auto [texture, color] = Compositor::self()->textureForOutput(output);
    if (!texture) {
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::unknown);
        return false;
    }

    // Map the client buffer to get direct access to its memory
    auto* clientBuffer = frame.getShmClientBuffer();
    if (!clientBuffer) {
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::buffer_constraints);
        return false;
    }

    // Validate buffer parameters.
    // N.B. bufferAttributes->format is always in the DRM format even for ShmClientBuffer.
    //      DRM_FORMAT_ARGB8888 is the equivalent of WL_SHM_FORMAT_ARGB8888.
    //      See src/wayland/shmclientbuffer.cpp.
    const ShmAttributes* bufferAttributes = clientBuffer->shmAttributes();
    if (bufferAttributes->format != DRM_FORMAT_ARGB8888 ||
        bufferAttributes->size != output->pixelSize() ||
        bufferAttributes->stride != output->pixelSize().width() * 4) {
        qWarning() << "Buffer passed for image copy capture has the wrong format; failing the request";
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::buffer_constraints);
        return false;
    }

    auto mapping = clientBuffer->map(GraphicsBuffer::Write);
    if (!mapping.data) {
        clientBuffer->unmap();
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::unknown);
        return false;
    }

    QImage frameImage{static_cast<uchar*>(mapping.data),
                      bufferAttributes->size.width(),
                      bufferAttributes->size.height(),
                      mapping.stride,
                      QImage::Format_ARGB32_Premultiplied};

    grabTexture(texture.get(), &frameImage);

    // Paint cursor (if requested)
    if (cursorBox.isValid()) {
        const QImage cursorImage = kwinApp()->cursorImage().image();
        if (!cursorImage.isNull()) {
            QPainter painter(&frameImage);
            painter.drawImage(cursorBox, cursorImage);
        }
    }

    // Finish up with frame meta-information
    clientBuffer->unmap();
    frame.sendTransform(WL_OUTPUT_TRANSFORM_NORMAL); // No transform applied
    auto timestamp = m_output->renderLoop()->lastPresentationTimestamp();
    frame.sendPresentationTime(timestamp);

    return true;
}

QRectF ImageCopyCaptureSessionImpl::getCursorBox() const
{
    Q_ASSERT(m_output != nullptr);

    if (!m_cursorHasChanged) {
        return m_lastCursorBox;
    }

    if (m_overlayCursor) {
        const auto* cursors = Cursors::self();
        const Cursor* cursor = cursors->currentCursor();
        if (!cursors->isCursorHidden() && cursor != nullptr) {
            return scaledRect(cursor->geometry(), m_output->scale());
        }
    }

    return QRectF();
}

void ImageCopyCaptureSessionImpl::updateTracking(const QRectF &cursorBox)
{
    m_frameDamage.setRects(QSpan<QRect>());   // clear
    m_cursorHasChanged = false;
    m_lastCursorBox = cursorBox;
}

void ImageCopyCaptureSessionImpl::sendFrameUpdatesIfAny()
{
    // Skip if there are no updates (yet)
    if (m_frameDamage.isEmpty() && !m_cursorHasChanged) {
        return;
    }

    // Skip if the client destroyed the frame
    auto* frame = getCurrentFrame();
    if (frame == nullptr) {
        return;
    }

    // Fail if output has already gone away.
    if (m_output == nullptr) {
        qDebug() << "Output has gone away while a frame is being captured";
        frame->sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::stopped);
        return;
    }

    QRectF newCursorBox = getCursorBox();

    if (!renderFrame(*frame, newCursorBox)) {
        return;
    }

    // The cursor box may exceed the extents of the output, so it needs
    // to be clipped for reporting it as damage to the client.  We also do
    // the same, defensively, for the other rectangles of m_frameDamage.
    auto wholeArea = QRect(QPoint(0, 0), m_output->pixelSize());

    // Damage old and new locations of cursor
    if (m_cursorHasChanged) {
        if (m_lastCursorBox.isValid()) {
            m_frameDamage += m_lastCursorBox.toAlignedRect().intersected(wholeArea);
        }
        if (newCursorBox.isValid()) {
            m_frameDamage += newCursorBox.toAlignedRect().intersected(wholeArea);
        }
    }

    // Send damage event(s).
    if (m_frameDamage.rectCount() <= 8) {
        for (const QRect & rect : m_frameDamage)
            frame->sendDamage(rect.intersected(wholeArea));
    } else {
        frame->sendDamage(m_frameDamage.boundingRect().intersected(wholeArea));
    }

    frame->sendReady();

    updateTracking(newCursorBox);
}

void ImageCopyCaptureSessionImpl::captureFrame()
{
    // Send updates immediately if there are any
    sendFrameUpdatesIfAny();
}

void ImageCopyCaptureSessionImpl::advertiseBufferConstraints()
{
    sendBufferSize(m_output->pixelSize());
    sendShmFormat(WL_SHM_FORMAT_ARGB8888);
    sendConstraintsDone();
}

class ImageCopyCaptureManagerImpl final : public ImageCopyCaptureManagerV1Interface
{
    Q_OBJECT

public:
    ImageCopyCaptureManagerImpl(Display *display, ImageCopyCapturePlugin* parent)
        : ImageCopyCaptureManagerV1Interface(display, parent)
    {
    }

protected:
    ImageCopyCaptureSessionV1Interface* createSession(wl_resource* resource,
                                                      Output *output,
                                                      bool overlayCursor) override
    {
        return new ImageCopyCaptureSessionImpl(resource, this, output, overlayCursor);
    }
};

} // anonymous namespace

//
// Implementation of ImageCopyCapturePlugin
//

ImageCopyCapturePlugin::ImageCopyCapturePlugin()
    : m_imageCopyCaptureManager(std::make_unique<ImageCopyCaptureManagerImpl>(waylandServer()->display(), this))
    , m_outputSourceManager(std::make_unique<OutputImageCaptureSourceManagerV1Interface>(waylandServer()->display(), this))
{
}

ImageCopyCapturePlugin::~ImageCopyCapturePlugin() = default;

} // namespace KWin

#include "imagecopycaptureplugin.moc"
