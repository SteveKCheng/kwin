/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "screencopy_v1.h"
#include "display.h"
#include "output.h"
#include "wayland/output.h"
#include "wayland/shmclientbuffer_p.h"

#include <QPointer>

#include <chrono>

#include "qwayland-server-wlr-screencopy-unstable-v1.h"

namespace KWin
{

static const int s_version = 3;

class ScreencopyManagerV1InterfacePrivate final : public QtWaylandServer::zwlr_screencopy_manager_v1
{
public:
    ScreencopyManagerV1InterfacePrivate(ScreencopyManagerV1Interface *q, Display *display)
        : QtWaylandServer::zwlr_screencopy_manager_v1(*display, s_version)
        , m_parent(q)
    {
    }

private:
    ScreencopyManagerV1Interface* const m_parent;
    
    // Map from wl_client to per-client state objects
    QHash<wl_client *, QObject *> m_clientStates;

    void createClientState(wl_client *client)
    {
        Q_ASSERT(!m_clientStates.contains(client)); // Should not already exist
        
        // Create new client state via factory
        QObject *clientState = m_parent->createClientState();
        if (clientState != nullptr) {
            m_clientStates[client] = clientState;
        }
    }

    QObject *getClientState(wl_client *client)
    {
        QObject *clientState = m_clientStates.value(client);
        Q_ASSERT(clientState != nullptr); // Should have been created in bind_resource
        return clientState;
    }


protected:
    void zwlr_screencopy_manager_v1_bind_resource(Resource *resource) override
    {
        // Create per-client state when client first binds to manager
        wl_client *client = resource->client();
        if (!m_clientStates.contains(client)) {
            createClientState(client);
        }
    }

    void zwlr_screencopy_manager_v1_destroy_resource(Resource *resource) override
    {
        // Clean up per-client state if this was the last resource for this client
        wl_client *client = resource->client();
        if (!resourceMap().contains(client)) {
            delete m_clientStates.take(client);
        }
    }

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

    void zwlr_screencopy_manager_v1_destroy(Resource *resource) override
    {
        wl_resource_destroy(resource->handle);
    }
};

class ScreencopyFrameV1InterfacePrivate final : public QtWaylandServer::zwlr_screencopy_frame_v1
{
public:
    ScreencopyFrameV1InterfacePrivate(ScreencopyFrameV1Interface *q, ScreencopyManagerV1Interface* manager)
        : m_parent(q)
        , m_manager(manager)
    {
    }

    QPointer<Output> m_output;
    QRect m_frameBox;
    bool m_overlayCursor = false;

    QPointer<ShmClientBuffer> m_shmClientBuffer;

private:
    /**
     * @brief Pointer to the owner of this private pimpl, required for deletion
     *        and for passing to ScreencopyManagerV1Interface::copyFrame.
     */
    ScreencopyFrameV1Interface * const m_parent;

    /**
     * @brief Pointer to owning manager, for calling ScreencopyManagerV1Interface::copyFrame.
     *
     * This manager is the QObject parent of m_parent so it is guaranteed to be alive if this object is.
     */
    ScreencopyManagerV1Interface * const m_manager;

    /**
     * @brief Common code to handle client's request for \c copy and \c copy_with_damage.
     */
    void copyRequested(struct ::wl_resource *buffer, bool withDamage);

protected:
    void zwlr_screencopy_frame_v1_destroy_resource(Resource *resource) override
    {
        delete m_parent;
    }

    void zwlr_screencopy_frame_v1_destroy(Resource *resource) override
    {
        wl_resource_destroy(resource->handle);
        m_shmClientBuffer = nullptr;
    }

    void zwlr_screencopy_frame_v1_copy(Resource *resource, struct ::wl_resource *buffer) override
    {
        copyRequested(buffer, false);
    }

    void zwlr_screencopy_frame_v1_copy_with_damage(Resource *resource, struct ::wl_resource *buffer) override
    {
        copyRequested(buffer, true);
    }
};

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

