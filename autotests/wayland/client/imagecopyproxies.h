/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QObject>
#include <QRect>
#include <QSize>
#include <QVector>
#include <chrono>
#include <memory>

#include <sys/types.h> // for dev_t

struct wl_registry;
struct wl_buffer;
struct ext_image_capture_source_v1;
struct ext_image_copy_capture_session_v1;
struct ext_image_copy_capture_frame_v1;

namespace KWayland::Client
{

class EventQueue;
class Output;
class Buffer;

}

namespace ImageCopyCaptureClient
{

class CaptureManagerPrivate;
class OutputSourceManagerPrivate;
class CaptureSourcePrivate;
class CaptureSessionPrivate;
class CaptureFramePrivate;
class CaptureSource;
class CaptureSession;
class CaptureFrame;

/**
 * @brief Client-side proxy for ext_image_copy_capture_manager_v1
 *
 * This class wraps the QtWayland generated proxy and provides
 * a more convenient interface for testing.
 */
class CaptureManager : public QObject
{
    Q_OBJECT

public:
    explicit CaptureManager(QObject *parent = nullptr);
    ~CaptureManager() override;

    /**
     * @brief Initialize the manager from registry
     */
    void setup(wl_registry *registry, uint32_t name, uint32_t version);

    /**
     * @brief Check if the manager is valid and ready to use
     */
    bool isValid() const;

    /**
     * @brief Set the event queue for this manager
     */
    void setEventQueue(KWayland::Client::EventQueue *queue);

    /**
     * @brief Create a capture session for a capture source object
     * @param source The capture source object to capture from
     * @param paintCursors Whether to paint cursors onto captured frames
     * @return New capture session object (ownership transferred to caller)
     */
    std::unique_ptr<CaptureSession> createSession(const CaptureSource &source, bool paintCursors = false);

    /**
     * @brief Create a capture session passing the raw options bitfield (for testing invalid values)
     */
    std::unique_ptr<CaptureSession> createSession(const CaptureSource &source, uint32_t options);

    /**
     * @brief Destroy the manager (calls protocol destroy)
     */
    void destroy();

    /// Name of this interface in the Wayland protocol.
    static const char *interfaceName();

private:
    std::unique_ptr<CaptureManagerPrivate> d;
};

/**
 * @brief Client-side proxy for ext_output_image_capture_source_manager_v1
 *
 * This class manages creation of image capture sources for outputs.
 */
class OutputSourceManager : public QObject
{
    Q_OBJECT

public:
    explicit OutputSourceManager(QObject *parent = nullptr);
    ~OutputSourceManager() override;

    /**
     * @brief Initialize the manager from registry
     */
    void setup(wl_registry *registry, uint32_t name, uint32_t version);

    /**
     * @brief Check if the manager is valid and ready to use
     */
    bool isValid() const;

    /**
     * @brief Set the event queue for this manager
     */
    void setEventQueue(KWayland::Client::EventQueue *queue);

    /**
     * @brief Create an image capture source for an output
     * @param output The desired Wayland output for capturing
     * @return New capture source object (ownership transferred to caller)
     */
    std::unique_ptr<CaptureSource> createSource(KWayland::Client::Output &output);

    /**
     * @brief Destroy the manager (calls protocol destroy)
     */
    void destroy();

    /// Name of this interface in the Wayland protocol.
    static const char *interfaceName();

private:
    std::unique_ptr<OutputSourceManagerPrivate> d;
};

/**
 * @brief Client-side proxy for ext_image_capture_source_v1
 *
 * This class is a simple wrapper that provides automatic resource cleanup.
 * It has no events or requests beyond the destroy request.
 */
class CaptureSource : public QObject
{
    Q_OBJECT

public:
    explicit CaptureSource(QObject *parent = nullptr);
    ~CaptureSource() override;

    /**
     * @brief Initialize the source with a Wayland resource
     * @param resource The source resource from create_source
     */
    void setup(struct ::ext_image_capture_source_v1 *resource);

    /**
     * @brief Check if the source is valid
     */
    bool isValid() const;

    /**
     * @brief Get the underlying Wayland resource
     * @return The ext_image_capture_source_v1 resource
     */
    ext_image_capture_source_v1 *resource() const;

