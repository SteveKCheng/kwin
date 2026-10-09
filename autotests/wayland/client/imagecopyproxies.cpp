/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "imagecopyproxies.h"

#include "KWayland/Client/buffer.h"
#include "KWayland/Client/event_queue.h"
#include "KWayland/Client/output.h"

// Include the generated QtWayland protocol bindings
#include "qwayland-ext-image-capture-source-v1.h"
#include "qwayland-ext-image-copy-capture-v1.h"

// Wayland protocol C bindings
extern "C" {
#include <wayland-client-protocol.h>
}

namespace ImageCopyCaptureClient
{

//
// CaptureManagerPrivate
//

class CaptureManagerPrivate : public QtWayland::ext_image_copy_capture_manager_v1
{
public:
    explicit CaptureManagerPrivate(CaptureManager *q);

    CaptureManager *q;
};

CaptureManagerPrivate::CaptureManagerPrivate(CaptureManager *q)
    : q(q)
{
}

//
// CaptureManager
//

const char *CaptureManager::interfaceName()
{
    return ::ext_image_copy_capture_manager_v1_interface.name;
}

CaptureManager::CaptureManager(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<CaptureManagerPrivate>(this))
{
}

CaptureManager::~CaptureManager()
{
    if (isValid()) {
        destroy();
    }
}

void CaptureManager::setup(wl_registry *registry, uint32_t name, uint32_t version)
{
    d->init(registry, name, version);
}

bool CaptureManager::isValid() const
{
    return d->isInitialized();
}

void CaptureManager::setEventQueue(KWayland::Client::EventQueue *queue)
{
    if (!isValid()) {
        return;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(d->object()), *queue);
}

std::unique_ptr<CaptureSession> CaptureManager::createSession(const CaptureSource &source, bool paintCursors)
{
    if (!isValid()) {
        return nullptr;
    }

    uint32_t options = paintCursors ? QtWayland::ext_image_copy_capture_manager_v1::options_paint_cursors : 0;
    auto *sessionResource = d->create_session(source.resource(), options);

    auto session = std::make_unique<CaptureSession>();
    session->setup(sessionResource);

    return session;
}

void CaptureManager::destroy()
{
    if (isValid()) {
        d->destroy();
    }
}

//
// OutputSourceManagerPrivate
//

class OutputSourceManagerPrivate : public QtWayland::ext_output_image_capture_source_manager_v1
{
public:
    explicit OutputSourceManagerPrivate(OutputSourceManager *q);

    OutputSourceManager *q;
};

OutputSourceManagerPrivate::OutputSourceManagerPrivate(OutputSourceManager *q)
    : q(q)
{
}

//
// OutputSourceManager
//

const char *OutputSourceManager::interfaceName()
{
    return ::ext_output_image_capture_source_manager_v1_interface.name;
}

OutputSourceManager::OutputSourceManager(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<OutputSourceManagerPrivate>(this))
{
}

OutputSourceManager::~OutputSourceManager()
{
    if (isValid()) {
        destroy();
    }
}

void OutputSourceManager::setup(wl_registry *registry, uint32_t name, uint32_t version)
{
    d->init(registry, name, version);
}

bool OutputSourceManager::isValid() const
{
    return d->isInitialized();
}

void OutputSourceManager::setEventQueue(KWayland::Client::EventQueue *queue)
{
    if (!isValid()) {
        return;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(d->object()), *queue);
}

std::unique_ptr<CaptureSource> OutputSourceManager::createSource(KWayland::Client::Output &output)
{
    if (!isValid()) {
        return nullptr;
    }

    auto *sourceResource = d->create_source(output.output());

    auto source = std::make_unique<CaptureSource>(this);
    source->setup(sourceResource);

    return source;
}

void OutputSourceManager::destroy()
{
    if (isValid()) {
        d->destroy();
    }
}

//
// CaptureSourcePrivate
//

class CaptureSourcePrivate : public QtWayland::ext_image_capture_source_v1
{
public:
    explicit CaptureSourcePrivate(CaptureSource *q);

    CaptureSource *q;
};

CaptureSourcePrivate::CaptureSourcePrivate(CaptureSource *q)
    : q(q)
{
}

//
// CaptureSource
//

CaptureSource::CaptureSource(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<CaptureSourcePrivate>(this))
{
}

CaptureSource::~CaptureSource()
{
    if (isValid()) {
        destroy();
    }
}

void CaptureSource::setup(struct ::ext_image_capture_source_v1 *resource)
{
    d->init(resource);
}

bool CaptureSource::isValid() const
{
    return d->isInitialized();
}

ext_image_capture_source_v1 *CaptureSource::resource() const
{
    return d->object();
}

void CaptureSource::destroy()
{
    if (isValid()) {
        d->destroy();
    }
}

//
// CaptureSessionPrivate
//

class CaptureSessionPrivate : public QtWayland::ext_image_copy_capture_session_v1
{
public:
    explicit CaptureSessionPrivate(CaptureSession *q);

