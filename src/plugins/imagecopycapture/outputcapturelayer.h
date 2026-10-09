/*
    SPDX-FileCopyrightText: 2025 Xaver Hugl <xaver.hugl@kde.org>
    SPDX-FileCopyrightText: 2026 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/outputlayer.h"

namespace KWin
{

/**
 * @brief An output layer that paints into a framebuffer supplied by the caller
 *        instead of a real display plane.
 *
 * In kwin, every SceneView must be attached to an OutputLayer.  The layer is where
 * damage (repaint requests from scene items) accumulates, and it is what the view
 * paints onto.  For a real screen, the layer is a DRM plane with a swapchain.
 * For screen capture we want a "fake" plane whose buffer is simply the
 * framebuffer we choose.
 *
 * This class is a copy of @c ScreencastLayer from kwin's screencast plug-in,
 * which is not exported from libkwin.
 *
 * The render loop is deliberately unset so this layer never schedules frames
 * on the physical output that it is nominally attached to.
 */
class OutputCaptureLayer : public OutputLayer
{
public:
    explicit OutputCaptureLayer(LogicalOutput *output, const FormatModifierMap &formats);

    /**
     * @brief Set the framebuffer that the next frame will be painted into.
     *
     * @param buffer  Framebuffer to paint into.  Must remain valid until the frame ends.
     * @param bufferRepair  The region of @a buffer whose contents are stale and must be
     *                      re-painted regardless of scene damage.  Pass the whole
     *                      framebuffer area for a freshly allocated buffer.
     */
    void setFramebuffer(GLFramebuffer *buffer, const Region &bufferRepair);

    DrmDevice *scanoutDevice() const override;
    FormatModifierMap supportedDrmFormats() const override;
    void releaseBuffers() override;

private:
    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const Region &renderedRegion, const Region &damagedRegion, OutputFrame *frame) override;

    const FormatModifierMap m_formats;
    GLFramebuffer *m_buffer = nullptr;
    Region m_bufferRepair;
};

} // namespace KWin
