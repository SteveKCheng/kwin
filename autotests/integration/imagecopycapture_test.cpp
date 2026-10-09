/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "compositor.h"
#include "core/output.h"
#include "core/outputbackend.h"
#include "core/outputconfiguration.h"
#include "cursor.h"
#include "generic_scene_opengl_test.h"
#include "input.h"
#include "main.h"
#include "pointer_input.h"
#include "wayland/surface.h"
#include "window.h"
#include "workspace.h"

#include <KWayland/Client/buffer.h>
#include <KWayland/Client/output.h>
#include <KWayland/Client/pointer.h>
#include <KWayland/Client/seat.h>
#include <KWayland/Client/shm_pool.h>
#include <KWayland/Client/surface.h>

#include <QElapsedTimer>
#include <QPainter>
#include <QTestEventLoop>

#include "wayland-ext-image-copy-capture-v1-client-protocol.h"
#include <wayland-client-protocol.h>

// Image copy capture proxy classes
#include "../wayland/client/imagecopyproxies.h"

using namespace ImageCopyCaptureClient;

namespace
{

/// Pixel-exact comparison of two images within @a rect (whole image if null).
bool sameImageContent(const QImage &actual, const QImage &expected, const QRect &rect = QRect())
{
    if (actual.size() != expected.size()) {
        qWarning() << "image size mismatch" << actual.size() << expected.size();
        return false;
    }
    const QImage a = actual.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage e = expected.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QRect area = rect.isNull() ? a.rect() : rect.intersected(a.rect());

    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (a.pixel(x, y) != e.pixel(x, y)) {
                qWarning() << "pixel mismatch at" << QPoint(x, y) << Qt::hex << a.pixel(x, y) << e.pixel(x, y);
                return false;
            }
        }
    }
    return true;
}

/// True if every pixel within @a rect has the given color.
bool isSolidColor(const QImage &image, const QRect &rect, const QColor &color)
{
    const QImage a = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QRgb expected = qPremultiply(color.rgba());
    const QRect area = rect.intersected(a.rect());
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (a.pixel(x, y) != expected) {
                qWarning() << "pixel at" << QPoint(x, y) << "is" << Qt::hex << a.pixel(x, y) << "expected" << expected;
                return false;
            }
        }
    }
    return true;
}

/// Union of a list of rectangles.
QRect boundingRect(const QVector<QRect> &rects)
{
    QRect result;
    for (const QRect &rect : rects) {
        result |= rect;
    }
    return result;
}

/// A test pattern that is asymmetric in both axes, so flips and rotations are detected.
QImage makeTestPattern(const QSize &size, const QColor &background = Qt::blue)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    QPainter p(&image);
    p.setPen(Qt::red);
    p.setBrush(Qt::yellow);
    p.drawRect(50, 50, 200, 100);
    p.setBrush(Qt::green);
    p.drawEllipse(150, 50, 110, 110);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawRect(size.width() - 40, size.height() - 20, 30, 10);
    return image;
}

} // anonymous namespace

namespace KWin
{

static const QString s_socketName = QStringLiteral("wayland_test_imagecopycapture-0");

/**
 * The result of one captured frame.
 */
struct CaptureResult
{
    bool ready = false;
    bool failed = false;
    uint32_t failureReason = 0;
    uint32_t transform = 0;
    QVector<QRect> damage;
};

/**
 * A client-side shm buffer with a QImage view onto its memory.
 */
struct ClientBuffer
{
    QWeakPointer<KWayland::Client::Buffer> buffer;
    QSize size;

    bool isValid() const
    {
        return !buffer.isNull();
    }

    /// Image view onto the buffer's memory (not a copy).
    QImage image() const
    {
        auto b = buffer.lock();
        return QImage(static_cast<uchar *>(b->address()), size.width(), size.height(), b->stride(), QImage::Format_ARGB32_Premultiplied);
    }

    void fill(const QColor &color)
    {
        QImage view = image();
        view.fill(color);
    }

    void release()
    {
        if (auto b = buffer.lock()) {
            b->setUsed(false);
        }
    }
};

class ImageCopyCaptureTest : public GenericSceneOpenGLTest
{
    Q_OBJECT

public:
    ImageCopyCaptureTest()
        : GenericSceneOpenGLTest(QByteArrayLiteral("O2"))
    {
    }

private Q_SLOTS:
    void init();
    void cleanup();

