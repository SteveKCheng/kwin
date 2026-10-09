/*
    SPDX-FileCopyrightText: 2025 Xaver Hugl <xaver.hugl@kde.org>
    SPDX-FileCopyrightText: 2026 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "outputcapturelayer.h"

#include "core/output.h"

namespace KWin
{

OutputCaptureLayer::OutputCaptureLayer(LogicalOutput *output, const FormatModifierMap &formats)
    : OutputLayer(output->backendOutput(), OutputLayerType::Primary)
    , m_formats(formats)
{
    // Prevent the layer from scheduling frames on the actual output.
    setRenderLoop(nullptr);
}

void OutputCaptureLayer::setFramebuffer(GLFramebuffer *buffer, const Region &bufferRepair)
{
    m_buffer = buffer;
    m_bufferRepair = bufferRepair;
}

DrmDevice *OutputCaptureLayer::scanoutDevice() const
{
    return nullptr;
}

FormatModifierMap OutputCaptureLayer::supportedDrmFormats() const
{
    return m_formats;
}

std::optional<OutputLayerBeginFrameInfo> OutputCaptureLayer::doBeginFrame()
{
    if (!m_buffer) {
        return std::nullopt;
    }
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_buffer),
        .repaint = m_bufferRepair,
    };
}

bool OutputCaptureLayer::doEndFrame(const Region &renderedRegion, const Region &damagedRegion, OutputFrame *frame)
{
    // Nothing to present: the caller reads the framebuffer directly.
    return true;
}

void OutputCaptureLayer::releaseBuffers()
{
}

} // namespace KWin
