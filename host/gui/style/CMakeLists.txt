# QML module DriveLink.Style: the generated Theme singleton, the DL* controls,
# the bundled fonts and Lucide icons. Static, linked into every GUI executable.
set(DRIVELINK_ICONS
  activity bell cable chart-line check chevron-down chevron-left chevron-right
  chevron-up circle-alert circle-check circle-x compass copy cpu crosshair
  download ellipsis external-link eye eye-off file flask-conical folder-open
  gauge grip-vertical hand history info key-round link-2-off list loader-circle
  lock lock-open minus network octagon-x pause pencil play plus power radio
  refresh-cw rotate-ccw save scan-line search server settings shield-alert
  sliders-horizontal square timer trash-2 triangle-alert undo-2 unplug upload
  usb users wand-sparkles wifi-off workflow x zap)
set(DRIVELINK_FONTS
  Inter-Regular.ttf Inter-Medium.ttf Inter-SemiBold.ttf
  JetBrainsMono-Regular.ttf JetBrainsMono-Medium.ttf)

set(resources ${CMAKE_CURRENT_SOURCE_DIR}/../resources)
list(TRANSFORM DRIVELINK_ICONS PREPEND ${resources}/icons/ OUTPUT_VARIABLE icon_files)
list(TRANSFORM icon_files APPEND .svg)
list(TRANSFORM DRIVELINK_FONTS PREPEND ${resources}/fonts/ OUTPUT_VARIABLE font_files)
list(JOIN DRIVELINK_ICONS "," icon_list)

set(theme ${CMAKE_CURRENT_SOURCE_DIR}/../theme/Theme.qml)
set_source_files_properties(${theme} PROPERTIES QT_QML_SINGLETON_TYPE TRUE QT_RESOURCE_ALIAS Theme.qml)

qt_add_library(drivelink_gui_style STATIC style.cpp)
target_include_directories(drivelink_gui_style PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(drivelink_gui_style PRIVATE DRIVELINK_ICONS="${icon_list}")
target_link_libraries(drivelink_gui_style PUBLIC Qt6::Gui Qt6::Qml Qt6::Quick PRIVATE Qt6::Svg Qt6::QuickTemplates2)

qt_add_qml_module(drivelink_gui_style
  URI DriveLink.Style
  VERSION 1.0
  NO_PLUGIN
  QML_FILES
    ${theme}
    DLIcon.qml
    DLShadow.qml
    DLButton.qml
    DLIconButton.qml
    DLArmButton.qml
    DLStateChip.qml
    DLBadge.qml
    DLDeviceChip.qml
    DLNavItem.qml
    DLNavRail.qml
    DLStatusBar.qml
    DLBanner.qml
    DLDialog.qml
    DLToast.qml
    DLToolTip.qml
    DLReasonHover.qml
    DLTextField.qml
    DLComboBox.qml
    DLCheckItem.qml
    DLProgressBar.qml
)

qt_add_resources(drivelink_gui_style drivelink_gui_assets
  PREFIX /DriveLink
  BASE ${resources}
  FILES ${icon_files} ${font_files})
