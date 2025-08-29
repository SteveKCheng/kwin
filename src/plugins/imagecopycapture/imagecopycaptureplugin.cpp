/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QPainter>
#include <QTimer>
#include <chrono>

#include <drm_fourcc.h>

#include "imagecopycaptureplugin.h"
#include "wayland/image_capture_source_v1.h"
#include "wayland/image_copy_capture_v1.h"
#include "wayland/output.h"
#include "wayland/shmclientbuffer_p.h"
#include "wayland_server.h"

#include "core/graphicsbuffer.h"
#include "core/output.h"

#include "compositor.h"
#include "cursor.h"
#include "main.h"

namespace KWin
{

// Defined in rendertexture.cpp
bool renderTextureToImage(GLTexture &texture, QImage &target, const QPoint &topLeft, bool &isInverted);

namespace
{

class ImageCopyCaptureSessionImpl final : public ImageCopyCaptureSessionV1Interface
{
    Q_OBJECT

public:
    ImageCopyCaptureSessionImpl(wl_resource *resource,
                                ImageCopyCaptureManagerV1Interface *manager,
                                Output *output,
                                bool overlayCursor);

protected:
    void captureFrame() override;
    void damageClientBuffer(const QRect &damage) override;

private Q_SLOTS:
    void handleOutputDamage(const QRegion &damageLogical);
    void handleOutputSizeChange();
    void handleOutputTransformChange();
    void handleOutputDestroyed();
    void handleCursorChanged(Cursor *cursor);
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
     * @brief Bounding rectangle for the region of the client's buffer that needs
     *        to be re-painted for the next captured frame.
     *
     * Although the client can enumerate the rectangles of the region, only
     * the bounding rectangle is tracked by this implementation for efficiency.
     * Rendering a texture for individual rectangles may be slow.
     */
    QRect m_bufferDamage;

    /**
     * @brief The cumulative region of the Output that has changed since the
     *        last "ready" event was fired.
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
     * @brief Consider the whole frame's area to be damaged for the next captured frame.
     */
    void damageWholeFrame();

