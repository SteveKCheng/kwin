/*
    SPDX-FileCopyrightText: 2026 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "outputrenderer.h"
#include "outputcapturelayer.h"

#include "compositor.h"
#include "core/output.h"
#include "core/outputlayer.h"
#include "core/rendertarget.h"
#include "main.h"
#include "opengl/eglbackend.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/glframebuffer.h"
#include "scene/scene.h"
#include "scene/workspacescene.h"
#include "window.h"
#include "workspace.h"

namespace KWin
{

namespace
{

/**
 * @brief The SceneView used for capturing.
 *
 * Compared to a plain SceneView, this one:
 *   - hides windows that asked not to be captured (e.g. password managers);
 *   - supplies its own presentation timestamp, because there is no real
 *     display frame that the paint is synchronised with.
 */
class CaptureSceneView final : public SceneView
{
public:
    CaptureSceneView(Scene *scene, LogicalOutput *output, OutputLayer *layer)
        : SceneView(scene, output, nullptr, layer)
    {
        addWindowFilter([](Window *window) {
            return window->excludeFromCapture();
        });

        // Toggling excludeFromCapture does not damage the window's area, so force a full repaint.
        auto watchWindow = [this](Window *window) {
            connect(window, &Window::excludeFromCaptureChanged, this, [this]() {
                addDeviceRepaint(Region::infinite());
            });
        };
        connect(workspace(), &Workspace::windowAdded, this, watchWindow);
        const auto windows = workspace()->windows();
        for (Window *window : windows) {
            watchWindow(window);
        }
    }

    void prePaint(OutputFrame *frame = nullptr) override
    {
        m_nextPresentationTimestamp = std::chrono::steady_clock::now().time_since_epoch();
        SceneView::prePaint(frame);
    }

    void setRefreshRate(uint refreshRate)
    {
        m_refreshRate = refreshRate;
    }
};

EglBackend *eglBackend()
{
    return qobject_cast<EglBackend *>(Compositor::self()->backend());
}

} // anonymous namespace

bool OutputRenderer::isSupported()
{
    return eglBackend() != nullptr && kwinApp()->scene() != nullptr;
}

OutputRenderer::OutputRenderer(LogicalOutput *output, bool renderCursor)
    : m_output(output)
{
    Q_ASSERT(isSupported());

    EglBackend *backend = eglBackend();
    m_layer = std::make_unique<OutputCaptureLayer>(output, backend->openglContext()->displayObject()->nonExternalOnlySupportedDrmFormats());

    auto sceneView = std::make_unique<CaptureSceneView>(kwinApp()->scene(), output, m_layer.get());
    sceneView->setRefreshRate(output->refreshRate());
    m_sceneView = std::move(sceneView);
    updateViewport();
    connect(output, &LogicalOutput::changed, this, &OutputRenderer::updateViewport);

    // The cursor is an ordinary item in kwin's scene.  An "exclusive" view over it
    // takes the item away from the scene view, which is how the cursor is hidden
    // from the capture when the client does not want it painted.
    m_cursorView = std::make_unique<ItemTreeView>(m_sceneView.get(), kwinApp()->scene()->cursorItem(), output, nullptr, nullptr);
    m_cursorView->setExclusive(!renderCursor);

    connect(m_layer.get(), &OutputLayer::repaintScheduled, this, &OutputRenderer::contentChanged);
}

OutputRenderer::~OutputRenderer()
{
    // Destroy in reverse order of creation: the cursor view refers to the scene view,
    // and the scene view refers to the layer.
    m_cursorView.reset();
    m_sceneView.reset();
    m_layer.reset();
}

void OutputRenderer::updateViewport()
{
    if (!m_output) {
        return;
    }
    m_sceneView->setViewport(m_output->geometryF());
    m_sceneView->setScale(m_output->scale());
    static_cast<CaptureSceneView *>(m_sceneView.get())->setRefreshRate(m_output->refreshRate());
}

bool OutputRenderer::hasPendingChanges() const
{
    return m_layer->needsRepaint();
}

std::optional<Region> OutputRenderer::render(GLFramebuffer *target, const Region &bufferRepair)
{
    if (!m_output) {
        return std::nullopt;
    }

    const Rect targetRect(QPoint(), target->size());

    // Tell the layer where to paint.  The "repair" region is what must be painted
    // because the buffer itself is stale, independently of scene damage.
    m_layer->setFramebuffer(target, bufferRepair & targetRect);

    // beginFrame hands back the render target and the repair region.
    const auto beginInfo = m_layer->beginFrame();
    if (!beginInfo) {
        return std::nullopt;
    }

    // prePaint walks the item tree and lets effects adjust what is to be painted.
    m_sceneView->prePaint();

    // collectDamage turns the repaints that items queued against this view into a
    // region, minus what is hidden behind opaque windows.  Together with the layer's
    // own accumulated repaints, this is "what changed on screen since we last painted".
    const Region sceneDamage = (m_layer->deviceRepaints() | m_sceneView->collectDamage()) & targetRect;

    // Everything that must be painted: scene damage plus stale parts of the buffer.
    const Region repaint = beginInfo->repaint | sceneDamage;
    m_layer->resetRepaints();

    m_sceneView->paint(beginInfo->renderTarget, QPoint(), repaint);
    m_sceneView->postPaint();

    if (!m_layer->endFrame(repaint, sceneDamage, nullptr)) {
        return std::nullopt;
    }

    // Report everything painted, so a caller that keeps the buffer persistent knows
    // exactly which pixels are new.
    return repaint;
}

} // namespace KWin

#include "moc_outputrenderer.cpp"
