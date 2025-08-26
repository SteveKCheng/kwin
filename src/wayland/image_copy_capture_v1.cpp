/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "image_copy_capture_v1.h"
#include "image_capture_source_v1.h"
#include "display.h"
#include "output.h"
#include "wayland/shmclientbuffer_p.h"

#include <QPointer>
#include <chrono>

#include "qwayland-server-ext-image-copy-capture-v1.h"

namespace KWin
{

static const int s_version = 1;

class ImageCopyCaptureManagerV1InterfacePrivate final : public QtWaylandServer::ext_image_copy_capture_manager_v1
{
    /**
     * @brief Pointer to the parent of this pimpl, required for calling its virtual methods.
     */
    ImageCopyCaptureManagerV1Interface * const m_parent;

public:
    ImageCopyCaptureManagerV1InterfacePrivate(ImageCopyCaptureManagerV1Interface *q, Display *display)
        : QtWaylandServer::ext_image_copy_capture_manager_v1(*display, s_version)
        , m_parent(q)
    {
    }

protected:
    void ext_image_copy_capture_manager_v1_destroy(Resource *resource) override
    {
        wl_resource_destroy(resource->handle);
    }

    void ext_image_copy_capture_manager_v1_create_session(Resource *resource,
                                                         uint32_t session_id,
                                                         struct ::wl_resource *source_resource,
                                                         uint32_t options) override;

    void ext_image_copy_capture_manager_v1_create_pointer_cursor_session(Resource *resource,
                                                                        uint32_t session_id,
                                                                        struct ::wl_resource *source,
                                                                        struct ::wl_resource *pointer) override
    {
        // Not implemented in this first version - could be added later
        wl_resource_post_error(resource->handle, QtWaylandServer::ext_image_copy_capture_manager_v1::error_invalid_option, 
                              "pointer cursor sessions not yet supported");
    }
};

class ImageCopyCaptureFrameV1InterfacePrivate final : public QtWaylandServer::ext_image_copy_capture_frame_v1
{
    /**
     * @brief Pointer to the owner of this pimpl, required for deletion only.
     */
    ImageCopyCaptureFrameV1Interface* const m_parent;

public:
    ImageCopyCaptureFrameV1InterfacePrivate(ImageCopyCaptureFrameV1Interface* parent, ImageCopyCaptureSessionV1Interface *session)
        : m_parent(parent)
        , m_session(session)
    {
    }

    QPointer<ShmClientBuffer> m_shmClientBuffer;

    enum class Stage
    {
        NotCreated,     ///< No current frame
        Created,        ///< Client created the frame, but not capturing yet
        Capturing,      ///< Capturing was requested by the client
        Finished,       ///< Capturing is finished (success or failure)
    };

    /**
     * @brief Overall state of the frame for its capture session.
     *
     * This must be tracked for checking errors in usage, and because a single C++ object
     * is re-used for all frames created by the client for the session.
     */
    Stage m_stage = Stage::NotCreated;

    /**
     * @brief Pointer to the session that owns this frame, required for calling
     *        its virtual methods.
     *
     * Set to null once the session has gone away.  If non-null, the session is alive.
     */
    ImageCopyCaptureSessionV1Interface * m_session;

protected:
    void ext_image_copy_capture_frame_v1_destroy_resource(Resource *resource) override
    {
        if (m_session == nullptr) {
            // The session has gone away, and thus this object is no longer owned by it
            delete m_parent;
        }
    }

    void ext_image_copy_capture_frame_v1_destroy(Resource *resource) override
    {
        m_stage = Stage::NotCreated;
        wl_resource_destroy(resource->handle);
    }

    void ext_image_copy_capture_frame_v1_attach_buffer(Resource *resource, struct ::wl_resource *buffer) override
    {
        Q_ASSERT(m_stage != Stage::NotCreated);
        if (m_stage != Stage::Created) {
            wl_resource_post_error(resource->handle, error_already_captured, "capture already requested");
            return;
        }

        auto* shmClientBuffer = ShmClientBuffer::get(buffer);
        if (!shmClientBuffer) {
            // For now we only support SHM buffers
            wl_resource_post_error(resource->handle, error_no_buffer, "only SHM buffers supported");
            return;
        }

        m_shmClientBuffer = shmClientBuffer;
    }