    // Accumulated constraint data
    QSize m_bufferSize;
    QVector<uint32_t> m_shmFormats;
    dev_t m_dmaBufDevice = static_cast<dev_t>(-1);
    QVector<CaptureSession::DmaBufFormat> m_dmaBufFormats;
    bool m_constraintsDone = false;
    bool m_stopped = false;

    CaptureSession *q;

protected:
    // Protocol event handlers
    void ext_image_copy_capture_session_v1_buffer_size(uint32_t width, uint32_t height) override;
    void ext_image_copy_capture_session_v1_shm_format(uint32_t format) override;
    void ext_image_copy_capture_session_v1_dmabuf_device(wl_array *device) override;
    void ext_image_copy_capture_session_v1_dmabuf_format(uint32_t format, wl_array *modifiers) override;
    void ext_image_copy_capture_session_v1_done() override;
    void ext_image_copy_capture_session_v1_stopped() override;
};

CaptureSessionPrivate::CaptureSessionPrivate(CaptureSession *q)
    : q(q)
{
}

void CaptureSessionPrivate::ext_image_copy_capture_session_v1_buffer_size(uint32_t width, uint32_t height)
{
    m_bufferSize = QSize(static_cast<int>(width), static_cast<int>(height));
}

void CaptureSessionPrivate::ext_image_copy_capture_session_v1_shm_format(uint32_t format)
{
    m_shmFormats.append(format);
}

void CaptureSessionPrivate::ext_image_copy_capture_session_v1_dmabuf_device(wl_array *device)
{
    if (device->size != sizeof(dev_t)) {
        qWarning("'device' argument to dmabuf_device event has the wrong size");
        return;
    }

    std::memcpy(&m_dmaBufDevice, device->data, sizeof(dev_t));
}

void CaptureSessionPrivate::ext_image_copy_capture_session_v1_dmabuf_format(uint32_t format, wl_array *modifiers)
{
    if ((modifiers->size % sizeof(uint64_t)) != 0) {
        qWarning("'modifiers' array does not contain a whole number of 64-bit entries");
        return;
    }

    auto numModifiers = modifiers->size / sizeof(uint64_t);
    for (size_t i = 0; i < numModifiers; ++i) {
        // Element might not be aligned
        uint64_t modifier;
        std::memcpy(&modifier, static_cast<uint64_t *>(modifiers->data) + i, sizeof(uint64_t));

        m_dmaBufFormats.append({format, modifier});
    }
}

void CaptureSessionPrivate::ext_image_copy_capture_session_v1_done()
{
    m_constraintsDone = true;
    Q_EMIT q->constraintsReady();
}

void CaptureSessionPrivate::ext_image_copy_capture_session_v1_stopped()
{
    m_stopped = true;
    Q_EMIT q->stopped();
}

//
// CaptureSession
//

CaptureSession::CaptureSession(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<CaptureSessionPrivate>(this))
{
}

CaptureSession::~CaptureSession()
{
    if (isValid()) {
        destroy();
    }
}

void CaptureSession::setup(struct ::ext_image_copy_capture_session_v1 *resource)
{
    d->init(resource);
}

bool CaptureSession::isValid() const
{
    return d->isInitialized();
}

void CaptureSession::setEventQueue(KWayland::Client::EventQueue *queue)
{
    if (!isValid()) {
        return;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(d->object()), *queue);
}

QSize CaptureSession::bufferSize() const
{
    return d->m_bufferSize;
}

QVector<uint32_t> CaptureSession::shmFormats() const
{
    return d->m_shmFormats;
}

dev_t CaptureSession::dmaBufDevice() const
{
    return d->m_dmaBufDevice;
}

QVector<CaptureSession::DmaBufFormat> CaptureSession::dmaBufFormats() const
{
    return d->m_dmaBufFormats;
}

bool CaptureSession::constraintsDone() const
{
    return d->m_constraintsDone;
}

bool CaptureSession::isStopped() const
{
    return d->m_stopped;
}

std::unique_ptr<CaptureFrame> CaptureSession::createFrame()
{
    if (!isValid()) {
        return nullptr;
    }

    auto *frameResource = d->create_frame();

    auto frame = std::make_unique<CaptureFrame>();
    frame->setup(frameResource);

    return frame;
}

void CaptureSession::destroy()
{
    if (isValid()) {
        d->destroy();
    }
}

//
// CaptureFramePrivate
//

class CaptureFramePrivate : public QtWayland::ext_image_copy_capture_frame_v1
{
public:
    explicit CaptureFramePrivate(CaptureFrame *q);

