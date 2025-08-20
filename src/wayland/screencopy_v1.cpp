/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "screencopy_v1.h"
#include "display.h"
#include "output.h"
#include "core/output.h"

#include <QPointer>
#include <QImage>
#include <chrono>
#include <drm_fourcc.h>

#include "qwayland-server-wlr-screencopy-unstable-v1.h"

namespace KWin
{

static const int s_version = 3;

class ScreencopyManagerV1InterfacePrivate : public QtWaylandServer::zwlr_screencopy_manager_v1
{
public:
    ScreencopyManagerV1InterfacePrivate(ScreencopyManagerV1Interface *q, Display *display);

    ScreencopyManagerV1Interface* const m_parent;

protected:
    void zwlr_screencopy_manager_v1_capture_output(Resource *resource,
                                                   uint32_t frame,
                                                   int32_t overlay_cursor,
                                                   struct ::wl_resource *output) override;
    void zwlr_screencopy_manager_v1_capture_output_region(Resource *resource,
                                                          uint32_t frame,
                                                          int32_t overlay_cursor,
                                                          struct ::wl_resource *output,
                                                          int32_t x,
                                                          int32_t y,
                                                          int32_t width,
                                                          int32_t height) override;
    void zwlr_screencopy_manager_v1_destroy(Resource *resource) override;
};

class ScreencopyFrameV1InterfacePrivate : public QtWaylandServer::zwlr_screencopy_frame_v1
{
public:
    ScreencopyFrameV1InterfacePrivate(ScreencopyFrameV1Interface *q);

