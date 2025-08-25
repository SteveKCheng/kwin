/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include <QImage>
#include "opengl/glplatform.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"
#include "opengl/eglcontext.h"

namespace KWin
{

static void grabTextureToImage(GLTexture& texture, QImage& target, const QPoint &topLeft, bool &isInverted)
{
    const auto context = EglContext::currentContext();

    const bool invertNeeded = (context->isOpenGLES() != (texture.contentTransform() != OutputTransform::FlipY));
    const bool autoInvert = invertNeeded && context->supportsPackInvert();
    isInverted = invertNeeded;
    GLboolean prev;
    if (autoInvert) {
        glGetBooleanv(GL_PACK_INVERT_MESA, &prev);
        glPixelStorei(GL_PACK_INVERT_MESA, GL_TRUE);
        isInverted = false;
    }

    GLint oldRowLength;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &oldRowLength);
    glPixelStorei(GL_PACK_ROW_LENGTH,target.bytesPerLine() / 4);

    texture.bind();

    if (!context->isOpenGLES() && context->glPlatform()->driver() != Driver_NVidia &&
        texture.size() == target.size() && topLeft.isNull()) {
        context->glGetnTexImage(texture.target(), 0, GL_BGRA, GL_UNSIGNED_BYTE,
                                target.sizeInBytes(), target.bits());
    } else {
        // Bind a framebuffer to texture, then read the desired region
        // from the framebuffer.
        GLFramebuffer fbo(&texture);
        GLFramebuffer::pushFramebuffer(&fbo);

        auto y = isInverted ? target.height() - topLeft.y() : topLeft.y();

        context->glReadnPixels(topLeft.x(), y, target.width(), target.height(),
                              GL_BGRA, GL_UNSIGNED_BYTE,
                              target.sizeInBytes(), target.bits());

        GLFramebuffer::popFramebuffer();
    }

    // Restore GL context parameters
    glPixelStorei(GL_PACK_ROW_LENGTH, oldRowLength);
    if (autoInvert) {
        glPixelStorei(GL_PACK_INVERT_MESA, prev);
    }

    texture.unbind();
}

/**
 * @brief Render a rectangular portion of a texture onto an image buffer.
 *
 * Similar to the function \c grabTexture from src/plugins/screencast/screencastutils.hpp
 * but allows subsetting to a rectangular portion.  It is an important optimization:
 * for example, when a single glyph is updated on the screen, we do not want to have the
 * CPU be copying megabytes of data --- which would be needed should the whole screen be
 * considered damaged.
 *
 * Also this function allows arbitrary row strides (as long as they are valid) on the
 * image buffer, since in the image-copy-capture protocol that is determined by the client.
 *
 * @param texture The texture (representing the output) to render.
 *                Transformations on the texture are applied.
 * @param target The image (buffer) to render to.  The size of this image determines
 *               the size of the rectangular region.
 * @param topLeft  The top-left corner of the rectangular region to render.
 *                 The rectangle specified by @a topLeft and the size of @a target
 *                 must lie entirely inside the texture.
 * @param isInverted  On return, set to true if the image is vertically flipped
 *                    in the buffer.  False otherwise.
 * @return True if rendering is successful, false if not.
 */
bool renderTextureToImage(GLTexture& texture, QImage& target, const QPoint &topLeft, bool &isInverted)
{
    const OutputTransform contentTransform = texture.contentTransform();
    if (contentTransform == OutputTransform::Normal || contentTransform == OutputTransform::FlipY) {
        grabTextureToImage(texture, target, topLeft, isInverted);
    } else {
        const QSize contentSize = contentTransform.map(texture.size());
        const auto backingTexture = GLTexture::allocate(GL_RGBA8, contentSize);
        if (!backingTexture) {
            return false;
        }
        backingTexture->setContentTransform(OutputTransform::FlipY);

        ShaderBinder shaderBinder(ShaderTrait::MapTexture);
        QMatrix4x4 projectionMatrix;
        projectionMatrix.scale(1, -1);
        projectionMatrix.ortho(QRect(QPoint(), contentSize));
        shaderBinder.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projectionMatrix);

        GLFramebuffer fbo(backingTexture.get());
        GLFramebuffer::pushFramebuffer(&fbo);
        texture.render(contentSize);
        GLFramebuffer::popFramebuffer();

        grabTextureToImage(*backingTexture, target, topLeft, isInverted);
    }

    return true;
}

} // namespace KWin
