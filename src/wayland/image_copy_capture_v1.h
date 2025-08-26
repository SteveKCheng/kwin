/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "kwin_export.h"
#include <QObject>

struct wl_resource;

namespace KWin
{

class Display;
class Output;
class ImageCopyCaptureManagerV1InterfacePrivate;
class ImageCopyCaptureSessionV1InterfacePrivate;
class ImageCopyCaptureFrameV1InterfacePrivate;
class ImageCopyCaptureFrameV1Interface;
class ImageCopyCaptureSessionV1Interface;
class ShmClientBuffer;

/**
 * @brief The manager object for the Wayland ext-image-copy-capture protocol.
 *
 * This class corresponds to the Wayland interface @c ext_image_copy_capture_manager_v1;
 * it encapsulates the low-level details of the Wayland protocol.
 *
 * This class is abstract.  It is inherited from the @c imagecopycapture plug-in
 * to implement the manager's operations.
 *
 * The derived class is instantiated as a singleton to
 * make the manager object available to Wayland clients.
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
     *
     * The returned session object will be automatically deleted when the 
     * session is destroyed by the client.
     *
     * After creating the session object, this method should use that object to
     * advertise to the client the required buffer constraints
     * through the methods such as ImageCopyCaptureSessionV1Interface::sendBufferSize
     * and ImageCopyCaptureSessionV1Interface::sendShmFormat.
     *
     * This method is invoked in reaction to the client issuing the @c create_session
     * request.
     *
     * @param resource The Wayland resource behind the implementation.
     *                 This argument should be passed to the constructor of
     *                 ImageCopyCaptureSessionV1Interface, and only once.
     * @param output The output the client requested to capture from.
     * @param overlayCursor Whether to overlay cursor onto the captured frames.
     *
     * @return Newly instantiated session object, or null on failure
     */
    virtual ImageCopyCaptureSessionV1Interface *createSession(wl_resource* resource,
                                                              Output *output,
                                                              bool overlayCursor) = 0;

private:
    friend class ImageCopyCaptureManagerV1InterfacePrivate;
    const std::unique_ptr<ImageCopyCaptureManagerV1InterfacePrivate> d;
};

/**
 * @brief The session (state) object for the Wayland ext-image-copy-capture protocol.
 *
 * This class corresponds to the Wayland interface @c ext_image_copy_capture_session_v1;
 * it encapsulates the low-level details of the Wayland protocol.
 *
 * This class is abstract.  It is inherited from the @c imagecopycapture plug-in
 * to implement the session's operations.
*/
class KWIN_EXPORT ImageCopyCaptureSessionV1Interface : public QObject
{
    Q_OBJECT

public:
    ~ImageCopyCaptureSessionV1Interface() override;

protected:
    explicit ImageCopyCaptureSessionV1Interface(wl_resource* resource,
                                                ImageCopyCaptureManagerV1Interface *manager);

    /**
     * @brief Request for capturing one frame from the client.
     *
     * This method is invoked in reaction to the client issuing the @c capture request
     * on a frame.
     *
     * Note that there is no virtual method corresponding to the
     * @c create_frame request; the created frame to capture into
     * is simply made available by #getCurrentFrame.
     */
    virtual void captureFrame() = 0;

    /**
     * @brief Register a rectangle in the client's buffer that must be considered
     *        damaged.
     *
     * This method is invoked in reaction to the client issuing the @c damage_buffer request
     * on a frame.  As the protocol specification says, it enables the compositor
     * to optimize by reducing copying.
     *
     * Since the optimization is optional (and may be done in different ways),
     * the buffer damage is not stored concretely (e.g. in a QRegion instance variable)
     * in this base class or in the frame.
     *
     * This method may be invoked multiple times to register a union of rectangles
     * to be damaged.  These invocations happen before #captureFrame but after
     * the preceding frame is destroyed (by the client).
     *
     * This method corresponds to the client request @c damage_buffer on
     * @c ext_image_copy_capture_frame_v1.  It is in this class and not
     * in ImageCopyCaptureFrameV1Interface so the latter class does not have to
     * be abstract.
     *
     * @param damage An area needs to be re-painted into the buffer of the next
     *               frame that is captured.
     */
    virtual void damageClientBuffer(const QRect & damage) = 0;

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

