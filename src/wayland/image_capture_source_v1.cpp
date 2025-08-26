/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "image_capture_source_v1.h"
#include "display.h"
#include "output.h"
#include "wayland/output.h"

#include <QPointer>

#include "qwayland-server-ext-image-capture-source-v1.h"
#include "utils/resource.h"

namespace KWin
{

static const int s_version = 1;

class ImageCaptureSourceV1InterfacePrivate final : public QtWaylandServer::ext_image_capture_source_v1
{
public:
    ImageCaptureSourceV1InterfacePrivate(ImageCaptureSourceV1Interface *q)
        : m_parent(q)
    {
    }

    ImageCaptureSourceV1Interface * const m_parent;

    ImageCaptureSourceV1Interface::SourceType m_sourceType;
    QPointer<Output> m_output; // Only valid for output sources

protected:
    void ext_image_capture_source_v1_destroy_resource(Resource *resource) override
    {
        delete m_parent;
    }

    void ext_image_capture_source_v1_destroy(Resource *resource) override
    {
        wl_resource_destroy(resource->handle);
    }
};

class OutputImageCaptureSourceManagerV1InterfacePrivate final : public QtWaylandServer::ext_output_image_capture_source_manager_v1
{
public:
    OutputImageCaptureSourceManagerV1InterfacePrivate(OutputImageCaptureSourceManagerV1Interface *q, Display *display)
        : QtWaylandServer::ext_output_image_capture_source_manager_v1(*display, s_version)
        , m_parent(q)
    {
    }

private:
    OutputImageCaptureSourceManagerV1Interface * const m_parent;

protected:
    void ext_output_image_capture_source_manager_v1_destroy_resource(Resource *resource) override
    {
        // Manager can be destroyed, sources remain valid
    }

    void ext_output_image_capture_source_manager_v1_destroy(Resource *resource) override
    {
        wl_resource_destroy(resource->handle);
    }

    void ext_output_image_capture_source_manager_v1_create_source(Resource *resource,
                                                                 uint32_t source_id,
                                                                 struct ::wl_resource *output_resource) override
    {
        // Get the OutputInterface from the wl_resource
        OutputInterface *outputInterface = OutputInterface::get(output_resource);
        if (!outputInterface || !outputInterface->handle()) {
            wl_resource_post_error(resource->handle, WL_DISPLAY_ERROR_INVALID_OBJECT, "invalid output");
            return;
        }

        wl_resource *sourceResource = wl_resource_create(resource->client(), &ext_image_capture_source_v1_interface, resource->version(), source_id);
        if (!sourceResource) {
            wl_resource_post_no_memory(resource->handle);
            return;
        }

        // Create the source object
        auto* inst = new ImageCaptureSourceV1Interface(m_parent);
        inst->d->m_sourceType = ImageCaptureSourceV1Interface::SourceType::Output;
        inst->d->m_output = outputInterface->handle();
        inst->d->init(sourceResource);
    }
};

//
// Implementation of ImageCaptureSourceV1Interface
//

ImageCaptureSourceV1Interface::ImageCaptureSourceV1Interface(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<ImageCaptureSourceV1InterfacePrivate>(this))
{
}

ImageCaptureSourceV1Interface::~ImageCaptureSourceV1Interface() = default;

ImageCaptureSourceV1Interface* ImageCaptureSourceV1Interface::get(struct wl_resource *resource)
{
    auto* priv = resource_cast<ImageCaptureSourceV1InterfacePrivate *>(resource);
    return priv ? priv->m_parent : nullptr;
}

ImageCaptureSourceV1Interface::SourceType ImageCaptureSourceV1Interface::sourceType() const
{
    return d->m_sourceType;
}

Output* ImageCaptureSourceV1Interface::output() const
{
    return d->m_output.get();
}

//
// Implementation of OutputImageCaptureSourceManagerV1Interface
//

OutputImageCaptureSourceManagerV1Interface::OutputImageCaptureSourceManagerV1Interface(Display *display, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<OutputImageCaptureSourceManagerV1InterfacePrivate>(this, display))
{
}

OutputImageCaptureSourceManagerV1Interface::~OutputImageCaptureSourceManagerV1Interface() = default;

} // namespace KWin
