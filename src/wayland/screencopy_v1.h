/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "kwin_export.h"

#include <QObject>
#include <QRegion>
#include <memory>

struct wl_resource;
struct wl_buffer;

namespace KWin
{
class Display;
class OutputInterface;
class ShmClientBuffer;
class ScreencopyManagerV1InterfacePrivate;
class ScreencopyFrameV1InterfacePrivate;
class ScreencopyFrameV1Interface;

/**
 * The ScreencopyManagerV1Interface provides wlroots screencopy protocol support.
 * 
 * This allows clients to capture screen content directly to client-provided buffers,
 * which is much more efficient for VNC and similar applications than the PipeWire-based
 * screencast protocol.
 * 
 * The ScreencopyManagerV1Interface corresponds to the Wayland interface @c zwlr_screencopy_manager_v1.
 */
class KWIN_EXPORT ScreencopyManagerV1Interface : public QObject
{
    Q_OBJECT

public:
    explicit ScreencopyManagerV1Interface(Display *display, QObject *parent = nullptr);
    ~ScreencopyManagerV1Interface() override;

    /**
     * Returns the Wayland display for the screencopy manager.
     */
    Display *display() const;

Q_SIGNALS:
    /**
     * This signal is emitted when a new screencopy frame @a frame has been requested.
     */
    void frameRequested(ScreencopyFrameV1Interface *frame);

private:
    std::unique_ptr<ScreencopyManagerV1InterfacePrivate> d;
};

/**
 * The ScreencopyFrameV1Interface represents a single frame capture request.
 * 
 * The ScreencopyFrameV1Interface corresponds to the Wayland interface @c zwlr_screencopy_frame_v1.
 */
class KWIN_EXPORT ScreencopyFrameV1Interface : public QObject
{
    Q_OBJECT

public:
    enum class BufferType {
        Shm,
        DmaBuf
    };

    ~ScreencopyFrameV1Interface() override;

    /**
     * Returns the output that should be captured.
     */
    OutputInterface *output() const;

    /**
     * Returns the region to capture, or an invalid QRect for full output capture.
     */
    QRect region() const;

    /**
     * Returns true if the cursor should be included in the capture.
     */
    bool includesCursor() const;

    /**
     * Returns true if copy_with_damage was requested instead of copy.
     */
    bool waitForDamage() const;

    /**
     * Send buffer format information to the client.
     * This should be called for each supported buffer type.
     */
    void sendBuffer(uint32_t format, uint32_t width, uint32_t height, uint32_t stride);
    void sendLinuxDmabuf(uint32_t format, uint32_t width, uint32_t height);

    /**
     * Send buffer_done event to indicate all buffer types have been sent.
     */
    void sendBufferDone();

    /**
     * Send damage information when using copy_with_damage.
     */
    void sendDamage(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

    /**
     * Send flags about the captured frame.
     */
    void sendFlags(uint32_t flags);

    /**
     * Send ready event with timestamp to indicate successful capture.
     */
    void sendReady(std::chrono::nanoseconds timestamp);

    /**
     * Send failed event to indicate capture failure.
     */
    void sendFailed();

Q_SIGNALS:
    /**
     * Emitted when the client requests a copy operation.
     */
    void copyRequested(wl_resource *buffer, bool waitForDamage);

    /**
     * Emitted when the frame is destroyed.
     */
    void destroyed();

private:
    friend class ScreencopyManagerV1InterfacePrivate;
    explicit ScreencopyFrameV1Interface(OutputInterface *output, const QRect &region, bool includeCursor, QObject *parent = nullptr);

    std::unique_ptr<ScreencopyFrameV1InterfacePrivate> d;
};

} // namespace KWin