    QObject *clientState = getClientState(resource->client());

    m_parent->createFrame(overlay_cursor != 0,
                          QRect(),
                          frameResource,
                          outputInterface,
                          clientState);
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

void ScreencopyFrameV1InterfacePrivate::copyRequested(wl_resource *buffer, bool withDamage)
{
    if (m_shmClientBuffer != nullptr) {
        // zwlr_screencopy_frame_v1::error::already_used
        qWarning() << "screencopy's copy or copy_with_damage method called by the Wayland client while another copy is pending";
        send_failed();
        return;
    }

    auto* shmClientBuffer = ShmClientBuffer::get(buffer);
    if (!shmClientBuffer) {
        send_failed();
        return;
    }

    m_shmClientBuffer = shmClientBuffer;
    m_manager->copyFrame(m_parent, withDamage);
}

//
// Implementation of ScreencopyManagerV1Interface
//

ScreencopyManagerV1Interface::ScreencopyManagerV1Interface(Display *display, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<ScreencopyManagerV1InterfacePrivate>(this, display))
{
}

ScreencopyManagerV1Interface::~ScreencopyManagerV1Interface() = default;

//
// Implementation of ScreencopyFrameV1Interface
//

ScreencopyFrameV1Interface::ScreencopyFrameV1Interface(bool overlayCursor,
                                                       const QRect & frameBox,
                                                       OutputInterface* outputInterface,
                                                       wl_resource* frameResource,
                                                       ScreencopyManagerV1Interface *manager)
    : QObject(manager)
    , d(std::make_unique<ScreencopyFrameV1InterfacePrivate>(this, manager))
{
    d->m_output = outputInterface->handle();
    d->m_frameBox = frameBox;
    d->m_overlayCursor = overlayCursor;
    d->init(frameResource);
}

ScreencopyFrameV1Interface::~ScreencopyFrameV1Interface() = default;

Output* ScreencopyFrameV1Interface::getOutput() const
{
    return d->m_output.get();
}

const QRect & ScreencopyFrameV1Interface::getCapturedArea() const
{
    return d->m_frameBox;
}

bool ScreencopyFrameV1Interface::shouldOverlayCursor() const
{
    return d->m_overlayCursor;
}

ShmClientBuffer* ScreencopyFrameV1Interface::takeShmClientBuffer()
{
    auto* p = d->m_shmClientBuffer.get();
    d->m_shmClientBuffer = nullptr;
    return p;
}

void ScreencopyFrameV1Interface::sendBuffer(uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
    return d->send_buffer(format, width, height, stride);
}

void ScreencopyFrameV1Interface::sendLinuxDmabuf(uint32_t format, uint32_t width, uint32_t height)
{
    d->send_linux_dmabuf(format, width, height);
}

void ScreencopyFrameV1Interface::sendBufferDone()
{
    d->send_buffer_done();
}

void ScreencopyFrameV1Interface::sendFailed()
{
    d->send_failed();
}

void ScreencopyFrameV1Interface::sendFlags(uint32_t flags)
{
    d->send_flags(flags);
}

void ScreencopyFrameV1Interface::sendDamage(const QRect &rect)
{
    d->send_damage(rect.x(), rect.y(), rect.width(), rect.height());
}

void ScreencopyFrameV1Interface::sendReady(std::chrono::nanoseconds timestamp)
{
    uint64_t tv_sec = std::chrono::duration_cast<std::chrono::seconds>(timestamp).count();
    uint32_t tv_sec_hi = tv_sec >> 32;
    uint32_t tv_sec_lo = tv_sec & 0xFFFFFFFF;
    uint32_t tv_nsec = (timestamp % std::chrono::seconds(1)).count();
    d->send_ready(tv_sec_hi, tv_sec_lo, tv_nsec);
}

} // namespace KWin

#include "moc_screencopy_v1.cpp"