    void testOutputCapture();
    void testSecondOutput();
    void testPartialUpdate();
    void testClientBufferDamage();
    void testNoChangesNoFrame();
    void testCursorOverlay();
    void testBufferConstraints();
    void testOutputScaleChange();
    void testOutputRemoved();
    void testSessionOnRemovedOutput();
    void testMultipleSessions();
    void testFrameDestroyedWhilePending();
    void testSessionDestroyedWithPendingFrame();

private:
    /// Create a session for @a output and wait for its buffer constraints.
    std::unique_ptr<CaptureSession> createSession(KWayland::Client::Output *output, bool paintCursors = false);

    /// Allocate a client shm buffer of the given size (ARGB8888 unless told otherwise).
    ClientBuffer createBuffer(const QSize &size, KWayland::Client::Buffer::Format format = KWayland::Client::Buffer::Format::ARGB32);

    /**
     * Capture one frame into @a buffer.
     *
     * @param bufferDamage  Rectangles to pass as damage_buffer; by default the whole buffer.
     * @param timeout       How long to wait for ready/failed.
     */
    CaptureResult capture(CaptureSession *session, const ClientBuffer &buffer, const QVector<QRect> &bufferDamage, int timeout = 5000);
    CaptureResult capture(CaptureSession *session, const ClientBuffer &buffer, int timeout = 5000)
    {
        return capture(session, buffer, {QRect(QPoint(), buffer.size)}, timeout);
    }

    /// Show a full-screen window with @a image on @a output and wait until it is rendered.
    Window *showFullScreenWindow(KWayland::Client::Output *output, const QImage &image);

    /// Replace (part of) the window's contents and wait until the compositor has taken the commit.
    void updateWindow(Window *window, const QImage &image, const QRect &damage);

    /**
     * Run the event loop until @a condition holds or @a timeout ms have passed.
     *
     * Unlike QTRY_* / QTest::qWait, this blocks in the event loop, which is what makes
     * kwin flush its pending events to the client.
     */
    static bool waitUntil(const std::function<bool()> &condition, int timeout = 5000);

    /// (Re-)collect the client-side outputs, wait for their geometry, and sort them left to right.
    void refreshOutputs();

    std::unique_ptr<KWayland::Client::Surface> m_surface;
    std::unique_ptr<Test::XdgToplevel> m_shellSurface;

    /// The client-side outputs, sorted left to right.
    QList<KWayland::Client::Output *> m_outputs;
};

void ImageCopyCaptureTest::init()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::Seat
                                         | Test::AdditionalWaylandInterface::ImageCopyCaptureV1
                                         | Test::AdditionalWaylandInterface::ImageCaptureSourceV1));
    QVERIFY(Test::imageCopyCaptureManager());
    QVERIFY(Test::imageCaptureSourceManager());
    QVERIFY(Test::waylandSeat());

    refreshOutputs();
    QCOMPARE(m_outputs.size(), 2);
    QCOMPARE(m_outputs[0]->globalPosition(), QPoint(0, 0));
    QCOMPARE(m_outputs[1]->globalPosition(), QPoint(1280, 0));

    // Keep the cursor out of the picture unless a test wants it.
    Cursors::self()->hideCursor();
    input()->pointer()->warp(QPointF(1000, 1000));
}

void ImageCopyCaptureTest::cleanup()
{
    m_shellSurface.reset();
    m_surface.reset();
    m_outputs.clear();
    Cursors::self()->showCursor();

    // Restore the output layout in case a test changed it
    Test::setOutputConfig({
        Rect(0, 0, 1280, 1024),
        Rect(1280, 0, 1280, 1024),
    });

    Test::destroyWaylandConnection();
}

std::unique_ptr<CaptureSession> ImageCopyCaptureTest::createSession(KWayland::Client::Output *output, bool paintCursors)
{
    auto source = Test::imageCaptureSourceManager()->createSource(*output);
    if (!source || !source->isValid()) {
        qWarning() << "Failed to create capture source";
        return nullptr;
    }

    auto session = Test::imageCopyCaptureManager()->createSession(*source, paintCursors);
    if (!session) {
        qWarning() << "Failed to create capture session";
        return nullptr;
    }

    QSignalSpy constraintsSpy(session.get(), &CaptureSession::constraintsReady);
    if (!session->constraintsDone() && !constraintsSpy.wait()) {
        qWarning() << "Did not receive buffer constraints";
        return nullptr;
    }
    return session;
}

ClientBuffer ImageCopyCaptureTest::createBuffer(const QSize &size, KWayland::Client::Buffer::Format format)
{
    ClientBuffer result;
    result.size = size;
    result.buffer = Test::waylandShmPool()->getBuffer(size, size.width() * 4, format);
    return result;
}

