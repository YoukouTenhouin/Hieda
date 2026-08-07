# Follow the host platform in the Qt Quick UI

Status: accepted

Toolkit neutrality ends at the `NotebookSession` boundary. Hieda's Qt Quick presentation deliberately follows the host desktop: it uses Qt Quick Controls with the runtime-selected platform style, native dialogs, shared standard actions, conventional menus and shortcuts, and system-derived fonts, colors, and metrics. It does not impose a bespoke cross-platform visual skin. Custom visuals are appropriate only when a product capability or accessible interaction cannot be expressed with the standard controls.

This keeps the domain and application behavior portable while allowing the installed application to provide the functionality, familiarity, and accessibility expected on each supported system.

On Linux, Hieda uses `QApplication` and links Qt Widgets even though its main UI is Qt Quick. Desktop platform-theme plugins such as KDE's expose their integrated file-dialog helper through that application path; using only `QGuiApplication` causes `QtQuick.Dialogs.FileDialog` to fall back to Qt's generic Quick implementation on those systems.
