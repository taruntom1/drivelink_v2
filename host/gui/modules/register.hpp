#pragma once
// The custom panels' C++ QML types (H2f). The app and the tests call this
// once before loading the shell; the generic GUI never names a module.
namespace drivelink::gui {
void registerModulePanels();
}
