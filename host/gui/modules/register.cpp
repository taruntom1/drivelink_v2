#include "register.hpp"
#include "imu.bno055/calibration_qml.hpp"
#include <QQmlEngine>

namespace drivelink::gui {
void registerModulePanels() {
    static const bool registered=[] {
        // imu.bno055 (and imu.mock): one instance per engine, owned by it.
        qmlRegisterSingletonType<imu_calibration::ImuCalibration>("DriveLink.Modules.ImuBno055",1,0,"ImuCalibration",
            [](QQmlEngine *,QJSEngine *)->QObject * {return new imu_calibration::ImuCalibration;});
        return true;
    }();
    (void)registered;
}
}
