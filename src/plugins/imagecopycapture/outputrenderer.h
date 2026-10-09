/*
    SPDX-FileCopyrightText: 2026 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#pragma once

#include "core/region.h"

#include <QObject>
#include <QPointer>

#include <memory>
#include <optional>

namespace KWin
{

class GLFramebuffer;
class ItemTreeView;
class LogicalOutput;
class OutputCaptureLayer;
class SceneView;

/**
 * @brief Renders the contents of one output into a framebuffer of the caller's choosing,
 *        with damage tracking.
 *
 * Since Plasma 6.5 kwin no longer keeps a texture holding the contents of each output
 * (the display may be assembled from several hardware planes), so a capture has to
 * re-render the scene itself.  This class owns the pieces needed to do that:
 *
 *   - an OutputCaptureLayer, the "fake plane" that scene damage accumulates on;
 *   - a SceneView, the camera onto kwin's scene that paints onto that layer;
 *   - an ItemTreeView over the cursor item, used to include or exclude the cursor.
 *
 * The scene marks the view dirty whenever something visible on the output changes;
 * this is reported through #contentChanged.  The caller then calls #render with a
 * framebuffer.  The framebuffer can be backed by anything OpenGL can draw into, which
 * allows a client-supplied DMA-BUF to be rendered into directly in the future.
 *
 * All methods must be called with kwin's OpenGL context current.
 */
class OutputRenderer : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Whether the compositor is in a state where rendering is possible
     *        (OpenGL compositing is active).
     */
    static bool isSupported();

    /**
     * @param output  The output to render.  The renderer must be destroyed before the output is removed.
     * @param renderCursor  Whether the mouse cursor is painted into the rendered image.
     */
    explicit OutputRenderer(LogicalOutput *output, bool renderCursor);
    ~OutputRenderer() override;

    /**
     * @brief Whether the scene has reported changes since the last #render call.
     */
    bool hasPendingChanges() const;

    /**
     * @brief Render the output's contents into @a target.
     *
     * Only the parts of @a target covered by scene damage and @a bufferRepair are painted;
     * everything else is left as is.  Hence @a target should normally be persistent
     * between calls.
     *
     * @param target  Framebuffer to paint into.  Its size should match the output's pixel size.
     * @param bufferRepair  Region of @a target whose contents are stale and must be painted
     *                      regardless of scene damage, in device pixels.  Pass
     *                      Region::infinite() for a freshly allocated buffer.
     * @return The region of @a target that was painted, in device pixels, or nullopt
     *         if rendering failed.  The region may be empty if nothing visible changed.
     */
    std::optional<Region> render(GLFramebuffer *target, const Region &bufferRepair);

Q_SIGNALS:
    /**
     * @brief Emitted when something visible on the output has changed, i.e. a new frame
     *        should be rendered when the client is ready for one.
     *
     * May be emitted many times in a row; it is cheap to ignore.
     */
    void contentChanged();

private:
    void updateViewport();

    QPointer<LogicalOutput> m_output;
    std::unique_ptr<OutputCaptureLayer> m_layer;
    std::unique_ptr<SceneView> m_sceneView;
    std::unique_ptr<ItemTreeView> m_cursorView;
};

} // namespace KWin
