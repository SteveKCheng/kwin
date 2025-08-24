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
class ImageCopyCaptureManagerV1InterfacePrivate;
class ImageCopyCaptureSessionV1InterfacePrivate;
class ImageCopyCaptureFrameV1InterfacePrivate;
class ImageCopyCaptureFrameV1Interface;
class ImageCopyCaptureSessionV1Interface;
class ShmClientBuffer;

/**
 * The ImageCopyCaptureManagerV1Interface provides ext_image_copy_capture protocol support.
 * 
 * This allows clients to capture screen content directly to client-provided buffers,
 * which is much more efficient for VNC and similar applications than the PipeWire-based
 * screencast protocol.
 * 
 * The ImageCopyCaptureManagerV1Interface corresponds to the Wayland interface @c ext_image_copy_capture_manager_v1.
 */
class KWIN_EXPORT ImageCopyCaptureManagerV1Interface : public QObject
{
    Q_OBJECT

public:
    ~ImageCopyCaptureManagerV1Interface() override;

protected:
    explicit ImageCopyCaptureManagerV1Interface(Display *display, QObject *parent);

    /**
     * @brief Instantiate a capture session for the given output with specified options.
     *
     * This factory method is called when a client requests to create a session.
     * The returned session object will be automatically deleted when the 
     * session is destroyed by the client.
     *
     * @param output The output to capture from
     * @param overlayCursor Whether to overlay cursor onto captured frames
     * @return Newly instantiated session object or nullptr on failure
     */
    virtual ImageCopyCaptureSessionV1Interface *createSession(Output *output, bool overlayCursor) = 0;

private:
    friend class ImageCopyCaptureManagerV1InterfacePrivate;
    const std::unique_ptr<ImageCopyCaptureManagerV1InterfacePrivate> d;
};

/**
 * @brief Per-client session (state) object for ImageCopyCaptureManagerV1Interface.
 */
class KWIN_EXPORT ImageCopyCaptureSessionV1Interface : public QObject
{
    Q_OBJECT

public:
    ~ImageCopyCaptureSessionV1Interface() override;

protected:
    explicit ImageCopyCaptureSessionV1Interface(ImageCopyCaptureManagerV1Interface *manager);

    /**
     * @brief Tell the client what buffer formats this capture session are compatible with.
     *
     * This method should inform the client of the required buffer constraints
     * through the methods #sendBufferSize, #sendShmFormat, etc.
     *
     * This method is called once on creating the session (on what is returned by
     * ImageCopyCaptureManagerV1Interface::createSession), but the session may autonomously
     * re-advertise the buffer constraints if they change, in accordance with the
     * protocol specification.
     */
    virtual void advertiseBufferConstraints() = 0;

    /**
     * @brief Request for capturing one frame from the client.
     *
     * @param frame The frame object created by the client that is the target of the capture.
     */
    virtual void captureFrame(ImageCopyCaptureFrameV1Interface* frame) = 0;

    /**
     * Send buffer size constraint to the client.
     */
    void sendBufferSize(const QSize &size);

    /**
     * Send shared memory format constraint to the client.
     */
    void sendShmFormat(uint32_t format);

    /**
     * Send DMA-buf device constraint to the client.
     */
    void sendDmaBufDevice(const QByteArray &device);

    /**
     * Send DMA-buf format constraint to the client.
     */
    void sendDmaBufFormat(uint32_t format, const QByteArray &modifiers);

    /**
     * Send done event to indicate all buffer constraints have been sent.
     */
    void sendConstraintsDone();

    /**
     * Send stopped event to indicate the session is no longer available.
     */
    void sendStopped();

private:
    friend class ImageCopyCaptureSessionV1InterfacePrivate;
    friend class ImageCopyCaptureManagerV1InterfacePrivate; // for advertiseBufferConstraints
    friend class ImageCopyCaptureFrameV1InterfacePrivate; // for captureFrame

    const std::unique_ptr<ImageCopyCaptureSessionV1InterfacePrivate> d;
};


/**
 * The ImageCopyCaptureFrameV1Interface represents a single frame capture request.
 * 
 * The ImageCopyCaptureFrameV1Interface corresponds to the Wayland interface @c ext_image_copy_capture_frame_v1.
 */
class KWIN_EXPORT ImageCopyCaptureFrameV1Interface final : public QObject
{
    Q_OBJECT

public:
    ~ImageCopyCaptureFrameV1Interface() override;

    /**
     * @brief Take the shared-memory buffer from the client to copy the frame's contents
     *        into.
     *
     * A pointer to the buffer attached by the client is captured by this object.
     * A buffer should only be used once per frame, so the pointer
     * to that buffer is erased from this object once this method is called.
     *
     * The client buffer is guaranteed to be alive until control returns to the Wayland dispatch loop.
     *
     * This method returns null if there is no registered buffer or the client destroyed it.
     */
    ShmClientBuffer* takeShmClientBuffer();

    /**
     * Send transform information to indicate how the buffer contents are oriented.
     */
    void sendTransform(uint32_t transform);

    /**
     * Send damage information about the region that changed.
     */
    void sendDamage(const QRect &rect);

    /**
     * Send presentation time of the captured frame.
     */
    void sendPresentationTime(std::chrono::nanoseconds timestamp);

    /**
     * Send ready event to indicate successful capture.
     */
    void sendReady();

    enum class FailureReason : uint32_t
    {
        unknown = 0,
        buffer_constraints = 1,
        stopped = 2,
    };

    /**
     * Send failed event to indicate capture failure.
     */
    void sendFailed(FailureReason reason);

private:
    explicit ImageCopyCaptureFrameV1Interface(ImageCopyCaptureSessionV1Interface *session);

    friend class ImageCopyCaptureSessionV1InterfacePrivate; // for construction
    const std::unique_ptr<ImageCopyCaptureFrameV1InterfacePrivate> d;
};

} // namespace KWin
