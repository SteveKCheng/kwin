/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "compositor.h"
#include "core/output.h"
#include "generic_scene_opengl_test.h"
#include "opengl/glplatform.h"
#include "pointer_input.h"
#include "window.h"
#include "workspace.h"

#include <KWayland/Client/output.h>
#include <KWayland/Client/shm_pool.h>
#include <KWayland/Client/surface.h>
#include <QPainter>

// Image copy capture proxy classes
#include "../wayland/client/imagecopyproxies.h"

namespace
{

bool hasSameImageContent(const QImage &img1, const QImage &img2)
{
    if (img1.size() != img2.size()) {
        return false;
    }

    for (int y = 0; y < img1.height(); ++y) {
        for (int x = 0; x < img1.width(); ++x) {
            auto p1 = img1.pixel(x, y);
            auto p2 = img2.pixel(x, y);
            if (p1 != p2) {
                return false;
            }
        }
    }

    return true;
}

} // anonymous namespace

namespace KWin
{

static const QString s_socketName = QStringLiteral("wayland_test_imagecopycapture-0");

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
    void testOutputCapture();

private:
    std::optional<QImage> captureOutput(KWayland::Client::LogicalOutput *output);
};

void ImageCopyCaptureTest::init()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::ImageCopyCaptureV1 | Test::AdditionalWaylandInterface::ImageCaptureSourceV1));
    QVERIFY(Test::imageCopyCaptureManager());
    QVERIFY(Test::imageCaptureSourceManager());
    Cursors::self()->hideCursor();
}

std::optional<QImage> ImageCopyCaptureTest::captureOutput(KWayland::Client::LogicalOutput *output)
{
    // Create capture source for the output
    auto captureSource = Test::imageCaptureSourceManager()->createSource(*output);
    if (!captureSource || !captureSource->isValid()) {
        qDebug() << "Failed to create capture source";
        return std::nullopt;
    }

    // Create capture session
    auto session = Test::imageCopyCaptureManager()->createSession(*captureSource, false);
    if (!session) {
        qDebug() << "Failed to create capture session";
        return std::nullopt;
    }

    // Get buffer constraints
    QSignalSpy constraintsSpy(session.get(), &ImageCopyCaptureClient::CaptureSession::constraintsReady);
    if (!constraintsSpy.wait() || !session->constraintsDone()) {
        qDebug() << "Failed to get buffer constraints";
        return std::nullopt;
    }
    auto bufferSize = session->bufferSize();
    if (bufferSize.isEmpty()) {
        qDebug() << "Invalid buffer size";
        return std::nullopt;
    }

    // Create frame
    auto frame = session->createFrame();
    if (!frame || !frame->isValid()) {
        qDebug() << "Failed to create frame";
        return std::nullopt;
    }

    // Create SHM buffer
    auto buffer = Test::waylandShmPool()->getBuffer(bufferSize,
                                                    bufferSize.width() * 4,
                                                    KWayland::Client::Buffer::Format::ARGB32);
    if (!buffer) {
        qDebug() << "Failed to create SHM buffer";
        return std::nullopt;
    }
    frame->attachBuffer(*buffer.lock());

    // Want the entire buffer
    frame->damageBuffer(QRect(QPoint(0, 0), bufferSize));

    // Start capture
    QSignalSpy readySpy(frame.get(), &ImageCopyCaptureClient::CaptureFrame::ready);
    QSignalSpy failedSpy(frame.get(), &ImageCopyCaptureClient::CaptureFrame::failed);
    frame->capture();

    if (!readySpy.wait() && failedSpy.count() == 0) {
        qDebug() << "Capture timeout";
        return std::nullopt;
    }

    if (failedSpy.count() > 0) {
        qDebug() << "Frame capture failed with reason:" << failedSpy.first().first().toUInt();
        return std::nullopt;
    }

    if (!frame->isReady() || frame->hasFailed()) {
        qDebug() << "Frame not ready or failed";
        return std::nullopt;
    }

    // Extract the image from the SHM buffer
    auto bufferData = static_cast<uchar *>(buffer.lock()->address());
    if (!bufferData) {
        qDebug() << "Failed to get buffer data";
        return std::nullopt;
    }

    QImage image(bufferData, bufferSize.width(), bufferSize.height(),
                 bufferSize.width() * 4, QImage::Format_ARGB32_Premultiplied);

    // Make a copy since the SHM buffer will go out of scope
    return image.copy();
}

void ImageCopyCaptureTest::testOutputCapture()
{
    auto waylandOutputs = Test::waylandOutputs();
    QVERIFY(!waylandOutputs.isEmpty());
    auto theOutput = waylandOutputs.constFirst();

    // Create a fullscreen surface
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    QVERIFY(surface != nullptr);
    std::unique_ptr<Test::XdgToplevel> shellSurface = Test::createXdgToplevelSurface(surface.get(), [theOutput](Test::XdgToplevel *toplevel) {
        toplevel->set_fullscreen(theOutput->output());
    });

    // Create a distinctive test pattern
    QImage sourceImage(theOutput->pixelSize(), QImage::Format_ARGB32_Premultiplied);
    sourceImage.fill(Qt::blue);
    {
        QPainter p(&sourceImage);
        p.setPen(Qt::red);
        p.setBrush(Qt::yellow);
        p.drawRect(50, 50, 200, 100);

        p.setBrush(Qt::green);
        p.drawEllipse(150, 50, 110, 110);

        QFont font;
        font.setHintingPreference(QFont::PreferNoHinting);
        font.setPointSize(24);
        p.setFont(font);
        p.setPen(Qt::black);
        p.drawText(60, 100, "ImageCopyCapture Test");
    }

    // Commit output for compositor
    Window *window = Test::renderAndWaitForShown(surface.get(), sourceImage);
    QVERIFY(window);
    QVERIFY(window->isFullScreen());
    QCOMPARE(window->frameGeometry(), window->output()->geometry());

    // Let the compositor process the frame
    QTest::qWait(100);

    // Capture the output
    auto capturedImage = captureOutput(theOutput);
    QVERIFY(capturedImage.has_value());

    // Convert to the same format for comparison
    capturedImage->convertTo(sourceImage.format());

    /*
    capturedImage->save("_captured.png");
    sourceImage.save("_source.png");
    */

    QVERIFY(hasSameImageContent(*capturedImage, sourceImage));
}

}

WAYLANDTEST_MAIN(KWin::ImageCopyCaptureTest)
#include "imagecopycapture_test.moc"
