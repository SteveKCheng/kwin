/*
    SPDX-FileCopyrightText: 2025 Steve Cheng <coder@gold-saucer.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#pragma once

#include <QPoint>

class QImage;

namespace KWin
{

class GLTexture;

/**
 * @brief Copy a rectangular portion of a texture into an image buffer, upright.
 *
 * Similar to the function @c grabTexture from src/plugins/screencast/screencastutils.h
 * but allows subsetting to a rectangular portion.  It is an important optimization:
 * for example, when a single glyph is updated on the screen, we do not want to have the
 * CPU be copying megabytes of data --- which would be needed should the whole screen be
 * considered damaged.
 *
 * Also this function allows arbitrary row strides (as long as they are valid) on the
 * image buffer, since in the image-copy-capture protocol that is determined by the client.
 *
 * The result is always upright (row 0 of @a target is the top of the picture), whatever
 * the orientation the texture is stored in.  The texture's content transform must be
 * Normal or FlipY, which is always the case for a texture rendered to by OutputRenderer.
 *
 * Must be called with kwin's OpenGL context current.
 *
 * @param texture The texture to read from.
 * @param target The image (buffer) to copy into, in BGRA byte order
 *               (QImage::Format_ARGB32_Premultiplied).  The size of this image determines
 *               the size of the rectangular region.
 * @param topLeft  The top-left corner of the rectangular region to read, in picture
 *                 coordinates (origin at the top left).  The rectangle specified by
 *                 @a topLeft and the size of @a target must lie entirely inside the texture.
 * @return True if successful, false if not.
 */
bool readTextureRegion(GLTexture &texture, QImage &target, const QPoint &topLeft);

} // namespace KWin