    void ext_image_copy_capture_frame_v1_damage_buffer(Resource *resource,
                                                      int32_t x, int32_t y,
                                                      int32_t width, int32_t height) override
    {
        Q_ASSERT(m_stage != Stage::NotCreated);
        if (m_stage != Stage::Created) {
            wl_resource_post_error(resource->handle, error_already_captured, "capture already requested");
            return;
        }

        if (x < 0 || y < 0 || width <= 0 || height <= 0) {
            wl_resource_post_error(resource->handle, error_invalid_buffer_damage, "invalid damage region");
            return;
        }

        // Silently ignore if the session already has been destroyed.
        // This frame cannot be captured anyway so client damage is irrelevant.
        if (m_session == nullptr) {
            return;
        }

        m_session->damageClientBuffer(QRect(x, y, width, height));
    }

    void ext_image_copy_capture_frame_v1_capture(Resource *resource) override
    {
        Q_ASSERT(m_stage != Stage::NotCreated);
        if (m_stage != Stage::Created) {
            wl_resource_post_error(resource->handle, error_already_captured, "capture already requested");
            return;
        }

        if (!m_shmClientBuffer) {
            wl_resource_post_error(resource->handle, error_no_buffer, "no buffer attached");
            return;
        }

        if (m_session == nullptr) {
            // Session was destroyed first.  Cannot capture.
            send_failed(failure_reason_stopped);
            return;
        }

        m_stage = Stage::Capturing;
        m_session->captureFrame();
    }
};

class ImageCopyCaptureSessionV1InterfacePrivate final : public QtWaylandServer::ext_image_copy_capture_session_v1
{
    /**
     * @brief Pointer to the owner of this private pimpl.
     */
    ImageCopyCaptureSessionV1Interface * const m_parent;

public:
    ImageCopyCaptureSessionV1InterfacePrivate(ImageCopyCaptureSessionV1Interface *q)
        : m_parent(q)
        , m_currentFrame(new ImageCopyCaptureFrameV1Interface(q))
    {
    }

    ~ImageCopyCaptureSessionV1InterfacePrivate()
    {
        auto* d = m_currentFrame->d.get();

        // No outstanding frame from client's point of view.  Can de-allocate C++ object.
        if (d->m_stage == ImageCopyCaptureFrameV1InterfacePrivate::Stage::NotCreated)
            delete m_currentFrame;

        // Leave a "dangling" frame which will delete itself when the client destroys it.
        else
            d->m_session = nullptr;
    }

    /**
     * @brief Object for the frame associated to this session.
     *
     * The protocol only allows at most one frame to exist for any session so we can optimize
     * by allocating only one object and re-using it.  The logical state of the frame
     * is tracked in ImageCopyCaptureFrameV1InterfacePrivate::m_stage.
     *
     * When this session is deleted, the protocol says an outstanding frame from the client's point
     * of view is "not affected" (although, clearly, no capturing can happen subsequently).
     * To implement this requirement, ownership of the frame is passed from this session
     * to the frame itself in the destructor of the session.
     */
    ImageCopyCaptureFrameV1Interface * const m_currentFrame;

    /**
     * @brief Whether this C++ object is responsible for destroying the corresponding Wayland resource
     *        for the capture session.
     *
     * Normally it is responsible, but when the client requests to create a session,
     * the Wayland resource is created first before a derived instance of this class
     * is constructed.  If failure occurs, i.e. the instance is not constructed,
     * the Wayland resource must be destroyed, so the caller of
     * ImageCopyCaptureManager::createSession must temporarily have ownership
     * of that resource.  Yet, we need the Wayland resource be available so
     * the implementation of ImageCopyCaptureManager::createSession can send
     * buffer constraints; thus at the time this object must borrow the Wayland
     * resource, and this flag is set to false to reflect that.  Once
     * ownership has been passed to this object, this flag is set to true.
     */
    bool m_ownsResource = false;

