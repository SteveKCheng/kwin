/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "compositor.h"
#include "core/output.h"
#include "opengl/eglbackend.h"
#include "opengl/eglcontext.h"
#include "opengl/glplatform.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"
#include "utils/common.h"
#include <QImage>

namespace KWin
{

static void grabTextureToImage(EglContext &context,
                               GLTexture &texture,
                               QImage &target,
                               const QPoint &topLeft,
                               bool &isInverted)
{
    const bool invertNeeded = (context.isOpenGLES() != (texture.contentTransform() != OutputTransform::FlipY));
    const bool autoInvert = invertNeeded && context.supportsPackInvert();
    isInverted = invertNeeded;
    GLboolean oldPackInversion;
    if (autoInvert) {
        glGetBooleanv(GL_PACK_INVERT_MESA, &oldPackInversion);
        glPixelStorei(GL_PACK_INVERT_MESA, GL_TRUE);
        isInverted = false;
    }

    GLint oldRowLength;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &oldRowLength);
    glPixelStorei(GL_PACK_ROW_LENGTH, target.bytesPerLine() / 4);

    texture.bind();

    if (!context.isOpenGLES() && context.glPlatform()->driver() != Driver_NVidia && texture.size() == target.size() && topLeft.isNull()) {
        // Optimized path for full screen
        context.glGetnTexImage(texture.target(), 0,
                               GL_BGRA, GL_UNSIGNED_BYTE,
                               target.sizeInBytes(), target.bits());
    } else {
        // Bind a Framebuffer Object against the texture, to read the desired region.
        // Unfortunately, we cannot use glGetTextureSubImage since it does not work for
        // multi-sample textures.
        GLFramebuffer fbo(&texture);
        GLFramebuffer::pushFramebuffer(&fbo);

        auto y = isInverted ? target.height() - topLeft.y()
                            : topLeft.y();

        context.glReadnPixels(topLeft.x(), y, target.width(), target.height(),
                              GL_BGRA, GL_UNSIGNED_BYTE,
                              target.sizeInBytes(), target.bits());

        GLFramebuffer::popFramebuffer();
    }

    texture.unbind();

    // Restore GL context parameters
    glPixelStorei(GL_PACK_ROW_LENGTH, oldRowLength);
    if (autoInvert) {
        glPixelStorei(GL_PACK_INVERT_MESA, oldPackInversion);
    }
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
bool renderTextureToImage(GLTexture &texture, QImage &target, const QPoint &topLeft, bool &isInverted)
{
    // When the cursor is moved off the screen, there may be no current EglContext.
    // This might be happening because the "offscreen quick view" effect is cancelling
    // the current EglContext (that is, it calls EglContext::doneCurrent).  Apparently
    // effects plug-ins might be using a EglContext that is different than what kwin's
    // EglBackend normally uses.
    //
    // Since this function can be called in response to a request from an
    // image-copy-capture client, we need the EglContext even if one is not active
    // right now.  Install back the standard context if it is not present.
    auto *context = EglContext::currentContext();
    if (context == nullptr) {
        const auto *backend = qobject_cast<EglBackend *>(Compositor::self()->backend());
        if (backend == nullptr || (context = backend->openglContext()) == nullptr || !context->makeCurrent()) {
            qCritical() << "No OpenGL context available for texture capture";
            return false;
        }
    }

    const OutputTransform contentTransform = texture.contentTransform();
    if (contentTransform == OutputTransform::Normal || contentTransform == OutputTransform::FlipY) {
        grabTextureToImage(*context, texture, target, topLeft, isInverted);
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

        grabTextureToImage(*context, *backingTexture, target, topLeft, isInverted);
    }

    return true;
}

} // namespace KWin