    /**
     * @brief Remove accumulated damage to the frame after an update has been
     *        sent successfully.
     *
     * @param cursorBox  The new location of the cursor.
     */
    void clearDamage(const QRectF &cursorBox);

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
     * @param frame    The frame to render into.  Rendering will fail
     *                 if the frame does not have an attached buffer
     *                 or it is not of the correct format.
     * @param clipBox  The rectangle to re-paint in the frame's buffer.
     *                 Areas outside will remain unchanged.
     *                 This box will naturally be clipped against the Output's area.
     * @param cursorBox  The rectangle containing the cursor to overlay
     *                   onto the Output's contents.  The cursor image will
     *                   also be implicitly clipped against the Output's area
     *                   and also @a clipBox.
     *
     * @return True if rendering was successful. False if the frame has been marked
     *         has failed, or an update should not be sent out because
     *         there is no damage within the Output's area.
     */
    bool renderFrame(ImageCopyCaptureFrameV1Interface &frame,
                     const QRect &clipBox,
                     const QRectF &cursorBox);

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

ImageCopyCaptureSessionImpl::ImageCopyCaptureSessionImpl(wl_resource *resource,
                                                         ImageCopyCaptureManagerV1Interface *manager,
                                                         Output *output,
                                                         bool overlayCursor)
    : ImageCopyCaptureSessionV1Interface(resource, manager)
    , m_output(output)
    , m_overlayCursor(overlayCursor)
{
    // All output contents are damaged at start
    damageWholeFrame();

    connect(output, &Output::outputChange, this, &ImageCopyCaptureSessionImpl::handleOutputDamage);
    connect(output, &Output::geometryChanged, this, &ImageCopyCaptureSessionImpl::handleOutputSizeChange);
    connect(output, &Output::scaleChanged, this, &ImageCopyCaptureSessionImpl::handleOutputSizeChange);
    connect(output, &Output::destroyed, this, &ImageCopyCaptureSessionImpl::handleOutputDestroyed);

    if (overlayCursor) {
        auto *cursors = Cursors::self();
        connect(cursors, &Cursors::currentCursorChanged, this, &ImageCopyCaptureSessionImpl::handleCursorChanged);
        connect(cursors, &Cursors::hiddenChanged, this, &ImageCopyCaptureSessionImpl::handleCursorHidden);
        connect(cursors, &Cursors::positionChanged, this, &ImageCopyCaptureSessionImpl::handleCursorMoved);
    }

    advertiseBufferConstraints();
}

void ImageCopyCaptureSessionImpl::damageWholeFrame()
{
    Q_ASSERT(m_output);

    m_frameDamage.setRects(QSpan<const QRect>()); // clear
    m_frameDamage += QRect(QPoint(), m_output->pixelSize());
    m_cursorHasChanged = true;
    m_lastCursorBox = QRectF();
}

void ImageCopyCaptureSessionImpl::clearDamage(const QRectF &cursorBox)
{
    m_bufferDamage = QRect();

    m_frameDamage.setRects(QSpan<const QRect>()); // clear
    m_cursorHasChanged = false;
    m_lastCursorBox = cursorBox;
}

void ImageCopyCaptureSessionImpl::handleOutputDamage(const QRegion &damageLogical)
{
    auto outputScale = m_output->scale();
    for (const auto &rectLogical : damageLogical) {
        auto rectPhysical = scaledRect(rectLogical.toRectF(), outputScale);
        m_frameDamage += rectPhysical.toAlignedRect();
    }

    sendFrameUpdatesIfAny();
}

void ImageCopyCaptureSessionImpl::handleOutputSizeChange()
{
    damageWholeFrame();
    advertiseBufferConstraints();

    // This will likely fail because the constraints have changed,
    // but it is possible that the constraints have not changed.
    sendFrameUpdatesIfAny();
}

void ImageCopyCaptureSessionImpl::handleOutputTransformChange()
{
    damageWholeFrame();
    sendFrameUpdatesIfAny();
}

void ImageCopyCaptureSessionImpl::handleOutputDestroyed()
{
    sendStopped();
}

void ImageCopyCaptureSessionImpl::handleCursorChanged(Cursor *cursor)
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

bool ImageCopyCaptureSessionImpl::renderFrame(ImageCopyCaptureFrameV1Interface &frame,
                                              const QRect &clipBox,
                                              const QRectF &cursorBox)
{
    Output *output = m_output.get();
    Q_ASSERT(output != nullptr);

    auto outputSize = output->pixelSize();

    // Ensure pointer arithmetic below does not go out of bounds
    auto inBoundsClipBox = clipBox.intersected(QRect(QPoint(), outputSize));

    // An empty inBoundsClipBox can happen if the cursor is entirely off the screen (Output).
    // sendFrameUpdatesIfAny relies on this method to clip to the Output's area.
    // In this case, we should not send an update at all and keep waiting for additional
    // damage.  We return false so sendFrameUpdatesIfAny does not mark the frame
    // as ready, although this case is clearly not a failure.
    if (inBoundsClipBox.isEmpty()) {
        return false;
    }

    auto *clientBuffer = frame.getShmClientBuffer();
    if (!clientBuffer) {
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::buffer_constraints);
        return false;
    }

    // Validate buffer parameters.
    // N.B. bufferAttributes->format is always in the DRM format even for ShmClientBuffer.
    //      DRM_FORMAT_ARGB8888 is the equivalent of WL_SHM_FORMAT_ARGB8888.
    //      See src/wayland/shmclientbuffer.cpp.
    const ShmAttributes *bufferAttributes = clientBuffer->shmAttributes();
    if (!(bufferAttributes->format == DRM_FORMAT_ARGB8888 && bufferAttributes->size == outputSize && (bufferAttributes->stride % 4) == 0 && (bufferAttributes->stride / 4) >= outputSize.width())) {
        qWarning() << "Buffer passed for image copy capture has the wrong format; failing the request";
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::buffer_constraints);
        return false;
    }

    // Get the compositor texture for the output
    auto [texture, color] = Compositor::self()->textureForOutput(output);
    if (!texture) {
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::unknown);
        return false;
    }

