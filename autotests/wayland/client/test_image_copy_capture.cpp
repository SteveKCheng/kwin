/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

// Qt
#include <QSignalSpy>
#include <QTest>
#include <QThread>

// KWin
#include "core/gpumanager.h"
#include "wayland/compositor.h"
#include "wayland/display.h"
#include "wayland/image_capture_source_v1.h"
#include "wayland/image_copy_capture_v1.h"
#include "wayland/output.h"

// KWayland Client
#include "KWayland/Client/compositor.h"
#include "KWayland/Client/connection_thread.h"
#include "KWayland/Client/event_queue.h"
#include "KWayland/Client/output.h"
#include "KWayland/Client/registry.h"
#include "KWayland/Client/shm_pool.h"

#include <cerrno> // for EPROTO

// Classes used for testing
#include "../../../tests/fakeoutput.h"
#include "imagecopyproxies.h"

/**
 * Test-specific implementation of the ImageCopyCaptureSessionV1Interface
 */
class TestImageCopyCaptureSession : public KWin::ImageCopyCaptureSessionV1Interface
{
    Q_OBJECT

public:
    explicit TestImageCopyCaptureSession(wl_resource *resource,
                                         KWin::ImageCopyCaptureManagerV1Interface *manager,
                                         KWin::LogicalOutput *output,
                                         bool overlayCursor)
        : KWin::ImageCopyCaptureSessionV1Interface(resource, manager)
        , m_output(output)
        , m_overlayCursor(overlayCursor)
    {
        // Send buffer constraints immediately for testing
        sendBufferSize(QSize(800, 600));
        sendShmFormat(0); // WL_SHM_FORMAT_ARGB8888
        sendConstraintsDone();
    }

protected:
    void captureFrame() override
    {
        // Simple test implementation - just signal that a frame was requested
        Q_EMIT frameCaptureRequested();

        // For testing, simulate a successful capture immediately
        auto *frame = getCurrentFrame();
        QVERIFY(frame);
        frame->sendReady();
    }

    void damageClientBuffer(const QRect &damage) override
    {
        m_clientBufferDamage.append(damage);
        Q_EMIT clientBufferDamaged(damage);
    }

    void frameDestroyed() override
    {
        m_clientBufferDamage.clear();
    }

public:
    KWin::LogicalOutput *output() const
    {
        return m_output.get();
    }
    bool overlayCursor() const
    {
        return m_overlayCursor;
    }
    QVector<QRect> clientBufferDamage() const
    {
        return m_clientBufferDamage;
    }

Q_SIGNALS:
    void frameCaptureRequested();
    void clientBufferDamaged(const QRect &damage);

private:
    QPointer<KWin::LogicalOutput> m_output;
    bool m_overlayCursor;
    QVector<QRect> m_clientBufferDamage;
};

/**
 * Test-specific implementation of the ImageCopyCaptureManagerV1Interface
 */
class TestImageCopyCaptureManager : public KWin::ImageCopyCaptureManagerV1Interface
{
    Q_OBJECT

public:
    explicit TestImageCopyCaptureManager(KWin::Display *display, QObject *parent = nullptr)
        : KWin::ImageCopyCaptureManagerV1Interface(display, parent)
    {
    }

protected:
    KWin::ImageCopyCaptureSessionV1Interface *createSession(wl_resource *resource,
                                                            KWin::LogicalOutput *output,
                                                            bool overlayCursor) override
    {
        auto *session = new TestImageCopyCaptureSession(resource, this, output, overlayCursor);
        m_sessions.append(session);
        Q_EMIT sessionCreated(output, overlayCursor);
        return session;
    }

public:
    const QVector<TestImageCopyCaptureSession *> &sessions() const
    {
        return m_sessions;
    }

Q_SIGNALS:
    void sessionCreated(KWin::LogicalOutput *output, bool overlayCursor);

private:
    QVector<TestImageCopyCaptureSession *> m_sessions;
};

/**
 * Main test class for ext-image-copy-capture protocol
 */
class TestImageCopyCapture : public QObject
{
    Q_OBJECT

public:
    explicit TestImageCopyCapture(QObject *parent = nullptr);

private Q_SLOTS:
    /// Initialize required server-side objects to test the image-copy-capture protocol.
    void initTestCase();

    /// Clean up all server-side and client-side objects used during testing.
    void cleanupTestCase();

    /// Test against dummy server-side implementation defined in this test program
    void testDummyServer();

