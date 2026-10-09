/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QPointer>
#include <chrono>

#include <drm_fourcc.h>

#include "imagecopycaptureplugin.h"
#include "outputrenderer.h"
#include "rendertexture.h"

#include "wayland/image_capture_source_v1.h"
#include "wayland/image_copy_capture_v1.h"
#include "wayland/shmclientbuffer_p.h"
#include "wayland_server.h"

#include "core/backendoutput.h"
#include "core/graphicsbuffer.h"
#include "core/output.h"
#include "core/renderloop.h"

#include "compositor.h"
#include "opengl/eglbackend.h"
#include "opengl/eglcontext.h"
#include "opengl/glframebuffer.h"
#include "opengl/gltexture.h"
#include "workspace.h"

namespace KWin
{

namespace
{

class ImageCopyCaptureSessionImpl final : public ImageCopyCaptureSessionV1Interface
{
    Q_OBJECT

public:
    ImageCopyCaptureSessionImpl(wl_resource *resource,
                                ImageCopyCaptureManagerV1Interface *manager,
                                LogicalOutput *output,
                                bool overlayCursor);
    ~ImageCopyCaptureSessionImpl() override;

protected:
    void captureFrame() override;
    void damageClientBuffer(const QRect &damage) override;
    void frameDestroyed() override;

private Q_SLOTS:
    void handleOutputSizeChange();
    void handleOutputRemoved(LogicalOutput *output);
    void handleCompositingAboutToToggle();

    /**
     * @brief Arrange for #sendFrameUpdatesIfAny to run from the event loop.
     *
     * Scene damage is reported while kwin is in the middle of other work (possibly
     * even while we are painting), so we never render directly in response to it.
     * Many reports in a row are coalesced into one update.
     */
    void scheduleFrameUpdate();

private:
    /**
     * @brief The display output that is being captured by this session.
     *
     * Part of the parameters set by the client.
     */
    const QPointer<LogicalOutput> m_output;

    /**
     * @brief Whether the cursor should be rendered as part of the frame.
     *
     * Part of the parameters set by the client.
     */
    const bool m_overlayCursor;

    /**
     * @brief Renders the output's contents into #m_framebuffer and reports damage.
     *
     * Created on first use, and dropped whenever the output or the OpenGL
     * compositor goes away.
     */
    std::unique_ptr<OutputRenderer> m_renderer;

    /**
     * @brief Our own persistent copy of the output's picture, in GPU memory.
     *
     * Only damaged parts are re-painted on each frame, so the texture must
     * persist between frames.  It is reallocated when the output's pixel size changes.
     *
     * In a future version, a client-supplied DMA-BUF may be rendered into instead
     * of this texture.
     */
    std::unique_ptr<GLTexture> m_texture;
    std::unique_ptr<GLFramebuffer> m_framebuffer;

    /**
     * @brief The region of #m_texture whose contents are stale and must be
     *        re-painted regardless of scene damage.
     *
     * Infinite right after (re)allocation; empty once painted.
     */
    Region m_textureRepair;

    /**
     * @brief The region of the output that has been painted into #m_texture since the
     *        last "ready" event was fired, i.e. not yet delivered to the client.
     *
     * This region is expressed in device (physical) pixels.
     *
     * It is kept separately from the renderer's damage tracking so that a frame which
     * fails (e.g. wrong buffer) does not lose the damage for the next frame.
     */
    Region m_pendingDamage;

    /**
     * @brief Bounding rectangle for the region of the client's buffer that needs
     *        to be re-painted for the next captured frame, as requested by the client
     *        through @c damage_buffer.
     *
     * Although the client can enumerate the rectangles of the region, only
     * the bounding rectangle is tracked by this implementation for efficiency.
     * Reading back many small rectangles from the GPU may be slow.
     */
    QRect m_bufferDamage;

    /// True while a call to #sendFrameUpdatesIfAny is queued on the event loop.
    bool m_updateScheduled = false;

    /// Re-entrancy guard for #sendFrameUpdatesIfAny.
    bool m_inFrameUpdate = false;

    /**
     * @brief Send events to advertise the supported buffer formats to the client.
     *
     * Currently only one buffer format, the most straightforward one, is supported.
     */
    void advertiseBufferConstraints();

    /**
     * @brief Make sure #m_renderer, #m_texture and #m_framebuffer exist and match the output.
     *
     * Must be called with the OpenGL context current.
     *
     * @return False if rendering is impossible, e.g. compositing is not OpenGL.
     */
    bool ensureRenderResources();

    /**
     * @brief Release #m_renderer, #m_texture and #m_framebuffer.
     *
     * The next frame will be a full repaint.
     */
    void dropRenderResources();