    /**
     * Get the current frame being captured by this session, if any.
     *
     * Note that the protocol specification disallows more than one frame to exist
     * for a given session at any time.  So, as an optimization, the implementation
     * of this method re-uses the same C++ object for subsequent frames "created"
     * by the client.  However, the C++ interface is designed in the logical manner
     * with the frame appearing as an independent object (pointer).
     *
     * Because the client can destroy a frame at any time,
     * this method may return null in the middle of a frame capture.  In that case
     * the caller should abandon capturing the frame.
     *
     * This method also returns null if the Wayland client has created a frame
     * from this session but has not yet requested it to be captured; or, if
     * the (preceding) capture is already complete (by having called
     * ImageCopyCaptureFrameV1Interface::sendReady
     * or ImageCopyCaptureFrameV1Interface::sendFailed).
     *
     * Taking into account the above points, the caller should not save the returned
     * pointer to the frame and expect it can be accessed later.  The scope of the
     * returned (pointer to the) frame must be considered to last until the next
     * request from the Wayland client begins processing.
     */
    ImageCopyCaptureFrameV1Interface * getCurrentFrame() const;

private:
    friend class ImageCopyCaptureSessionV1InterfacePrivate;
    friend class ImageCopyCaptureManagerV1InterfacePrivate; // for advertiseBufferConstraints
    friend class ImageCopyCaptureFrameV1InterfacePrivate; // for captureFrame

    const std::unique_ptr<ImageCopyCaptureSessionV1InterfacePrivate> d;
};


/**
 * @brief Represents a single request to capture a frame, from ImageCopyCaptureSessionV1Interface.
 *
 * Corresponds to the Wayland interface @c ext_image_copy_capture_frame_v1.
 *
 * This class is concrete: (the derived class of) ImageCopyCaptureSessionV1Interface
 * calls methods on this class to retrieve information on buffers supplied by the client,
 * and to report the results of the frame capture.
 */
class KWIN_EXPORT ImageCopyCaptureFrameV1Interface final
{
public:
    ~ImageCopyCaptureFrameV1Interface();

    /**
     * @brief Get the shared-memory buffer from the client to copy the frame's contents
     *        into.
     *
     * A pointer to the buffer attached by the client is captured by this object.
     *
     * The client buffer is guaranteed to be alive until control returns to the Wayland dispatch loop.
     * This method returns null if there is no registered buffer or the client destroyed it.
     *
     * This method must not be called after capturing has finished.
     */
    ShmClientBuffer* getShmClientBuffer();

    /**
     * Send transform information to indicate how the buffer contents are oriented.
     *
     * This method must not be called after capturing has finished.
     */
    void sendTransform(uint32_t transform);

    /**
     * Send damage information about the region that changed.
     *
     * This method must not be called after capturing has finished.
     */
    void sendDamage(const QRect &rect);

    /**
     * Send presentation time of the captured frame.
     *
     * This method must not be called after capturing has finished.
     */
    void sendPresentationTime(std::chrono::nanoseconds timestamp);

    /**
     * Send ready event to indicate successful capture.
     *
     * This method is considered to finish the capturing.
     */
    void sendReady();

    /**
     * @brief The reason for failing to capture a frame, to report to the client.
     */
    enum class FailureReason : uint32_t
    {
        unknown = 0,
        buffer_constraints = 1,
        stopped = 2,
    };

    /**
     * Send failed event to indicate capture failure.
     *
     * This method is considered to finish the capturing (unsuccessfully).
     */
    void sendFailed(FailureReason reason);

private:
    explicit ImageCopyCaptureFrameV1Interface(ImageCopyCaptureSessionV1Interface* session);

    friend class ImageCopyCaptureSessionV1InterfacePrivate; // for construction
    std::unique_ptr<ImageCopyCaptureFrameV1InterfacePrivate> const d;
};

} // namespace KWin