CaptureResult ImageCopyCaptureTest::capture(CaptureSession *session, const ClientBuffer &buffer, const QVector<QRect> &bufferDamage, int timeout)
{
    CaptureResult result;

    auto frame = session->createFrame();
    if (!frame || !frame->isValid()) {
        qWarning() << "Failed to create frame";
        return result;
    }

    frame->attachBuffer(*buffer.buffer.lock());
    for (const QRect &rect : bufferDamage) {
        frame->damageBuffer(rect);
    }

    QSignalSpy readySpy(frame.get(), &CaptureFrame::ready);
    QSignalSpy failedSpy(frame.get(), &CaptureFrame::failed);
    frame->capture();

    // Wait for either event (or the timeout, for tests expecting no frame).
    // N.B. The wait must actually block in the event loop: kwin flushes events to
    // clients only when the loop is about to block, so QTest::qWait() would never
    // deliver the ready event.
    QElapsedTimer timer;
    timer.start();
    while (readySpy.isEmpty() && failedSpy.isEmpty() && timer.elapsed() < timeout) {
        readySpy.wait(qMin<qint64>(100, timeout - timer.elapsed()));
    }

    result.ready = frame->isReady();
    result.failed = frame->hasFailed();
    result.failureReason = result.failed ? frame->failureReason() : 0;
    result.transform = frame->transform();
    result.damage = frame->damageRegions();
    return result;
}

Window *ImageCopyCaptureTest::showFullScreenWindow(KWayland::Client::Output *output, const QImage &image)
{
    m_surface = Test::createSurface();
    if (!m_surface) {
        return nullptr;
    }
    m_shellSurface = Test::createXdgToplevelSurface(m_surface.get(), [output](Test::XdgToplevel *toplevel) {
        toplevel->set_fullscreen(output->output());
    });
    Window *window = Test::renderAndWaitForShown(m_surface.get(), image);
    if (!window) {
        return nullptr;
    }
    if (!window->isFullScreen() || window->frameGeometry() != window->output()->geometry()) {
        qWarning() << "window is not full screen" << window->frameGeometry();
        return nullptr;
    }
    // Let the compositor paint the window at least once
    QTest::qWait(100);
    return window;
}

bool ImageCopyCaptureTest::waitUntil(const std::function<bool()> &condition, int timeout)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() >= timeout) {
            return false;
        }
        QTestEventLoop::instance().enterLoopMSecs(50);
    }
    return true;
}

void ImageCopyCaptureTest::refreshOutputs()
{
    // The client-side output objects receive their geometry asynchronously;
    // wait until there are two outputs and every one knows its size and position.
    // If the outputs were recreated, also wait until the stale ones are gone.
    // Their order in the registry is not stable, so sort them left to right.
    const QList<KWayland::Client::Output *> stale = m_outputs;
    QVERIFY(waitUntil([&stale]() {
        const auto outputs = Test::waylandOutputs();
        return outputs.size() == 2 && std::ranges::all_of(outputs, [&stale](KWayland::Client::Output *o) {
            return !stale.contains(o) && !o->pixelSize().isEmpty();
        });
    }));
    m_outputs = Test::waylandOutputs();
    std::sort(m_outputs.begin(), m_outputs.end(), [](KWayland::Client::Output *a, KWayland::Client::Output *b) {
        return a->globalPosition().x() < b->globalPosition().x();
    });
}

void ImageCopyCaptureTest::updateWindow(Window *window, const QImage &image, const QRect &damage)
{
    QSignalSpy committedSpy(window->surface(), &SurfaceInterface::committed);
    m_surface->attachBuffer(Test::waylandShmPool()->createBuffer(image));
    m_surface->damage(damage);
    m_surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());
}

//
// Tests
//

void ImageCopyCaptureTest::testOutputCapture()
{
    // The most basic case: one full capture of an output showing a known pattern.
    const auto &outputs = m_outputs;
    QVERIFY(outputs.size() >= 2);
    auto *output = outputs[0];

    const QImage pattern = makeTestPattern(output->pixelSize());
    QVERIFY(showFullScreenWindow(output, pattern));

    auto session = createSession(output);
    QVERIFY(session);
    QCOMPARE(session->bufferSize(), output->pixelSize());
    QVERIFY(session->shmFormats().contains(WL_SHM_FORMAT_ARGB8888));

    ClientBuffer buffer = createBuffer(session->bufferSize());
    QVERIFY(buffer.isValid());
    buffer.fill(Qt::magenta); // sentinel: must be overwritten entirely

    const CaptureResult result = capture(session.get(), buffer);
    QVERIFY(!result.failed);
    QVERIFY(result.ready);
    QCOMPARE(result.transform, uint32_t(WL_OUTPUT_TRANSFORM_NORMAL));

    // The first frame of a session always carries full damage
    QCOMPARE(boundingRect(result.damage), QRect(QPoint(), buffer.size));

    QVERIFY(sameImageContent(buffer.image(), pattern));
    buffer.release();
}