    /// Test that error is signaled when protocol is misused.
    void testProtocolError();

private:
    std::unique_ptr<KWin::Display> m_display;
    KWin::CompositorInterface *m_compositorInterface;
    KWin::OutputInterface *m_outputInterface;
    std::unique_ptr<FakeBackendOutput> m_backendOutput;
    std::unique_ptr<KWin::LogicalOutput> m_output;
    std::unique_ptr<TestImageCopyCaptureManager> m_copyCaptureManager;
    std::unique_ptr<KWin::OutputImageCaptureSourceManagerV1Interface> m_captureSourceManager;

    std::unique_ptr<QThread> m_thread;
    KWayland::Client::ConnectionThread *m_connection;
    std::unique_ptr<KWayland::Client::EventQueue> m_queue;
    std::unique_ptr<KWayland::Client::Registry> m_registry;

    std::unique_ptr<KWayland::Client::Output> m_clientOutput;
    std::unique_ptr<KWayland::Client::ShmPool> m_clientShmPool;
    std::unique_ptr<ImageCopyCaptureClient::OutputSourceManager> m_captureSourceClient;
    std::unique_ptr<ImageCopyCaptureClient::CaptureManager> m_copyCaptureClient;

    /// Initialize objects for the client side of the Wayland connection.
    void initClient();

    /// Clean up (destroy) objects for the client side of the Wayland connection.
    void cleanupClient();

    /// Create capture session assuming #initClient has completed successfully
    void createSession(std::unique_ptr<ImageCopyCaptureClient::CaptureSession> &session);

    /// Test that error is signaled when protocol is misused.
    void testProtocolErrorInternal(int subcase);
};

static const QString s_socketName = QStringLiteral("kwin-test-image-copy-capture-0");

TestImageCopyCapture::TestImageCopyCapture(QObject *parent)
    : QObject(parent)
    , m_compositorInterface(nullptr)
    , m_outputInterface(nullptr)
    , m_connection(nullptr)
{
}

void TestImageCopyCapture::initTestCase()
{
    using namespace KWin;

    // Required by ShmClientBuffer (it tries to wrap shm buffers in udmabufs)
    GpuManager::s_self = std::make_unique<GpuManager>();

    m_display = std::make_unique<KWin::Display>(this);
    m_display->addSocketName(s_socketName);
    m_display->start();
    QVERIFY(m_display->isRunning());
    m_display->createShm();

    m_compositorInterface = new CompositorInterface(m_display.get(), m_display.get());

    // Create a fake output for testing
    m_backendOutput = std::make_unique<FakeBackendOutput>();
    m_backendOutput->setMode(QSize(800, 600), 60000);
    m_backendOutput->setScale(1.0);
    m_output = std::make_unique<KWin::LogicalOutput>(m_backendOutput.get());

    // Create output interface to expose it to clients
    m_outputInterface = new KWin::OutputInterface(m_display.get(), m_output.get(), this);

    // Create the source manager (needed for ext-image-capture-source protocol)
    m_captureSourceManager = std::make_unique<OutputImageCaptureSourceManagerV1Interface>(m_display.get(), this);

    // Create our test manager
    m_copyCaptureManager = std::make_unique<TestImageCopyCaptureManager>(m_display.get(), this);
}

void TestImageCopyCapture::cleanupTestCase()
{
    cleanupClient();

    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        m_thread.reset();
    }

    m_copyCaptureManager.reset();
    m_captureSourceManager.reset();
    m_output.reset();
    m_backendOutput.reset();

    m_display.reset();
    KWin::GpuManager::s_self.reset();

    // These objects are deleted when the Wayland resource is deleted
    m_compositorInterface = nullptr;
    m_outputInterface = nullptr;
}

void TestImageCopyCapture::cleanupClient()
{
    m_copyCaptureClient.reset();
    m_captureSourceClient.reset();
    m_clientShmPool.reset();
    m_clientOutput.reset();
    m_registry.reset();
    m_queue.reset();

    if (m_connection) {
        // m_connection is managed by m_thread so must defer deletion
        m_connection->deleteLater();
        m_connection = nullptr;
    }
}