    // Map in the client buffer to start compositing
    auto mapping = clientBuffer->map(GraphicsBuffer::Write);
    if (!mapping.data) {
        clientBuffer->unmap();
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::unknown);
        return false;
    }

    // Set up targetImage to include only the clipped area
    auto imageStart = static_cast<uchar *>(mapping.data)
        + inBoundsClipBox.top() * mapping.stride
        + inBoundsClipBox.left() * 4;
    QImage targetImage{imageStart,
                       inBoundsClipBox.width(),
                       inBoundsClipBox.height(),
                       mapping.stride,
                       QImage::Format_ARGB32_Premultiplied};

    bool isInverted;
    if (!renderTextureToImage(*texture, targetImage, inBoundsClipBox.topLeft(), isInverted)) {
        clientBuffer->unmap();
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::unknown);
        return false;
    }

    // Paint cursor (if requested)
    if (cursorBox.isValid()) {
        const QImage cursorImage = kwinApp()->cursorImage().image();

        if (!cursorImage.isNull()) {
            // cursorBox in the "local" coordinates of inBoundsClipBox.
            auto cursorBoxLocal = cursorBox.translated(-inBoundsClipBox.topLeft().toPointF());

            QPainter painter(&targetImage);

            // Clip against cursorBoxLocal in case the cursor image has a different size than
            // what the caller calculated as damage. (Defensive programming, should not happen.)
            painter.setClipRect(cursorBoxLocal.toAlignedRect());

            painter.drawImage(cursorBoxLocal.topLeft(), cursorImage);
        }
    }

    clientBuffer->unmap();

    frame.sendTransform(isInverted ? WL_OUTPUT_TRANSFORM_FLIPPED
                                   : WL_OUTPUT_TRANSFORM_NORMAL);

    auto timestamp = output->renderLoop()->lastPresentationTimestamp();
    frame.sendPresentationTime(timestamp);

    return true;
}

QRectF ImageCopyCaptureSessionImpl::getCursorBox() const
{
    Output *output = m_output.get();
    Q_ASSERT(output != nullptr);

    if (!m_cursorHasChanged) {
        return m_lastCursorBox;
    }

    if (m_overlayCursor) {
        const auto *cursors = Cursors::self();
        const Cursor *cursor = cursors->currentCursor();
        if (!cursors->isCursorHidden() && cursor != nullptr) {
            return scaledRect(output->mapFromGlobal(cursor->geometry()), output->scale());
        }
    }

    return QRectF();
}

void ImageCopyCaptureSessionImpl::sendFrameUpdatesIfAny()
{
    // Skip if there are no updates (yet)
    if (m_frameDamage.isEmpty() && !m_cursorHasChanged) {
        return;
    }

    // Skip if the client destroyed the frame
    auto *frame = getCurrentFrame();
    if (frame == nullptr) {
        m_bufferDamage = QRect(); // client will refresh on new frame
        return;
    }

    // Fail if output has already gone away.
    Output *output = m_output.get();
    if (output == nullptr) {
        qDebug() << "Output has gone away while a frame is being captured";
        frame->sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::stopped);
        return;
    }

    QRectF newCursorBox = getCursorBox();

    // Damage the old and new locations of the cursor.
    //
    // If rendering fails below, we still add to m_frameDamage, which only
    // incurs a small inefficiency and does not cause incorrect behavior.
    //
    // Note the rectangles may be invalid which cause no damage to be added.
    if (m_cursorHasChanged) {
        m_frameDamage += m_lastCursorBox.toAlignedRect();
        m_frameDamage += newCursorBox.toAlignedRect();
    }

    auto frameDamageBounds = m_frameDamage.boundingRect();

    QRect clipBox = m_bufferDamage.united(frameDamageBounds);
    if (!renderFrame(*frame, clipBox, newCursorBox)) {
        return;
    }

    // The cursor box may exceed the extents of the output, so it needs
    // to be clipped for reporting it as damage to the client.  We also do
    // the same, defensively, for the other rectangles of m_frameDamage.
    auto wholeArea = QRect(QPoint(), output->pixelSize());

    // Send damage event(s).  Do not send too many.
    if (m_frameDamage.rectCount() <= 32) {
        for (const QRect &rect : m_frameDamage) {
            frame->sendDamage(rect.intersected(wholeArea));
        }
    } else {
        frame->sendDamage(frameDamageBounds.intersected(wholeArea));
    }

    frame->sendReady();

    clearDamage(newCursorBox);
}

void ImageCopyCaptureSessionImpl::damageClientBuffer(const QRect &damage)
{
    m_bufferDamage |= damage;
}

void ImageCopyCaptureSessionImpl::advertiseBufferConstraints()
{
    sendBufferSize(m_output->pixelSize());
    sendShmFormat(WL_SHM_FORMAT_ARGB8888);
    sendConstraintsDone();
}

void ImageCopyCaptureSessionImpl::captureFrame()
{
    // Send updates immediately if there are any
    sendFrameUpdatesIfAny();
}

class ImageCopyCaptureManagerImpl final : public ImageCopyCaptureManagerV1Interface
{
    Q_OBJECT

public:
    ImageCopyCaptureManagerImpl(Display *display, ImageCopyCapturePlugin *parent)
        : ImageCopyCaptureManagerV1Interface(display, parent)
    {
    }

protected:
    ImageCopyCaptureSessionV1Interface *createSession(wl_resource *resource,
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
