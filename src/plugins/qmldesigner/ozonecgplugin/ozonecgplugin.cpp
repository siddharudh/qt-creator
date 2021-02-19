/* ozonecgplugin.cpp
 *
 * Copyright (C) 2021 Siddharudh P T <siddharudh@gmail.com>
 *
 * This file is part of OzoneCG Project.
 *
 * OzoneCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OzoneCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OzoneCG.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ozonecgplugin.h"
#include "assetsview/o3assetsview.h"
#include "assetspreview/o3assetspreview.h"

#include <qmldesignerplugin.h>
#include <designeractionmanager.h>
#include <modelnodecontextmenu_helper.h>
#include <viewmanager.h>

using namespace QmlDesigner;

namespace OzoneCG {
namespace Designer {

OzoneCGPlugin::OzoneCGPlugin()
    : m_assetsView(new AssetsView),
      m_assetsPreview(new AssetsPreview)
{
    ViewManager &viewManager = QmlDesignerPlugin::instance()->viewManager();
    viewManager.registerViewTakingOwnership(m_assetsView);
    viewManager.registerViewTakingOwnership(m_assetsPreview);

    connect(m_assetsView, &AssetsView::assetSelected,
            m_assetsPreview, &AssetsPreview::requestPreview);
}

QString OzoneCGPlugin::metaInfo() const
{
    return QLatin1String(":/ozonecgplugin/ozonecgplugin.metainfo");
}

QString OzoneCGPlugin::pluginName() const
{
    return QLatin1String("OzoneCGPlugin");
}

} // namespace Designer
} // namespace OzoneCG
