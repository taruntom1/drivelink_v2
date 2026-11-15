#pragma once
// Runtime setup for the DriveLink.Style QML module.
#include <QStringList>

class QQmlEngine;

namespace drivelink::gui {

// Registers the bundled Inter and JetBrains Mono fonts (once per process) and
// the "dlicon" image provider on the engine. Call before loading QML that
// imports DriveLink.Style. Returns false if a bundled font failed to load.
bool installStyle(QQmlEngine &engine);

// The Lucide icon names compiled into the resources (the Figma G2 icon set).
QStringList bundledIconNames();

} // namespace drivelink::gui