void TestImageCopyCapture::initClient()
{
    cleanupClient();

    if (!m_thread) {
        m_thread = std::make_unique<QThread>(this);
        m_thread->start();
    }

    // Set up client connection to be dispatched by m_thread
    m_connection = new KWayland::Client::ConnectionThread();
    QSignalSpy connectedSpy(m_connection, &KWayland::Client::ConnectionThread::connected);
    m_connection->setSocketName(s_socketName);
    m_connection->moveToThread(m_thread.get());
    m_connection->initConnection();
    QVERIFY(connectedSpy.wait());

    m_queue = std::make_unique<KWayland::Client::EventQueue>(this);
    m_queue->setup(m_connection);

    m_registry = std::make_unique<KWayland::Client::Registry>();
    auto &registry = *m_registry;

    QSignalSpy spy{&registry, &KWayland::Client::Registry::interfacesAnnounced};
    registry.setEventQueue(m_queue.get());
    registry.create(m_connection->display());
    QVERIFY(registry.isValid());

    auto conn1 = connect(&registry, &KWayland::Client::Registry::interfaceAnnounced, this,
                         [&](const QByteArray &interface, quint32 name, quint32 version) {
        if (interface == ImageCopyCaptureClient::CaptureManager::interfaceName()) {
            Q_ASSERT(!m_copyCaptureClient); // there should not be more than one version
            m_copyCaptureClient = std::make_unique<ImageCopyCaptureClient::CaptureManager>(this);
            m_copyCaptureClient->setup(registry.registry(), name, version);
            m_copyCaptureClient->setEventQueue(registry.eventQueue());
        }
    });

    auto conn2 = connect(&registry, &KWayland::Client::Registry::interfaceAnnounced, this,
                         [&](const QByteArray &interface, quint32 name, quint32 version) {
        if (interface == ImageCopyCaptureClient::OutputSourceManager::interfaceName()) {
            Q_ASSERT(!m_captureSourceClient); // there should not be more than one version
            m_captureSourceClient = std::make_unique<ImageCopyCaptureClient::OutputSourceManager>(this);
            m_captureSourceClient->setup(registry.registry(), name, version);
            m_captureSourceClient->setEventQueue(registry.eventQueue());
        }
    });

    quint32 outputIfaceNumber = 0;
    quint32 outputIfaceVersion = 0;
    auto conn3 = connect(&registry, &KWayland::Client::Registry::outputAnnounced, this,
                         [&](quint32 name, quint32 version) {
        if (version > outputIfaceVersion) {
            outputIfaceNumber = name;
            outputIfaceVersion = version;
        }
    });

    // Listen for latest version of SHM interface
    quint32 shmIfaceNumber = 0;
    quint32 shmIfaceVersion = 0;
    auto conn4 = connect(&registry, &KWayland::Client::Registry::shmAnnounced, this,
                         [&](quint32 name, quint32 version) {
        if (version > shmIfaceVersion) {
            shmIfaceNumber = name;
            shmIfaceVersion = version;
        }
    });

    registry.setup();
    bool success = spy.wait();

    QObject::disconnect(conn4);
    QObject::disconnect(conn3);
    QObject::disconnect(conn2);
    QObject::disconnect(conn1);

    QVERIFY2(success, "Announcement of Wayland interfaces did not occur");

    if (shmIfaceVersion > 0) {
        m_clientShmPool.reset(registry.createShmPool(shmIfaceNumber, shmIfaceVersion));
    }
    if (outputIfaceVersion > 0) {
        m_clientOutput.reset(registry.createOutput(outputIfaceNumber, outputIfaceVersion));
    }

    QVERIFY(m_copyCaptureClient);
    QVERIFY(m_captureSourceClient);
    QVERIFY(m_clientOutput);
    QVERIFY(m_clientShmPool);

    QVERIFY(m_copyCaptureClient->isValid());
    QVERIFY(m_captureSourceClient->isValid());
    QVERIFY(m_clientOutput->isValid());
    QVERIFY(m_clientShmPool->isValid());
}

void TestImageCopyCapture::createSession(std::unique_ptr<ImageCopyCaptureClient::CaptureSession> &session)
{
    // Select sole output available as capture source
    auto captureSource = m_captureSourceClient->createSource(*m_clientOutput);
    QVERIFY(captureSource);
    QVERIFY(captureSource->isValid());

    // Create session on capture source
    QSignalSpy sessionCreatedSpy(m_copyCaptureManager.get(), &TestImageCopyCaptureManager::sessionCreated);
    session = m_copyCaptureClient->createSession(*captureSource, false);
    QVERIFY(session);
    QVERIFY(sessionCreatedSpy.wait());
}