void ImageCopyCaptureTest::testSecondOutput()
{
    // Capturing an output that does not sit at the origin must not pick up
    // the other output's contents or offset the picture.
    const auto &outputs = m_outputs;
    QVERIFY(outputs.size() >= 2);
    auto *output = outputs[1];
    QVERIFY(output->globalPosition() != QPoint(0, 0));

    const QImage pattern = makeTestPattern(output->pixelSize(), Qt::darkCyan);
    QVERIFY(showFullScreenWindow(output, pattern));

    auto session = createSession(output);
    QVERIFY(session);
    QCOMPARE(session->bufferSize(), output->pixelSize());

    ClientBuffer buffer = createBuffer(session->bufferSize());
    QVERIFY(buffer.isValid());
    const CaptureResult result = capture(session.get(), buffer);
    QVERIFY(result.ready);
    QVERIFY(sameImageContent(buffer.image(), pattern));
    buffer.release();
}

void ImageCopyCaptureTest::testPartialUpdate()
{
    // After the first frame, only what changed on screen is copied and reported.
    const auto &outputs = m_outputs;
    auto *output = outputs[0];

    QImage pattern = makeTestPattern(output->pixelSize());
    Window *window = showFullScreenWindow(output, pattern);
    QVERIFY(window);

    auto session = createSession(output);
    QVERIFY(session);

    ClientBuffer buffer = createBuffer(session->bufferSize());
    QVERIFY(buffer.isValid());
    QVERIFY(capture(session.get(), buffer).ready);
    QVERIFY(sameImageContent(buffer.image(), pattern));

    // Change a small rectangle of the window
    const QRect changed(400, 300, 120, 80);
    {
        QPainter p(&pattern);
        p.fillRect(changed, Qt::cyan);
    }
    updateWindow(window, pattern, changed);

    // The client's buffer already holds the previous frame, so it reports no buffer damage.
    const CaptureResult result = capture(session.get(), buffer, QVector<QRect>{});
    QVERIFY(result.ready);

    // Reported damage covers the change but not (much) more
    const QRect damage = boundingRect(result.damage);
    QVERIFY2(damage.contains(changed), qPrintable(QStringLiteral("damage %1 does not cover %2").arg(QDebug::toString(damage), QDebug::toString(changed))));
    QVERIFY2(changed.adjusted(-2, -2, 2, 2).contains(damage), qPrintable(QStringLiteral("damage %1 is much larger than the change %2").arg(QDebug::toString(damage), QDebug::toString(changed))));

    // The buffer as a whole now matches the new picture
    QVERIFY(sameImageContent(buffer.image(), pattern));
    buffer.release();
}

void ImageCopyCaptureTest::testClientBufferDamage()
{
    // A client handing in a buffer with stale contents must get those parts
    // refreshed even if nothing changed on screen there.
    const auto &outputs = m_outputs;
    auto *output = outputs[0];

    QImage pattern = makeTestPattern(output->pixelSize());
    Window *window = showFullScreenWindow(output, pattern);
    QVERIFY(window);

    auto session = createSession(output);
    QVERIFY(session);

    ClientBuffer first = createBuffer(session->bufferSize());
    QVERIFY(capture(session.get(), first).ready);
    QVERIFY(sameImageContent(first.image(), pattern));

    // Change a small rectangle on screen
    const QRect changed(100, 700, 60, 60);
    {
        QPainter p(&pattern);
        p.fillRect(changed, Qt::red);
    }
    updateWindow(window, pattern, changed);

    // Capture into a brand-new buffer, declared entirely stale: the whole picture must arrive,
    // although the reported damage is still only what changed on screen.
    ClientBuffer second = createBuffer(session->bufferSize());
    second.fill(Qt::magenta);
    CaptureResult result = capture(session.get(), second);
    QVERIFY(result.ready);
    QVERIFY(boundingRect(result.damage).contains(changed));
    QVERIFY(sameImageContent(second.image(), pattern));

    // Now a buffer with a partially stale region: only that region plus screen damage is copied.
    const QRect stale(900, 100, 200, 150);
    QImage expected = pattern;
    first.fill(Qt::magenta);
    {
        // Simulate "first" still holding the old picture outside the stale region
        QImage view = first.image();
        QPainter p(&view);
        p.drawImage(QPoint(), pattern);
        p.fillRect(stale, Qt::magenta);
    }
    const QRect changed2(1000, 800, 50, 50);
    {
        QPainter p(&pattern);
        p.fillRect(changed2, Qt::green);
    }
    updateWindow(window, pattern, changed2);

    result = capture(session.get(), first, {stale});
    QVERIFY(result.ready);
    QVERIFY(sameImageContent(first.image(), pattern));

    first.release();
    second.release();
}

