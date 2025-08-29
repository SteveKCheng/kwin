/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "image_copy_capture_v1.h"
#include "display.h"
#include "image_capture_source_v1.h"
#include "output.h"
#include "wayland/output.h"
#include "wayland/shmclientbuffer_p.h"

#include <QPointer>

#include <chrono>

#include "qwayland-server-ext-image-copy-capture-v1.h"

namespace KWin
{

static const int s_version = 1;

class ImageCopyCaptureManagerV1InterfacePrivate final : public QtWaylandServer::ext_image_copy_capture_manager_v1
{
public:
    ImageCopyCaptureManagerV1InterfacePrivate(ImageCopyCaptureManagerV1Interface *q, Display *display)
        : QtWaylandServer::ext_image_copy_capture_manager_v1(*display, s_version)
        , m_parent(q)
    {
    }

private:
    ImageCopyCaptureManagerV1Interface *const m_parent;

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

class ImageCopyCaptureSessionV1InterfacePrivate final : public QtWaylandServer::ext_image_copy_capture_session_v1
{
public:
    ImageCopyCaptureSessionV1InterfacePrivate(ImageCopyCaptureSessionV1Interface *q)
        : m_parent(q)
    {
    }

    QPointer<ImageCopyCaptureFrameV1Interface> m_currentFrame;
    bool m_ownsResource = false;

    ImageCopyCaptureFrameV1Interface *getCurrentFrame() const;

private:
    /**
     * @brief Pointer to the owner of this private pimpl.
     */
    ImageCopyCaptureSessionV1Interface *const m_parent;

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

class ImageCopyCaptureFrameV1InterfacePrivate final : public QtWaylandServer::ext_image_copy_capture_frame_v1
{
public:
    ImageCopyCaptureFrameV1InterfacePrivate(ImageCopyCaptureFrameV1Interface *q,
                                            ImageCopyCaptureSessionV1Interface *session)
        : m_parent(q)
        , m_session(session)
    {
    }

    QPointer<ShmClientBuffer> m_shmClientBuffer;
    bool m_captureRequested = false;
    bool m_captureDone = false;

private:
    /**
     * @brief Pointer to the owner of this private pimpl, required for deletion.
     */
    ImageCopyCaptureFrameV1Interface *const m_parent;

    /**
     * @brief Pointer to the session that owns this frame.
     */
    ImageCopyCaptureSessionV1Interface *const m_session;

protected:
    void ext_image_copy_capture_frame_v1_destroy_resource(Resource *resource) override
    {
        // Clear the current frame pointer in the session
        if (m_session->d->m_currentFrame == m_parent) {
            m_session->d->m_currentFrame = nullptr;
        }
        delete m_parent;
    }

    void ext_image_copy_capture_frame_v1_destroy(Resource *resource) override
    {
        wl_resource_destroy(resource->handle);
    }

    void ext_image_copy_capture_frame_v1_attach_buffer(Resource *resource, struct ::wl_resource *buffer) override
    {
        if (m_captureRequested) {
            wl_resource_post_error(resource->handle, error_already_captured, "capture already requested");
            return;
        }

        auto *shmClientBuffer = ShmClientBuffer::get(buffer);
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
        if (m_captureRequested) {
            wl_resource_post_error(resource->handle, error_already_captured, "capture already requested");
            return;
        }

        if (x < 0 || y < 0 || width <= 0 || height <= 0) {
            wl_resource_post_error(resource->handle, error_invalid_buffer_damage, "invalid damage region");
            return;
        }

        m_session->damageClientBuffer(QRect(x, y, width, height));
    }

    void ext_image_copy_capture_frame_v1_capture(Resource *resource) override
    {
        if (m_captureRequested) {
            wl_resource_post_error(resource->handle, error_already_captured, "capture already requested");
            return;
        }

        if (!m_shmClientBuffer) {
            wl_resource_post_error(resource->handle, error_no_buffer, "no buffer attached");
            return;
        }

        m_captureRequested = true;
        m_session->captureFrame();
    }
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
    if (m_currentFrame) {
        wl_resource_post_error(resource->handle, error_duplicate_frame, "frame already exists for this session");
        return;
    }

    wl_resource *frameResource = wl_resource_create(resource->client(), &ext_image_copy_capture_frame_v1_interface, resource->version(), frame_id);
    if (!frameResource) {
        wl_resource_post_no_memory(resource->handle);
        return;
    }

    auto *frame = new ImageCopyCaptureFrameV1Interface(m_parent);
    frame->d->init(frameResource);
    m_currentFrame = frame;
}

ImageCopyCaptureSessionV1Interface::ImageCopyCaptureSessionV1Interface(wl_resource *resource,
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

ImageCopyCaptureFrameV1Interface *ImageCopyCaptureSessionV1Interface::getCurrentFrame() const
{
    return d->getCurrentFrame();
}

ImageCopyCaptureFrameV1Interface *ImageCopyCaptureSessionV1InterfacePrivate::getCurrentFrame() const
{
    auto *frame = m_currentFrame.get();
    return (frame != nullptr && frame->d->m_captureRequested && !frame->d->m_captureDone) ? frame : nullptr;
}

//
// Implementation of ImageCopyCaptureFrameV1Interface
//

ImageCopyCaptureFrameV1Interface::ImageCopyCaptureFrameV1Interface(ImageCopyCaptureSessionV1Interface *session)
    : QObject(session)
    , d(std::make_unique<ImageCopyCaptureFrameV1InterfacePrivate>(this, session))
{
}

ImageCopyCaptureFrameV1Interface::~ImageCopyCaptureFrameV1Interface() = default;

ShmClientBuffer *ImageCopyCaptureFrameV1Interface::getShmClientBuffer()
{
    Q_ASSERT(!d->m_captureDone);
    return d->m_shmClientBuffer.get();
}

void ImageCopyCaptureFrameV1Interface::sendTransform(uint32_t transform)
{
    Q_ASSERT(!d->m_captureDone);
    d->send_transform(transform);
}

void ImageCopyCaptureFrameV1Interface::sendDamage(const QRect &rect)
{
    Q_ASSERT(!d->m_captureDone);
    d->send_damage(rect.x(), rect.y(), rect.width(), rect.height());
}

void ImageCopyCaptureFrameV1Interface::sendPresentationTime(std::chrono::nanoseconds timestamp)
{
    Q_ASSERT(!d->m_captureDone);
    uint64_t tv_sec = std::chrono::duration_cast<std::chrono::seconds>(timestamp).count();
    uint32_t tv_sec_hi = tv_sec >> 32;
    uint32_t tv_sec_lo = tv_sec & 0xFFFFFFFF;
    uint32_t tv_nsec = (timestamp % std::chrono::seconds(1)).count();
    d->send_presentation_time(tv_sec_hi, tv_sec_lo, tv_nsec);
}

void ImageCopyCaptureFrameV1Interface::sendReady()
{
    Q_ASSERT(!d->m_captureDone);
    d->m_captureDone = true;
    d->send_ready();
}

void ImageCopyCaptureFrameV1Interface::sendFailed(FailureReason reason)
{
    Q_ASSERT(!d->m_captureDone);
    d->m_captureDone = true;
    d->send_failed(static_cast<uint>(reason));
}

} // namespace KWin

#include "moc_image_copy_capture_v1.cpp"
