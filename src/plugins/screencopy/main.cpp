/*
    SPDX-FileCopyrightText: 2024 Steve <steve@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "screencopymanager.h"

#include <KPluginFactory>

using namespace KWin;

class KWIN_EXPORT ScreencopyManagerFactory : public PluginFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PluginFactory_iid FILE "metadata.json")
    Q_INTERFACES(KWin::PluginFactory)

public:
    explicit ScreencopyManagerFactory() = default;

    std::unique_ptr<Plugin> create() const override;
};

std::unique_ptr<Plugin> ScreencopyManagerFactory::create() const
{
    return std::make_unique<ScreencopyManager>();
}

#include "main.moc"