void ImageCopyCaptureTest::testNoChangesNoFrame()
{
    // When nothing changes on screen, a capture request stays pending until something does.
    const auto &outputs = m_outputs;
    auto *output = outputs[0];

    QImage pattern = makeTestPattern(output->pixelSize());
    Window *window = showFullScreenWindow(output, pattern);
    QVERIFY(window);

    auto session = createSession(output);
    QVERIFY(session);

    ClientBuffer buffer = createBuffer(session->bufferSize());
    QVERIFY(capture(session.get(), buffer).ready);

    // Nothing changed: no ready within a reasonable time
    auto frame = session->createFrame();
    QVERIFY(frame && frame->isValid());
    frame->attachBuffer(*buffer.buffer.lock());
    QSignalSpy readySpy(frame.get(), &CaptureFrame::ready);
    QSignalSpy failedSpy(frame.get(), &CaptureFrame::failed);
    frame->capture();
    QVERIFY(!readySpy.wait(500));
    QVERIFY(failedSpy.isEmpty());

    // Now change the window: the pending frame completes
    const QRect changed(10, 10, 30, 30);
    {
        QPainter p(&pattern);
        p.fillRect(changed, Qt::white);
    }
    updateWindow(window, pattern, changed);
    QVERIFY(readySpy.count() > 0 || readySpy.wait());
    QVERIFY(failedSpy.isEmpty());
    QVERIFY(boundingRect(frame->damageRegions()).contains(changed));
    QVERIFY(sameImageContent(buffer.image(), pattern));

    frame.reset();
    buffer.release();
}

void ImageCopyCaptureTest::testCursorOverlay()
{
    // With paint_cursors, the cursor is composited into the frame; without, it is not.
    // Moving the cursor produces damage at both its old and new positions.
    const auto &outputs = m_outputs;
    auto *output = outputs[0];

    const QImage pattern = makeTestPattern(output->pixelSize());
    Window *window = showFullScreenWindow(output, pattern);
    QVERIFY(window);

    // Give the pointer a known 16x16 red cursor by focusing our window and setting a cursor surface
    std::unique_ptr<KWayland::Client::Pointer> pointer(Test::waylandSeat()->createPointer());
    QSignalSpy enteredSpy(pointer.get(), &KWayland::Client::Pointer::entered);
    Cursors::self()->showCursor();
    const QPoint cursorPos(600, 500);
    input()->pointer()->warp(cursorPos);
    QCOMPARE(input()->pointer()->focus(), window);
    QVERIFY(enteredSpy.wait());

    const QSize cursorSize(16, 16);
    QImage cursorImage(cursorSize, QImage::Format_ARGB32_Premultiplied);
    cursorImage.fill(Qt::red);
    auto cursorSurface = Test::createSurface();
    QSignalSpy cursorRenderedSpy(cursorSurface.get(), &KWayland::Client::Surface::frameRendered);
    cursorSurface->attachBuffer(Test::waylandShmPool()->createBuffer(cursorImage));
    cursorSurface->damage(cursorImage.rect());
    cursorSurface->commit();
    pointer->setCursor(cursorSurface.get(), QPoint(0, 0));
    QVERIFY(cursorRenderedSpy.wait());
    QTRY_COMPARE(kwinApp()->cursorImage().image().size(), cursorSize);
    QVERIFY(isSolidColor(kwinApp()->cursorImage().image(), QRect(QPoint(), cursorSize), Qt::red));

    const QRect cursorRect(cursorPos, cursorSize);

    // Session without cursor: picture equals the window
    {
        auto session = createSession(output, false);
        QVERIFY(session);
        ClientBuffer buffer = createBuffer(session->bufferSize());
        QVERIFY(capture(session.get(), buffer).ready);
        QVERIFY(sameImageContent(buffer.image(), pattern));
        buffer.release();
    }

    // Session with cursor: red square at the cursor position, window elsewhere
    auto session = createSession(output, true);
    QVERIFY(session);
    ClientBuffer buffer = createBuffer(session->bufferSize());
    QVERIFY(capture(session.get(), buffer).ready);
    QVERIFY(isSolidColor(buffer.image(), cursorRect, Qt::red));
    QVERIFY(sameImageContent(buffer.image(), pattern, QRect(0, 0, 500, 400)));

    // Move the cursor: the next frame erases it at the old position and paints it at the new one
    const QPoint newPos(200, 800);
    input()->pointer()->warp(newPos);
    const CaptureResult result = capture(session.get(), buffer, QVector<QRect>{});
    QVERIFY(result.ready);
    const QRect damage = boundingRect(result.damage);
    QVERIFY(damage.contains(cursorRect));
    QVERIFY(damage.contains(QRect(newPos, cursorSize)));
    QVERIFY(sameImageContent(buffer.image(), pattern, cursorRect));
    QVERIFY(isSolidColor(buffer.image(), QRect(newPos, cursorSize), Qt::red));
    buffer.release();

    // Hiding the cursor removes it from the next frame
    Cursors::self()->hideCursor();
    ClientBuffer buffer2 = createBuffer(session->bufferSize());
    QVERIFY(capture(session.get(), buffer2).ready);
    QVERIFY(sameImageContent(buffer2.image(), pattern));
    buffer2.release();
}

