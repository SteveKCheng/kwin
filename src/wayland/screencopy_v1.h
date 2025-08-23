/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "core/output.h"
#include "kwin_export.h"

#include <QObject>

struct wl_resource;

namespace KWin
{

class Display;
class OutputInterface;
class ScreencopyManagerV1InterfacePrivate;
class ScreencopyFrameV1InterfacePrivate;
class ScreencopyFrameV1Interface;
class ShmClientBuffer;

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
    ~ScreencopyManagerV1Interface() override;

protected:
    explicit ScreencopyManagerV1Interface(Display *display, QObject *parent);

    /**
     * @brief Instantiate a per-client state object.
     *
     * This factory method is called once per Wayland client when it first binds to the
     * screencopy manager. The returned QObject will be automatically deleted when the
     * client disconnects.
     *
     * @return Newly instantiated per-client state object.
     */
    virtual QObject *createClientState() = 0;

    /**
     * @brief Prepare to capture a frame's contents.
     *
     * This method should inform the client of the required buffer formats
     * (through ScreencopyFrameV1Interface::sendBuffer and ScreencopyFrameV1Interface::sendBufferDone),
     * and set up its own internal tracking of the display output.
     *
     * @param frame The frame that the Wayland client is asking to capture.
     * @param clientState The per-client state object created via createClientState(),
     *                    associated to the Wayland client requesting the frame capture.
     */
    virtual void prepareFrame(ScreencopyFrameV1Interface* frame,
                              QObject *clientState) = 0;

    /**
     * @brief Request for copying one frame from the client.
     *
     * @param frame The frame object created by the client that is the target of the framebuffer copy.
     * @param waitForDamage If false, the Wayland client called \c copy and expects the framebuffer
     *                      to be filled immediately.  If true, the Wayland called \c copy_with_damage
     *                      and expects the framebuffer to be filled only when damage is seen on
     *                      the output versus the preceding frame that was copied.
     * @param clientState The per-client state object created via createClientState(),
     *                    associated to the Wayland client requesting the copy.
     */
    virtual void copyFrame(ScreencopyFrameV1Interface* frame,
                           bool waitForDamage,
                           QObject* clientState) = 0;

private:
    friend class ScreencopyManagerV1InterfacePrivate;
    friend class ScreencopyFrameV1InterfacePrivate; // for calling copyFrame
    const std::unique_ptr<ScreencopyManagerV1InterfacePrivate> d;
};

/**
 * The ScreencopyFrameV1Interface represents a single frame capture request.
 * 
 * The ScreencopyFrameV1Interface corresponds to the Wayland interface @c zwlr_screencopy_frame_v1.
 */
class KWIN_EXPORT ScreencopyFrameV1Interface final : public QObject
{
    Q_OBJECT

public:
    ~ScreencopyFrameV1Interface() override;

    /**
     * @brief The target display output to capture.
     */
    Output * getOutput() const;

    /**
     *
     * @brief Get the rectangular area of the output that the client requested to capture.
     */
    const QRect & getCapturedArea() const;

    /**
     * @brief Whether the client requested the (mouse) cursor be overlaid (rendered) onto the frame.
     */
    bool shouldOverlayCursor() const;

    /**
     * @brief Take the shared-memory buffer from the client to copy the frame's contents
     *        into.
     *
     * A pointer to the buffer passed in from the Wayland client, calling \c copy or \c copy_with_damage,
     * is captured by this object.  A buffer should only be used once per frame, so the pointer
     * to that buffer is erased from this object once this method is called.
     *
     * The client buffer is guaranteed to be alive until control returns to the Wayland dispatch loop.
     *
     * This method returns null if there is no registered buffer or the client destroyed it.
     */
    ShmClientBuffer* takeShmClientBuffer();

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
    void sendDamage(const QRect &rect);

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

private:
    explicit ScreencopyFrameV1Interface(ScreencopyManagerV1Interface *manager,
                                        QObject* clientState);

    friend class ScreencopyFrameV1InterfacePrivate;
    friend class ScreencopyManagerV1InterfacePrivate; // for construction
    const std::unique_ptr<ScreencopyFrameV1InterfacePrivate> d;
};

} // namespace KWin