    ImageCopyCaptureFrameV1Interface* getCurrentFrame();

protected:
    void ext_image_copy_capture_session_v1_destroy_resource(Resource *resource) override
    {
        delete m_parent;
    }

    void ext_image_copy_capture_session_v1_destroy(Resource *resource) override
    {
        if (m_ownsResource) {
            wl_resource_destroy(resource->handle);
            m_ownsResource = false;
        }
    }

    void ext_image_copy_capture_session_v1_create_frame(Resource *resource, uint32_t frame_id) override;
};

//
// Implementation of ImageCopyCaptureManagerV1Interface
//

void ImageCopyCaptureManagerV1InterfacePrivate::ext_image_copy_capture_manager_v1_create_session(Resource *resource, uint32_t session_id, wl_resource *source_resource, uint32_t options)
{
    // Extract the ImageCaptureSourceV1Interface from the wl_resource
    ImageCaptureSourceV1Interface *sourceInterface = ImageCaptureSourceV1Interface::get(source_resource);
    if (!sourceInterface) {
        wl_resource_post_error(resource->handle, WL_DISPLAY_ERROR_INVALID_OBJECT, "invalid image capture source");
        return;
    }

    // For now, we only support output sources
    if (sourceInterface->sourceType() != ImageCaptureSourceV1Interface::SourceType::Output) {
        wl_resource_post_error(resource->handle, WL_DISPLAY_ERROR_INVALID_OBJECT, "unsupported source type");
        return;
    }

    Output *output = sourceInterface->output();
    if (!output) {
        wl_resource_post_error(resource->handle, WL_DISPLAY_ERROR_INVALID_OBJECT, "source has no valid output");
        return;
    }

    wl_resource *sessionResource = wl_resource_create(resource->client(), &ext_image_copy_capture_session_v1_interface, resource->version(), session_id);
    if (!sessionResource) {
        wl_resource_post_no_memory(resource->handle);
        return;
    }

    bool overlayCursor = (options & QtWaylandServer::ext_image_copy_capture_manager_v1::options_paint_cursors) != 0;

    auto *session = m_parent->createSession(sessionResource, output, overlayCursor);
    if (!session) {
        wl_resource_destroy(sessionResource);
        return;
    }

    // Pass ownership of sessionResource to new object only after all initialization is successful
    session->d->m_ownsResource = true;
}

ImageCopyCaptureManagerV1Interface::ImageCopyCaptureManagerV1Interface(Display *display, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<ImageCopyCaptureManagerV1InterfacePrivate>(this, display))
{
}

ImageCopyCaptureManagerV1Interface::~ImageCopyCaptureManagerV1Interface() = default;

//
// Implementation of ImageCopyCaptureSessionV1Interface
//

void ImageCopyCaptureSessionV1InterfacePrivate::ext_image_copy_capture_session_v1_create_frame(Resource *resource, uint32_t frame_id)
{
    auto* d = m_currentFrame->d.get();

    if (d->m_stage != ImageCopyCaptureFrameV1InterfacePrivate::Stage::NotCreated) {
        wl_resource_post_error(resource->handle, error_duplicate_frame, "frame already exists for this session");
        return;
    }

    wl_resource *frameResource = wl_resource_create(resource->client(), &ext_image_copy_capture_frame_v1_interface, resource->version(), frame_id);
    if (!frameResource) {
        wl_resource_post_no_memory(resource->handle);
        return;
    }

    d->init(frameResource);
    d->m_stage = ImageCopyCaptureFrameV1InterfacePrivate::Stage::Created;
}

ImageCopyCaptureSessionV1Interface::ImageCopyCaptureSessionV1Interface(wl_resource* resource,
                                                                       ImageCopyCaptureManagerV1Interface *manager)
    : QObject(manager)
    , d(std::make_unique<ImageCopyCaptureSessionV1InterfacePrivate>(this))
{
    d->init(resource);
}

ImageCopyCaptureSessionV1Interface::~ImageCopyCaptureSessionV1Interface() = default;

void ImageCopyCaptureSessionV1Interface::sendBufferSize(const QSize &size)
{
    d->send_buffer_size(size.width(), size.height());
}

void ImageCopyCaptureSessionV1Interface::sendShmFormat(uint32_t format)
{
    d->send_shm_format(format);
}

void ImageCopyCaptureSessionV1Interface::sendDmaBufDevice(const QByteArray &device)
{
    d->send_dmabuf_device(device);
}

void ImageCopyCaptureSessionV1Interface::sendDmaBufFormat(uint32_t format, const QByteArray &modifiers)
{
    d->send_dmabuf_format(format, modifiers);
}

void ImageCopyCaptureSessionV1Interface::sendConstraintsDone()
{
    d->send_done();
}

void ImageCopyCaptureSessionV1Interface::sendStopped()
{
    d->send_stopped();
}

ImageCopyCaptureFrameV1Interface* ImageCopyCaptureSessionV1Interface::getCurrentFrame() const
{
    return d->getCurrentFrame();
}

ImageCopyCaptureFrameV1Interface *ImageCopyCaptureSessionV1InterfacePrivate::getCurrentFrame()
{
    return (m_currentFrame->d->m_stage == ImageCopyCaptureFrameV1InterfacePrivate::Stage::Capturing)
            ? m_currentFrame
            : nullptr;
}

//
// Implementation of ImageCopyCaptureFrameV1Interface
//

ImageCopyCaptureFrameV1Interface::ImageCopyCaptureFrameV1Interface(ImageCopyCaptureSessionV1Interface* session)
    : d(std::make_unique<ImageCopyCaptureFrameV1InterfacePrivate>(this, session))
{
}

ImageCopyCaptureFrameV1Interface::~ImageCopyCaptureFrameV1Interface() = default;

ShmClientBuffer* ImageCopyCaptureFrameV1Interface::getShmClientBuffer()
{
    Q_ASSERT(d->m_stage == ImageCopyCaptureFrameV1InterfacePrivate::Stage::Capturing);
    return d->m_shmClientBuffer.get();
}

void ImageCopyCaptureFrameV1Interface::sendTransform(uint32_t transform)
{
    Q_ASSERT(d->m_stage == ImageCopyCaptureFrameV1InterfacePrivate::Stage::Capturing);
    d->send_transform(transform);
}

void ImageCopyCaptureFrameV1Interface::sendDamage(const QRect &rect)
{
    Q_ASSERT(d->m_stage == ImageCopyCaptureFrameV1InterfacePrivate::Stage::Capturing);
    d->send_damage(rect.x(), rect.y(), rect.width(), rect.height());
}

void ImageCopyCaptureFrameV1Interface::sendPresentationTime(std::chrono::nanoseconds timestamp)
{
    Q_ASSERT(d->m_stage == ImageCopyCaptureFrameV1InterfacePrivate::Stage::Capturing);
    uint64_t tv_sec = std::chrono::duration_cast<std::chrono::seconds>(timestamp).count();
    uint32_t tv_sec_hi = tv_sec >> 32;
    uint32_t tv_sec_lo = tv_sec & 0xFFFFFFFF;
    uint32_t tv_nsec = (timestamp % std::chrono::seconds(1)).count();
    d->send_presentation_time(tv_sec_hi, tv_sec_lo, tv_nsec);
}

void ImageCopyCaptureFrameV1Interface::sendReady()
{
    Q_ASSERT(d->m_stage == ImageCopyCaptureFrameV1InterfacePrivate::Stage::Capturing);
    d->m_stage = ImageCopyCaptureFrameV1InterfacePrivate::Stage::Finished;
    d->send_ready();
}

void ImageCopyCaptureFrameV1Interface::sendFailed(FailureReason reason)
{
    Q_ASSERT(d->m_stage == ImageCopyCaptureFrameV1InterfacePrivate::Stage::Capturing);
    d->m_stage = ImageCopyCaptureFrameV1InterfacePrivate::Stage::Finished;
    d->send_failed(static_cast<uint>(reason));
}

} // namespace KWin

#include "moc_image_copy_capture_v1.cpp"