    /**
     * @brief Copy the current picture of the output into the frame's buffer, and
     *        send the meta-information on the frame contents, but do not mark
     *        the frame as "ready".
     *
     * Called as part of #sendFrameUpdatesIfAny.
     *
     * @param frame    The frame to copy into.  Copying will fail
     *                 if the frame does not have an attached buffer
     *                 or it is not of the correct format.
     * @param clipBox  The rectangle to copy into the frame's buffer, in device pixels.
     *                 Areas outside will remain unchanged.
     *                 Must lie within the output's area.
     *
     * @return True if copying was successful. False if the frame has been marked
     *         has failed.
     */
    bool copyToFrame(ImageCopyCaptureFrameV1Interface &frame, const QRect &clipBox);

    /**
     * @brief Send to the client the contents of the current output, if there
     *        is a frame being captured and there is anything new to send.
     */
    void sendFrameUpdatesIfAny();
};

ImageCopyCaptureSessionImpl::ImageCopyCaptureSessionImpl(wl_resource *resource,
                                                         ImageCopyCaptureManagerV1Interface *manager,
                                                         LogicalOutput *output,
                                                         bool overlayCursor)
    : ImageCopyCaptureSessionV1Interface(resource, manager)
    , m_output(output)
    , m_overlayCursor(overlayCursor)
{
    connect(output, &LogicalOutput::geometryChanged, this, &ImageCopyCaptureSessionImpl::handleOutputSizeChange);
    connect(output, &LogicalOutput::scaleChanged, this, &ImageCopyCaptureSessionImpl::handleOutputSizeChange);
    connect(output, &LogicalOutput::transformChanged, this, &ImageCopyCaptureSessionImpl::handleOutputSizeChange);
    connect(workspace(), &Workspace::outputRemoved, this, &ImageCopyCaptureSessionImpl::handleOutputRemoved);
    connect(Compositor::self(), &Compositor::aboutToToggleCompositing, this, &ImageCopyCaptureSessionImpl::handleCompositingAboutToToggle);
    connect(Compositor::self(), &Compositor::aboutToDestroy, this, &ImageCopyCaptureSessionImpl::handleCompositingAboutToToggle);

    advertiseBufferConstraints();
}

ImageCopyCaptureSessionImpl::~ImageCopyCaptureSessionImpl()
{
    dropRenderResources();
}

void ImageCopyCaptureSessionImpl::advertiseBufferConstraints()
{
    sendBufferSize(m_output->pixelSize());
    sendShmFormat(WL_SHM_FORMAT_ARGB8888);
    sendConstraintsDone();
}

void ImageCopyCaptureSessionImpl::handleOutputSizeChange()
{
    advertiseBufferConstraints();

    // The texture is reallocated lazily (see ensureRenderResources) if the pixel size
    // actually changed.  The attempt below will then likely fail with buffer_constraints,
    // which is what tells the client to re-read the constraints.  But if the pixel size
    // did not change (e.g. the output only moved), the frame may well succeed.
    scheduleFrameUpdate();
}

void ImageCopyCaptureSessionImpl::scheduleFrameUpdate()
{
    if (m_updateScheduled || getCurrentFrame() == nullptr) {
        return;
    }
    m_updateScheduled = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_updateScheduled = false;
        sendFrameUpdatesIfAny();
    }, Qt::QueuedConnection);
}

void ImageCopyCaptureSessionImpl::handleOutputRemoved(LogicalOutput *output)
{
    if (output != m_output) {
        return;
    }
    // The scene view must not outlive the output.
    dropRenderResources();
    if (auto *frame = getCurrentFrame()) {
        frame->sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::stopped);
    }
    sendStopped();
}

void ImageCopyCaptureSessionImpl::handleCompositingAboutToToggle()
{
    // The OpenGL context and scene are going away.  Everything is rebuilt on the next frame.
    dropRenderResources();
}

bool ImageCopyCaptureSessionImpl::ensureRenderResources()
{
    Q_ASSERT(m_output);

    if (!m_renderer) {
        if (!OutputRenderer::isSupported()) {
            return false;
        }
        m_renderer = std::make_unique<OutputRenderer>(m_output, m_overlayCursor);
        connect(m_renderer.get(), &OutputRenderer::contentChanged, this, &ImageCopyCaptureSessionImpl::scheduleFrameUpdate);
    }

    const QSize size = m_output->pixelSize();
    if (!m_texture || m_texture->size() != size) {
        m_framebuffer.reset();
        m_texture = GLTexture::allocate(GL_RGBA8, size);
        if (!m_texture) {
            return false;
        }
        m_framebuffer = std::make_unique<GLFramebuffer>(m_texture.get());
        m_textureRepair = Region::infinite();
    }

    return true;
}

void ImageCopyCaptureSessionImpl::dropRenderResources()
{
    m_renderer.reset();
    m_framebuffer.reset();
    m_texture.reset();
    m_textureRepair = Region::infinite();
}