void ImageCopyCaptureTest::testBufferConstraints()
{
    // Buffers that do not satisfy the advertised constraints fail the frame
    // with buffer_constraints, and the session keeps working afterwards.
    const auto &outputs = m_outputs;
    auto *output = outputs[0];

    const QImage pattern = makeTestPattern(output->pixelSize());
    QVERIFY(showFullScreenWindow(output, pattern));

    auto session = createSession(output);
    QVERIFY(session);
    const QSize size = session->bufferSize();

    // Wrong size
    {
        ClientBuffer small = createBuffer(size - QSize(10, 10));
        QVERIFY(small.isValid());
        const CaptureResult result = capture(session.get(), small);
        QVERIFY(result.failed);
        QCOMPARE(result.failureReason, uint32_t(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS));
        small.release();
    }

    // Wrong format (no alpha)
    {
        ClientBuffer xrgb = createBuffer(size, KWayland::Client::Buffer::Format::RGB32);
        QVERIFY(xrgb.isValid());
        const CaptureResult result = capture(session.get(), xrgb);
        QVERIFY(result.failed);
        QCOMPARE(result.failureReason, uint32_t(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS));
        xrgb.release();
    }

    // A correct buffer still works, and still gets the full first frame
    {
        ClientBuffer good = createBuffer(size);
        const CaptureResult result = capture(session.get(), good);
        QVERIFY(result.ready);
        QCOMPARE(boundingRect(result.damage), QRect(QPoint(), size));
        QVERIFY(sameImageContent(good.image(), pattern));
        good.release();
    }
}

void ImageCopyCaptureTest::testOutputScaleChange()
{
    // Changing the output's mode and scale changes its pixel size: the session
    // re-advertises constraints, old-size buffers are rejected, new-size buffers work.

    // Give the first output a second mode to switch to.  (Test::setOutputConfig
    // recreates the outputs, so this has to happen before the session exists.)
    Test::setOutputConfig({
        Test::OutputInfo{
            .geometry = Rect(0, 0, 1280, 1024),
            .modes = {
                OutputModeline(QSize(1280, 1024), 60000, OutputModeline::Flag::Preferred),
                OutputModeline(QSize(1920, 1080), 60000),
            },
        },
        Test::OutputInfo{.geometry = Rect(1280, 0, 1280, 1024)},
    });
    refreshOutputs();
    const auto &outputs = m_outputs;
    auto *output = outputs[0];
    QCOMPARE(output->pixelSize(), QSize(1280, 1024));

    const QImage pattern = makeTestPattern(output->pixelSize());
    QVERIFY(showFullScreenWindow(output, pattern));

    auto session = createSession(output);
    QVERIFY(session);
    const QSize oldSize = session->bufferSize();
    QCOMPARE(oldSize, QSize(1280, 1024));

    ClientBuffer oldBuffer = createBuffer(oldSize);
    QVERIFY(capture(session.get(), oldBuffer).ready);

    // Switch the same output to 1920x1080 at scale 2, as a display settings change would.
    QSignalSpy constraintsSpy(session.get(), &CaptureSession::constraintsReady);
    QSignalSpy stoppedSpy(session.get(), &CaptureSession::stopped);
    {
        OutputConfiguration config;
        auto changeSet = config.changeSet(kwinApp()->outputBackend()->outputs()[0]);
        changeSet->currentMode = OutputModeline(QSize(1920, 1080), 60000);
        changeSet->desiredMode = OutputModeline(QSize(1920, 1080), 60000);
        changeSet->scale = 2;
        changeSet->scaleSetting = 2;
        QCOMPARE(workspace()->applyOutputConfiguration(config), OutputConfigurationError::None);
    }
    QVERIFY(constraintsSpy.count() > 0 || constraintsSpy.wait());
    QVERIFY(stoppedSpy.isEmpty());
    const QSize newSize = session->bufferSize();
    QCOMPARE(newSize, QSize(1920, 1080));
    QVERIFY(newSize != oldSize);

    // The old buffer no longer fits
    CaptureResult result = capture(session.get(), oldBuffer);
    QVERIFY(result.failed);
    QCOMPARE(result.failureReason, uint32_t(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS));
    oldBuffer.release();

    // A new buffer works and gets full damage
    QTest::qWait(100); // let the window re-render at the new scale
    ClientBuffer newBuffer = createBuffer(newSize);
    QVERIFY(newBuffer.isValid());
    result = capture(session.get(), newBuffer);
    QVERIFY(result.ready);
    QCOMPARE(boundingRect(result.damage), QRect(QPoint(), newSize));
    newBuffer.release();
}