    /**
     * @brief Destroy the source (calls protocol destroy)
     */
    void destroy();

private:
    std::unique_ptr<CaptureSourcePrivate> d;
};

/**
 * @brief Client-side proxy for ext_image_copy_capture_session_v1
 *
 * This class represents a capture session and accumulates all
 * buffer constraint information for easy testing.
 */
class CaptureSession : public QObject
{
    Q_OBJECT

public:
    explicit CaptureSession(QObject *parent = nullptr);
    ~CaptureSession() override;

    /**
     * @brief Initialize the session with a Wayland resource
     * @param resource The session resource from create_session
     */
    void setup(struct ::ext_image_copy_capture_session_v1 *resource);

    /**
     * @brief Check if the session is valid
     */
    bool isValid() const;

    /**
     * @brief Set the event queue for this session
     */
    void setEventQueue(KWayland::Client::EventQueue *queue);

    /**
     * @brief Get the buffer size constraint
     */
    QSize bufferSize() const;

    /**
     * @brief Get all supported SHM formats
     */
    QVector<uint32_t> shmFormats() const;

    /**
     * @brief Get the DMA-buf device constraint
     */
    dev_t dmaBufDevice() const;

    /**
     * @brief Get all supported DMA-buf formats with their modifiers
     */
    struct DmaBufFormat
    {
        uint32_t format;
        uint64_t modifier;
    };
    QVector<DmaBufFormat> dmaBufFormats() const;

    /**
     * @brief Check if constraint information is complete (done event received)
     */
    bool constraintsDone() const;

    /**
     * @brief Check if the session has been stopped
     */
    bool isStopped() const;

    /**
     * @brief Create a frame for this session
     * @return New capture frame object (ownership transferred to caller)
     */
    std::unique_ptr<CaptureFrame> createFrame();

    /**
     * @brief Destroy the session (calls protocol destroy)
     */
    void destroy();

Q_SIGNALS:
    /**
     * @brief Emitted when buffer constraints are complete
     */
    void constraintsReady();

    /**
     * @brief Emitted when the session is stopped
     */
    void stopped();

private:
    std::unique_ptr<CaptureSessionPrivate> d;
};

/**
 * @brief Client-side proxy for ext_image_copy_capture_frame_v1
 *
 * This class represents a single frame capture request and accumulates
 * all frame metadata for easy testing.
 */
class CaptureFrame : public QObject
{
    Q_OBJECT

public:
    explicit CaptureFrame(QObject *parent = nullptr);
    ~CaptureFrame() override;

    /**
     * @brief Initialize the frame with a Wayland resource
     * @param resource The frame resource from create_frame
     */
    void setup(struct ::ext_image_copy_capture_frame_v1 *resource);

    /**
     * @brief Check if the frame is valid
     */
    bool isValid() const;

    /**
     * @brief Set the event queue for this frame
     */
    void setEventQueue(KWayland::Client::EventQueue *queue);

    /// Attach a buffer (expressed as a raw Wayland resource) to this frame.
    void attachBuffer(wl_buffer *buffer);

    /// Attach a shared-memory buffer to this frame.
    void attachBuffer(KWayland::Client::Buffer &shmBuffer);

    /**
     * @brief Add damage to the buffer
     * @param damage The damaged region
     */
    void damageBuffer(const QRect &damage);

    /**
     * @brief Request capture of this frame
     */
    void capture();

    /**
     * @brief Get the transform applied to the buffer contents
     */
    uint32_t transform() const;

    /**
     * @brief Get all damage regions reported by the compositor
     */
    QVector<QRect> damageRegions() const;

    /**
     * @brief Get the presentation time of the frame
     */
    std::chrono::nanoseconds presentationTime() const;

    /**
     * @brief Check if the frame capture is ready (ready event received)
     */
    bool isReady() const;

    /**
     * @brief Check if the frame capture failed
     */
    bool hasFailed() const;

    /**
     * @brief Get the failure reason (only valid if hasFailed() returns true)
     */
    uint32_t failureReason() const;

    /**
     * @brief Destroy the frame (calls protocol destroy)
     */
    void destroy();

Q_SIGNALS:
    /**
     * @brief Emitted when the frame is ready for reading
     */
    void ready();

    /**
     * @brief Emitted when the frame capture failed
     * @param reason The failure reason
     */
    void failed(uint32_t reason);

private:
    std::unique_ptr<CaptureFramePrivate> d;
};

} // namespace ImageCopyCapture
