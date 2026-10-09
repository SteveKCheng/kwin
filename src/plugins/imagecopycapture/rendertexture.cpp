/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "rendertexture.h"

#include "opengl/eglcontext.h"
#include "opengl/glframebuffer.h"
#include "opengl/glplatform.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"

#include <QImage>

#include <cstring>
#include <vector>

namespace KWin
{

/// Swap rows top-for-bottom in place.
static void mirrorRowsVertically(uchar *data, int height, int stride, int rowBytes)
{
    std::vector<uchar> temp(rowBytes);
    for (int y = 0; y < height / 2; ++y) {
        uchar *top = data + y * stride;
        uchar *bottom = data + (height - 1 - y) * stride;
        std::memcpy(temp.data(), top, rowBytes);
        std::memcpy(top, bottom, rowBytes);
        std::memcpy(bottom, temp.data(), rowBytes);
    }
}

bool readTextureRegion(GLTexture &texture, QImage &target, const QPoint &topLeft)
{
    EglContext *context = EglContext::currentContext();
    if (!context) {
        qCritical() << "No OpenGL context available for texture read-back";
        return false;
    }

    const QSize textureSize = texture.size();
    const QRect region(topLeft, target.size());
    if (!QRect(QPoint(), textureSize).contains(region) || region.isEmpty()) {
        qWarning() << "Texture read-back region" << region << "is not inside the texture" << textureSize;
        return false;
    }
    if (texture.contentTransform() != OutputTransform::Normal && texture.contentTransform() != OutputTransform::FlipY) {
        qWarning() << "Texture read-back does not support rotated textures";
        return false;
    }

    // OpenGL stores a texture with row 0 at the *bottom*, and glReadPixels/glGetTexImage
    // write row 0 first into memory.  Whether the result comes out upside down depends on
    // how kwin painted the texture.  The formula below is the same one kwin's own
    // screencast code uses (see doGrabTexture in screencastutils.h).
    const bool invertNeeded = context->isOpenGLES() != (texture.contentTransform() != OutputTransform::FlipY);

    // Mesa can invert the rows for us as it packs them, which costs nothing.
    // Otherwise (e.g. NVIDIA) we have to mirror the rows on the CPU afterwards.
    const bool autoInvert = invertNeeded && context->supportsPackInvert();

    GLint oldRowLength;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &oldRowLength);
    glPixelStorei(GL_PACK_ROW_LENGTH, target.bytesPerLine() / 4);

    GLboolean oldPackInvert = GL_FALSE;
    if (autoInvert) {
        glGetBooleanv(GL_PACK_INVERT_MESA, &oldPackInvert);
        glPixelStorei(GL_PACK_INVERT_MESA, GL_TRUE);
    }

    texture.bind();

    const bool wholeTexture = topLeft.isNull() && target.size() == textureSize;
    if (wholeTexture && !context->isOpenGLES() && !context->glPlatform()->isNvidia()) {
        // Fast path: read the entire texture directly.
        // (The NVIDIA driver fails glGetTexImage, as noted in kwin's screencast code.)
        context->glGetnTexImage(texture.target(), 0, GL_BGRA, GL_UNSIGNED_BYTE, target.sizeInBytes(), target.bits());
    } else {
        // Bind a framebuffer object against the texture to read just the desired region.
        // (glGetTextureSubImage is not usable here: it does not work for all texture types.)
        GLFramebuffer fbo(&texture);
        GLFramebuffer::pushFramebuffer(&fbo);

        // When the texture is stored upside down relative to the picture, the picture rows
        // [top, top + height) live at GL rows [H - top - height, H - top).
        const int glY = invertNeeded ? textureSize.height() - topLeft.y() - target.height()
                                     : topLeft.y();
        context->glReadnPixels(topLeft.x(), glY, target.width(), target.height(),
                               GL_BGRA, GL_UNSIGNED_BYTE,
                               target.sizeInBytes(), target.bits());

        GLFramebuffer::popFramebuffer();
    }

    texture.unbind();

    // Restore GL pixel-store state
    glPixelStorei(GL_PACK_ROW_LENGTH, oldRowLength);
    if (autoInvert) {
        glPixelStorei(GL_PACK_INVERT_MESA, oldPackInvert);
    }

    if (invertNeeded && !autoInvert) {
        mirrorRowsVertically(target.bits(), target.height(), target.bytesPerLine(), target.width() * 4);
    }

    return true;
}

} // namespace KWin