void ImageCopyCaptureTest::testOutputRemoved()
{
    // Removing the captured output stops the session and fails a pending frame.
    const auto &outputs = m_outputs;
    QVERIFY(outputs.size() >= 2);
    auto *output = outputs[1];

    const QImage pattern = makeTestPattern(output->pixelSize());
    QVERIFY(showFullScreenWindow(output, pattern));

    auto session = createSession(output);
    QVERIFY(session);

    ClientBuffer buffer = createBuffer(session->bufferSize());
    QVERIFY(capture(session.get(), buffer).ready);

    // Leave a frame pending (nothing changes, so it is not completed)
    auto frame = session->createFrame();
    frame->attachBuffer(*buffer.buffer.lock());
    QSignalSpy failedSpy(frame.get(), &CaptureFrame::failed);
    QSignalSpy stoppedSpy(session.get(), &CaptureSession::stopped);
    frame->capture();

    Test::setOutputConfig({
        Rect(0, 0, 1280, 1024),
    });

    QVERIFY(stoppedSpy.count() > 0 || stoppedSpy.wait());
    QVERIFY(session->isStopped());
    QVERIFY(failedSpy.count() > 0 || failedSpy.wait());
    QCOMPARE(frame->failureReason(), uint32_t(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED));

    frame.reset();
    buffer.release();
}

void ImageCopyCaptureTest::testSessionOnRemovedOutput()
{
    // A client may create a session from a source whose output has just been removed,
    // before it learns about the removal.  That is not a protocol error: the session
    // is created and stopped right away, and frames on it fail with "stopped".
    const auto &outputs = m_outputs;
    QVERIFY(outputs.size() >= 2);
    const QList<KWayland::Client::Output *> oldOutputs = outputs;

    auto source = Test::imageCaptureSourceManager()->createSource(*outputs[1]);
    QVERIFY(source && source->isValid());
    // Make sure the compositor has processed create_source before the output goes
    Test::flushWaylandConnection();
    QTestEventLoop::instance().enterLoopMSecs(100);

    Test::setOutputConfig({
        Rect(0, 0, 1280, 1024),
    });
    QVERIFY(waitUntil([&oldOutputs]() {
        const auto now = Test::waylandOutputs();
        return now.size() == 1 && !oldOutputs.contains(now.first()) && !now.first()->pixelSize().isEmpty();
    }));

    auto session = Test::imageCopyCaptureManager()->createSession(*source, false);
    QVERIFY(session);
    QSignalSpy stoppedSpy(session.get(), &CaptureSession::stopped);
    QVERIFY(session->isStopped() || stoppedSpy.wait());

    auto frame = session->createFrame();
    QVERIFY(frame && frame->isValid());
    ClientBuffer buffer = createBuffer(QSize(64, 64));
    frame->attachBuffer(*buffer.buffer.lock());
    QSignalSpy failedSpy(frame.get(), &CaptureFrame::failed);
    frame->capture();
    QVERIFY(failedSpy.count() > 0 || failedSpy.wait());
    QCOMPARE(frame->failureReason(), uint32_t(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED));
    frame.reset();
    buffer.release();

    // The connection survived: a session on the remaining output works normally
    auto *remaining = Test::waylandOutputs().first();
    const QImage pattern = makeTestPattern(remaining->pixelSize());
    QVERIFY(showFullScreenWindow(remaining, pattern));
    auto session2 = createSession(remaining);
    QVERIFY(session2);
    ClientBuffer buffer2 = createBuffer(session2->bufferSize());
    QVERIFY(capture(session2.get(), buffer2).ready);
    QVERIFY(sameImageContent(buffer2.image(), pattern));
    buffer2.release();
}