    // Accumulated frame metadata
    uint32_t m_transform = 0;
    QVector<QRect> m_damageRegions;
    std::chrono::nanoseconds m_presentationTime{0};
    bool m_ready = false;
    bool m_failed = false;
    uint32_t m_failureReason = 0;

    CaptureFrame *q;

protected:
    // Protocol event handlers
    void ext_image_copy_capture_frame_v1_transform(uint32_t transform) override;
    void ext_image_copy_capture_frame_v1_damage(int32_t x, int32_t y, int32_t width, int32_t height) override;
    void ext_image_copy_capture_frame_v1_presentation_time(uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) override;
    void ext_image_copy_capture_frame_v1_ready() override;
    void ext_image_copy_capture_frame_v1_failed(uint32_t reason) override;
};

CaptureFramePrivate::CaptureFramePrivate(CaptureFrame *q)
    : q(q)
{
}

void CaptureFramePrivate::ext_image_copy_capture_frame_v1_transform(uint32_t transform)
{
    m_transform = transform;
}

void CaptureFramePrivate::ext_image_copy_capture_frame_v1_damage(int32_t x, int32_t y, int32_t width, int32_t height)
{
    m_damageRegions.append(QRect(x, y, width, height));
}

void CaptureFramePrivate::ext_image_copy_capture_frame_v1_presentation_time(uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    // Reconstruct the 64-bit timestamp
    uint64_t tv_sec = (static_cast<uint64_t>(tv_sec_hi) << 32) | tv_sec_lo;
    auto seconds = std::chrono::seconds(tv_sec);
    auto nanoseconds = std::chrono::nanoseconds(tv_nsec);
    m_presentationTime = seconds + nanoseconds;
}

void CaptureFramePrivate::ext_image_copy_capture_frame_v1_ready()
{
    m_ready = true;
    Q_EMIT q->ready();
}

void CaptureFramePrivate::ext_image_copy_capture_frame_v1_failed(uint32_t reason)
{
    m_failed = true;
    m_failureReason = reason;
    Q_EMIT q->failed(reason);
}

//
// CaptureFrame
//

CaptureFrame::CaptureFrame(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<CaptureFramePrivate>(this))
{
}

CaptureFrame::~CaptureFrame()
{
    if (isValid()) {
        destroy();
    }
}

void CaptureFrame::setup(struct ::ext_image_copy_capture_frame_v1 *resource)
{
    d->init(resource);
}

bool CaptureFrame::isValid() const
{
    return d->isInitialized();
}

void CaptureFrame::setEventQueue(KWayland::Client::EventQueue *queue)
{
    if (!isValid()) {
        return;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(d->object()), *queue);
}

void CaptureFrame::attachBuffer(wl_buffer *buffer)
{
    if (!isValid()) {
        return;
    }
    d->attach_buffer(buffer);
}

void CaptureFrame::attachBuffer(KWayland::Client::Buffer &shmBuffer)
{
    d->attach_buffer(shmBuffer.buffer());
}

void CaptureFrame::damageBuffer(const QRect &damage)
{
    if (!isValid()) {
        return;
    }
    d->damage_buffer(damage.x(), damage.y(), damage.width(), damage.height());
}

void CaptureFrame::capture()
{
    if (!isValid()) {
        return;
    }
    d->capture();
}

uint32_t CaptureFrame::transform() const
{
    return d->m_transform;
}

QVector<QRect> CaptureFrame::damageRegions() const
{
    return d->m_damageRegions;
}

std::chrono::nanoseconds CaptureFrame::presentationTime() const
{
    return d->m_presentationTime;
}

bool CaptureFrame::isReady() const
{
    return d->m_ready;
}

bool CaptureFrame::hasFailed() const
{
    return d->m_failed;
}

uint32_t CaptureFrame::failureReason() const
{
    return d->m_failureReason;
}

void CaptureFrame::destroy()
{
    if (isValid()) {
        d->destroy();
    }
}

} // namespace ImageCopyCapture

#include "imagecopyproxies.moc"