    ScreencopyFrameV1Interface *q;
    QPointer<OutputInterface> output;
    QRect region;
    bool includeCursor = false;

protected:
    void zwlr_screencopy_frame_v1_destroy_resource(Resource *resource) override;
    void zwlr_screencopy_frame_v1_copy(Resource *resource, struct ::wl_resource *buffer) override;
    void zwlr_screencopy_frame_v1_copy_with_damage(Resource *resource, struct ::wl_resource *buffer) override;
    void zwlr_screencopy_frame_v1_destroy(Resource *resource) override;
};

ScreencopyManagerV1InterfacePrivate::ScreencopyManagerV1InterfacePrivate(ScreencopyManagerV1Interface *q, Display *display)
    : QtWaylandServer::zwlr_screencopy_manager_v1(*display, s_version)
    , m_parent(q)
{
}

void ScreencopyManagerV1InterfacePrivate::zwlr_screencopy_manager_v1_capture_output(Resource *resource,
                                                                                   uint32_t frame,
                                                                                   int32_t overlay_cursor,
                                                                                   wl_resource *output_resource)
{
    // Get the OutputInterface from the wl_resource
    OutputInterface *outputInterface = OutputInterface::get(output_resource);
    if (!outputInterface || !outputInterface->handle()) {
        wl_resource_post_error(resource->handle, WL_DISPLAY_ERROR_INVALID_OBJECT, "invalid output");
        return;
    }

    wl_resource *frameResource = wl_resource_create(resource->client(), &zwlr_screencopy_frame_v1_interface, resource->version(), frame);
    if (!frameResource) {
        wl_resource_post_no_memory(resource->handle);
        return;
    }

    auto frameInterface = m_parent->createFrame(overlay_cursor != 0,
                                                QRect(),
                                                frameResource,
                                                outputInterface);

    // Send buffer format information
    Output *output = outputInterface->handle();
    QSize outputSize = output->pixelSize();
    uint32_t format = DRM_FORMAT_ARGB8888; // Standard ARGB format
    uint32_t stride = outputSize.width() * 4; // 4 bytes per pixel for ARGB

    frameInterface->sendBuffer(format, outputSize.width(), outputSize.height(), stride);
    frameInterface->sendBufferDone();

    // Emit signal for frame processing
    Q_EMIT m_parent->frameRequested(frameInterface);
}

void ScreencopyManagerV1InterfacePrivate::zwlr_screencopy_manager_v1_capture_output_region(Resource *resource,
                                                                                          uint32_t frame,
                                                                                          int32_t overlay_cursor,
                                                                                          wl_resource *output_resource,
                                                                                          int32_t x,
                                                                                          int32_t y,
                                                                                          int32_t width,
                                                                                          int32_t height)
{
    /*
    // TODO: Implement region capture
    // For now, just create a frame resource and immediately fail it
    wl_resource *frameResource = wl_resource_create(resource->client(), &zwlr_screencopy_frame_v1_interface, resource->version(), frame);
    if (!frameResource) {
        wl_resource_post_no_memory(resource->handle);
        return;
    }

    auto frameInterface = new ScreencopyFrameV1Interface(nullptr, QRect(x, y, width, height), overlay_cursor, m_parent);
    auto framePrivate = new ScreencopyFrameV1InterfacePrivate(frameInterface);
    framePrivate->init(frameResource);

    // Send failed event for now
    framePrivate->send_failed();
    */
}

void ScreencopyManagerV1InterfacePrivate::zwlr_screencopy_manager_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

ScreencopyFrameV1InterfacePrivate::ScreencopyFrameV1InterfacePrivate(ScreencopyFrameV1Interface *q)
    : q(q)
{
}

void ScreencopyFrameV1InterfacePrivate::zwlr_screencopy_frame_v1_destroy_resource(Resource *resource)
{
    Q_EMIT q->destroyed();
    delete q;
}

void ScreencopyFrameV1InterfacePrivate::zwlr_screencopy_frame_v1_copy(Resource *resource, wl_resource *buffer)
{
    Q_EMIT q->copyRequested(buffer, false);
}

void ScreencopyFrameV1InterfacePrivate::zwlr_screencopy_frame_v1_copy_with_damage(Resource *resource, wl_resource *buffer)
{
    Q_EMIT q->copyRequested(buffer, true);
}

void ScreencopyFrameV1InterfacePrivate::zwlr_screencopy_frame_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

ScreencopyManagerV1Interface::ScreencopyManagerV1Interface(Display *display, QObject *parent)
    : QObject(parent)
    , d(new ScreencopyManagerV1InterfacePrivate(this, display))
{
}

ScreencopyManagerV1Interface::~ScreencopyManagerV1Interface()
{
}

ScreencopyFrameV1Interface*
ScreencopyManagerV1Interface::createFrame(bool overlayCursor,
                                          const QRect &frameBox,
                                          wl_resource* frameResource,
                                          OutputInterface* output)
{
    return new ScreencopyFrameV1Interface(frameResource,
                                          output,
                                          QRect(),
                                          overlayCursor,
                                          this);
}

ScreencopyFrameV1Interface::ScreencopyFrameV1Interface(wl_resource* frameResource,
                                                       OutputInterface* output,
                                                       const QRect &region,
                                                       bool includeCursor,
                                                       ScreencopyManagerV1Interface *parent)
    : QObject(parent)
{
    auto framePrivate = new ScreencopyFrameV1InterfacePrivate(this);
    this->d = std::unique_ptr<ScreencopyFrameV1InterfacePrivate>(framePrivate);
    framePrivate->output = output;
    framePrivate->region = QRect(); // Empty means full output
    framePrivate->includeCursor = includeCursor;
    framePrivate->init(frameResource);
}

ScreencopyFrameV1Interface::~ScreencopyFrameV1Interface()
{
}

OutputInterface *ScreencopyFrameV1Interface::output() const
{
    return d ? d->output : nullptr;
}

QRect ScreencopyFrameV1Interface::region() const
{
    return d ? d->region : QRect();
}

bool ScreencopyFrameV1Interface::includesCursor() const
{
    return d ? d->includeCursor : false;
}

void ScreencopyFrameV1Interface::sendBuffer(uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
    if (d) {
        d->send_buffer(format, width, height, stride);
    }
}

void ScreencopyFrameV1Interface::sendLinuxDmabuf(uint32_t format, uint32_t width, uint32_t height)
{
    if (d) {
        d->send_linux_dmabuf(format, width, height);
    }
}

void ScreencopyFrameV1Interface::sendBufferDone()
{
    if (d) {
        d->send_buffer_done();
    }
}

void ScreencopyFrameV1Interface::sendDamage(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (d) {
        d->send_damage(x, y, width, height);
    }
}

void ScreencopyFrameV1Interface::sendFlags(uint32_t flags)
{
    if (d) {
        d->send_flags(flags);
    }
}

void ScreencopyFrameV1Interface::sendReady(std::chrono::nanoseconds timestamp)
{
    if (d) {
        uint64_t tv_sec = std::chrono::duration_cast<std::chrono::seconds>(timestamp).count();
        uint32_t tv_sec_hi = tv_sec >> 32;
        uint32_t tv_sec_lo = tv_sec & 0xFFFFFFFF;
        uint32_t tv_nsec = (timestamp % std::chrono::seconds(1)).count();
        d->send_ready(tv_sec_hi, tv_sec_lo, tv_nsec);
    }
}

void ScreencopyFrameV1Interface::sendFailed()
{
    if (d) {
        d->send_failed();
    }
}

} // namespace KWin

#include "moc_screencopy_v1.cpp"