bool ImageCopyCaptureSessionImpl::copyToFrame(ImageCopyCaptureFrameV1Interface &frame, const QRect &clipBox)
{
    Q_ASSERT(m_output && m_texture);

    const QSize outputSize = m_output->pixelSize();
    Q_ASSERT(QRect(QPoint(), outputSize).contains(clipBox));

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

    // Map in the client buffer
    auto mapping = clientBuffer->map(GraphicsBuffer::Write);
    if (!mapping.data) {
        clientBuffer->unmap();
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::unknown);
        return false;
    }

    // Set up targetImage to include only the clipped area
    auto imageStart = static_cast<uchar *>(mapping.data)
        + clipBox.top() * mapping.stride
        + clipBox.left() * 4;
    QImage targetImage{imageStart,
                       clipBox.width(),
                       clipBox.height(),
                       mapping.stride,
                       QImage::Format_ARGB32_Premultiplied};

    const bool ok = readTextureRegion(*m_texture, targetImage, clipBox.topLeft());
    clientBuffer->unmap();
    if (!ok) {
        frame.sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::unknown);
        return false;
    }

    // readTextureRegion always delivers the picture upright.
    frame.sendTransform(WL_OUTPUT_TRANSFORM_NORMAL);

    if (BackendOutput *backendOutput = m_output->backendOutput()) {
        frame.sendPresentationTime(backendOutput->renderLoop()->lastPresentationTimestamp());
    } else {
        frame.sendPresentationTime(std::chrono::steady_clock::now().time_since_epoch());
    }

    return true;
}

void ImageCopyCaptureSessionImpl::sendFrameUpdatesIfAny()
{
    if (m_inFrameUpdate) {
        return;
    }
    struct Guard
    {
        bool &flag;
        ~Guard()
        {
            flag = false;
        }
    } guard{m_inFrameUpdate};
    m_inFrameUpdate = true;

    // Skip if the client has no frame waiting to be captured
    auto *frame = getCurrentFrame();
    if (frame == nullptr) {
        return;
    }

    // Fail if output has already gone away.
    LogicalOutput *output = m_output.get();
    if (output == nullptr) {
        qDebug() << "Output has gone away while a frame is being captured";
        frame->sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::stopped);
        return;
    }

    // Skip cheaply if nothing has changed since the last frame (the common case when
    // the scene reports damage that turns out to be outside this output).
    if (m_renderer && !m_renderer->hasPendingChanges() && m_textureRepair.isEmpty() && m_pendingDamage.isEmpty()) {
        return;
    }

    // Rendering happens in kwin's OpenGL context, which is not necessarily current
    // when we are called in response to a request from the Wayland client.
    auto *backend = qobject_cast<EglBackend *>(Compositor::self()->backend());
    if (!backend || !backend->openglContext()->makeCurrent()) {
        qWarning() << "No OpenGL context available for image copy capture";
        frame->sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::unknown);
        return;
    }

    if (!ensureRenderResources()) {
        qWarning() << "Image copy capture requires OpenGL compositing";
        frame->sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::unknown);
        return;
    }

    // Bring our texture up to date if anything changed on screen (or the texture is new).
    if (m_renderer->hasPendingChanges() || !m_textureRepair.isEmpty()) {
        const auto painted = m_renderer->render(m_framebuffer.get(), m_textureRepair);
        if (!painted) {
            frame->sendFailed(ImageCopyCaptureFrameV1Interface::FailureReason::unknown);
            return;
        }
        m_textureRepair = Region();
        m_pendingDamage |= *painted;
    }

    // Skip if there are no updates (yet).  The first frame of a session always has
    // the whole output as pending damage, so it is sent immediately.
    if (m_pendingDamage.isEmpty()) {
        return;
    }

    // Copy everything that is new to us, plus what the client said is stale in its buffer.
    const Rect wholeArea(QPoint(), output->pixelSize());
    const QRect clipBox = (m_pendingDamage.boundingRect() | Rect(m_bufferDamage)) & wholeArea;
    if (clipBox.isEmpty()) {
        return;
    }

    if (!copyToFrame(*frame, clipBox)) {
        // m_pendingDamage is kept, so the next frame will include these changes.
        return;
    }

    // Send damage event(s).  Do not send too many.
    const auto rects = m_pendingDamage.rects();
    if (rects.size() <= 32) {
        for (const Rect &rect : rects) {
            frame->sendDamage(rect & wholeArea);
        }
    } else {
        frame->sendDamage(m_pendingDamage.boundingRect() & wholeArea);
    }

    frame->sendReady();

    m_pendingDamage = Region();
    m_bufferDamage = QRect();
}

void ImageCopyCaptureSessionImpl::damageClientBuffer(const QRect &damage)
{
    m_bufferDamage |= damage;
}

void ImageCopyCaptureSessionImpl::frameDestroyed()
{
    // The client will report damage anew for its next frame's buffer
    m_bufferDamage = QRect();
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
                                                      LogicalOutput *output,
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
