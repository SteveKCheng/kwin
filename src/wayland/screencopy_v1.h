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
     * @brief Instantiate a concrete implementation of ScreencopyFrameV1Interface.
     *
     * This factory method is called in response to the method
     * @c capture_output and @c capture_output_region of the @c zwlr_screencopy_manager_v1
     * interface.
     *
     * @param overlayCursor Whether to render the mouse cursor as part of the captured frame.
     * @param frameBox The rectangular subset of the output that the client requested to
     *                 capture.
     * @param frameResource The newly instantiated Wayland resource for
     *                      the @c zwlr_screencopy_frame_v1 interface.  This argument
     *                      should be passed directly to the constructor of
     *                      ScreencopyFrameV1Interface.
     * @param output The Wayland output that the client requested to capture.
     * @param clientState The per-client state object created via createClientState().
     *
     * @return Newly instantiated implementation of ScreencopyFrameV1Interface.
     */
    virtual ScreencopyFrameV1Interface* createFrame(bool overlayCursor,
                                                    const QRect &frameBox,
                                                    wl_resource* frameResource,
                                                    OutputInterface* output,
                                                    QObject *clientState) = 0;

    /**
     * @brief Request for copying one frame from the client.
     *
     * @param frame The frame object created by the client that is the target of the framebuffer copy.
     * @param waitForDamage If false, the Wayland client called \c copy and expects the framebuffer
     *                      to be filled immediately.  If true, the Wayland called \c copy_with_damage
     *                      and expects the framebuffer to be filled only when damage is seen on
     *                      the output versus the preceding frame that was copied.
     */
    virtual void copyFrame(ScreencopyFrameV1Interface* frame, bool waitForDamage) = 0;

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
class KWIN_EXPORT ScreencopyFrameV1Interface : public QObject
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

    explicit ScreencopyFrameV1Interface(bool overlayCursor,
                                        const QRect & frameBox,
                                        OutputInterface* outputInterface,
                                        wl_resource* frameResource,
                                        ScreencopyManagerV1Interface *manager);

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
    friend class ScreencopyFrameV1InterfacePrivate;
    const std::unique_ptr<ScreencopyFrameV1InterfacePrivate> d;
};

} // namespace KWin