void ImageCopyCaptureTest::testMultipleSessions()
{
    // Several sessions, on the same and on different outputs, are independent.
    const auto &outputs = m_outputs;
    QVERIFY(outputs.size() >= 2);

    const QImage pattern = makeTestPattern(outputs[0]->pixelSize());
    Window *window = showFullScreenWindow(outputs[0], pattern);
    QVERIFY(window);

    auto sessionA = createSession(outputs[0]);
    auto sessionB = createSession(outputs[0]);
    auto sessionC = createSession(outputs[1]);
    QVERIFY(sessionA && sessionB && sessionC);

    ClientBuffer bufferA = createBuffer(sessionA->bufferSize());
    ClientBuffer bufferB = createBuffer(sessionB->bufferSize());
    ClientBuffer bufferC = createBuffer(sessionC->bufferSize());

    QVERIFY(capture(sessionA.get(), bufferA).ready);
    QVERIFY(capture(sessionB.get(), bufferB).ready);
    QVERIFY(capture(sessionC.get(), bufferC).ready);
    QVERIFY(sameImageContent(bufferA.image(), pattern));
    QVERIFY(sameImageContent(bufferB.image(), pattern));

    // A change on output 0 is seen by both A and B, each with its own damage tracking.
    // Session C (other output) sees nothing.
    QImage updated = pattern;
    const QRect changed(300, 300, 40, 40);
    {
        QPainter p(&updated);
        p.fillRect(changed, Qt::white);
    }
    updateWindow(window, updated, changed);

    // Capture A twice: the second time there is no new damage
    CaptureResult result = capture(sessionA.get(), bufferA, QVector<QRect>{});
    QVERIFY(result.ready);
    QVERIFY(boundingRect(result.damage).contains(changed));
    QVERIFY(sameImageContent(bufferA.image(), updated));

    result = capture(sessionA.get(), bufferA, QVector<QRect>{}, 300);
    QVERIFY(!result.ready && !result.failed);

    // B still has the change pending
    result = capture(sessionB.get(), bufferB, QVector<QRect>{});
    QVERIFY(result.ready);
    QVERIFY(boundingRect(result.damage).contains(changed));
    QVERIFY(sameImageContent(bufferB.image(), updated));

    // C has nothing
    result = capture(sessionC.get(), bufferC, QVector<QRect>{}, 300);
    QVERIFY(!result.ready && !result.failed);

    bufferA.release();
    bufferB.release();
    bufferC.release();
}

void ImageCopyCaptureTest::testFrameDestroyedWhilePending()
{
    // Destroying a frame that is waiting for changes must not disturb the session;
    // a later frame still gets the changes.
    const auto &outputs = m_outputs;
    auto *output = outputs[0];

    QImage pattern = makeTestPattern(output->pixelSize());
    Window *window = showFullScreenWindow(output, pattern);
    QVERIFY(window);

    auto session = createSession(output);
    QVERIFY(session);
    ClientBuffer buffer = createBuffer(session->bufferSize());
    QVERIFY(capture(session.get(), buffer).ready);

    {
        auto frame = session->createFrame();
        frame->attachBuffer(*buffer.buffer.lock());
        frame->capture();
        Test::flushWaylandConnection();
        // destroyed here, while pending
    }

    const QRect changed(50, 600, 70, 70);
    {
        QPainter p(&pattern);
        p.fillRect(changed, Qt::yellow);
    }
    updateWindow(window, pattern, changed);

    const CaptureResult result = capture(session.get(), buffer, QVector<QRect>{});
    QVERIFY(result.ready);
    QVERIFY(boundingRect(result.damage).contains(changed));
    QVERIFY(sameImageContent(buffer.image(), pattern));
    buffer.release();
}

void ImageCopyCaptureTest::testSessionDestroyedWithPendingFrame()
{
    // The protocol allows the session to go away while a frame exists.
    // This must not crash, and the frame can still be destroyed afterwards.
    const auto &outputs = m_outputs;
    auto *output = outputs[0];

    const QImage pattern = makeTestPattern(output->pixelSize());
    QVERIFY(showFullScreenWindow(output, pattern));

    auto session = createSession(output);
    QVERIFY(session);
    ClientBuffer buffer = createBuffer(session->bufferSize());
    QVERIFY(capture(session.get(), buffer).ready);

    auto frame = session->createFrame();
    frame->attachBuffer(*buffer.buffer.lock());
    frame->capture();
    Test::flushWaylandConnection();

    session.reset();
    Test::flushWaylandConnection();
    QTest::qWait(50);

    frame.reset();
    Test::flushWaylandConnection();
    QTest::qWait(50);

    // The compositor is still healthy: a new session works
    auto session2 = createSession(output);
    QVERIFY(session2);
    QVERIFY(capture(session2.get(), buffer).ready);
    QVERIFY(sameImageContent(buffer.image(), pattern));
    buffer.release();
}

} // namespace KWin

WAYLANDTEST_MAIN(KWin::ImageCopyCaptureTest)
#include "imagecopycapture_test.moc"