void TestImageCopyCapture::testDummyServer()
{
    initClient();

    std::unique_ptr<ImageCopyCaptureClient::CaptureSession> session;
    createSession(session);
    QVERIFY(session);

    // Test constraint gathering
    QSignalSpy constraintsSpy(session.get(), &ImageCopyCaptureClient::CaptureSession::constraintsReady);
    QVERIFY(constraintsSpy.wait());
    QVERIFY(session->constraintsDone());
    QCOMPARE(session->bufferSize(), QSize(800, 600));
    QVERIFY(session->shmFormats().contains(0)); // WL_SHM_FORMAT_ARGB8888

    // Capture multiple frames, to test frame lifecycle management
    for (int i = 0; i < 3; ++i) {
        // Create a frame with a SHM buffer
        auto frame = session->createFrame();
        QVERIFY(frame);
        auto buffer = m_clientShmPool->getBuffer(session->bufferSize(),
                                                 session->bufferSize().width() * 4,
                                                 KWayland::Client::Buffer::Format::ARGB32);
        frame->attachBuffer(*buffer.lock());

        QCOMPARE(m_copyCaptureManager->sessions().size(), 1);

        // Check that the server side can receive a client damage request
        auto *serverSession = m_copyCaptureManager->sessions()[0];
        QVERIFY(serverSession);
        QSignalSpy clientBufferDamageSpy(serverSession, &TestImageCopyCaptureSession::clientBufferDamaged);
        QRect damageRect;
        if (i == 0) {
            damageRect = QRect(QPoint(), session->bufferSize());
        } else {
            int k = (i - 1) % 2;
            auto width = session->bufferSize().width();
            auto height = session->bufferSize().height();
            damageRect = QRect(QPoint(0, k != 0 ? height / 2 : 0), QSize(width, height));
        }
        frame->damageBuffer(damageRect);
        QVERIFY(clientBufferDamageSpy.wait());
        QCOMPARE(serverSession->clientBufferDamage().size(), 1);
        QCOMPARE(serverSession->clientBufferDamage()[0], damageRect);

        // Test frame capture
        QSignalSpy frameSpy(frame.get(), &ImageCopyCaptureClient::CaptureFrame::ready);
        frame->capture();
        QVERIFY(frameSpy.wait());
        QVERIFY(frame->isReady());
        QVERIFY(!frame->hasFailed());
    }
}

void TestImageCopyCapture::testProtocolError()
{
    testProtocolErrorInternal(0);
    testProtocolErrorInternal(1);
    testProtocolErrorInternal(2);
    testProtocolErrorInternal(3);
    testProtocolErrorInternal(4);
    testProtocolErrorInternal(5);
}

void TestImageCopyCapture::testProtocolErrorInternal(int subcase)
{
    initClient();

    // Set up error spy to detect protocol violations
    QSignalSpy errorSpy(m_connection, &KWayland::Client::ConnectionThread::errorOccurred);

    std::unique_ptr<ImageCopyCaptureClient::CaptureSession> session;
    createSession(session);
    QVERIFY(session);

    auto frame1 = session->createFrame();
    QVERIFY(frame1 && frame1->isValid());

    QSignalSpy constraintsSpy(session.get(), &ImageCopyCaptureClient::CaptureSession::constraintsReady);
    QVERIFY(constraintsSpy.wait());

    auto buffer = m_clientShmPool->getBuffer(session->bufferSize(),
                                             session->bufferSize().width() * 4,
                                             KWayland::Client::Buffer::Format::ARGB32);

    switch (subcase) {
    case 0:
        // error_duplicate_frame
        QVERIFY(session->createFrame());
        break;

    case 1:
        // error_no_buffer
        frame1->capture();
        break;

    case 2:
        frame1->attachBuffer(*buffer.lock());

        // error_invalid_buffer_damage
        frame1->damageBuffer(QRect());
        break;

    case 3:
        frame1->attachBuffer(*buffer.lock());
        frame1->capture();

        // error_already_captured
        frame1->capture();
        break;

    case 4:
        frame1->attachBuffer(*buffer.lock());
        frame1->capture();

        // error_already_captured
        frame1->attachBuffer(*buffer.lock());
        break;

    case 5: {
        // error_invalid_option on the manager
        auto source = m_captureSourceClient->createSource(*m_clientOutput);
        QVERIFY(source && source->isValid());
        auto badSession = m_copyCaptureClient->createSession(*source, 0xffu);
        break;
    }
    }

    QVERIFY(errorSpy.wait());
    QCOMPARE(m_connection->errorCode(), EPROTO);
}

QTEST_GUILESS_MAIN(TestImageCopyCapture)
#include "test_image_copy_capture.moc"
