import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window
import Faraday

ApplicationWindow {
    id: root
    width: 1180
    height: 760
    minimumWidth: 760
    minimumHeight: 560
    visible: true
    title: qsTr("Faraday — Battery Intelligence Suite")
    color: Theme.background

    Component.onCompleted: Theme.dark = battery.theme === "dark"
    Connections {
        target: battery
        function onSettingsChanged() { Theme.dark = battery.theme === "dark" }
    }
    Connections {
        target: tray
        function onOpenRequested() {
            root.show()
            root.raise()
            root.requestActivate()
        }
    }

    onClosing: (close) => {
        if (tray.available && battery.minimizeToTray) {
            close.accepted = false
            root.hide()
            tray.notifyMinimized()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        // Right-to-left UI languages (Arabic) mirror the whole layout:
        // the sidebar moves to the right and rows read right-to-left.
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        // ------- Sidebar -------
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 200
            color: Theme.surface

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 4

                RowLayout {
                    Layout.bottomMargin: 18
                    Layout.topMargin: 6
                    spacing: 10
                    // Brand mark. Uses the SIMPLIFIED mark, not the detailed
                    // artwork: at this size the illustration loses its
                    // silhouette. The asset is a dark rounded tile, so it
                    // reads on both the dark and light sidebar.
                    Image {
                        id: brandMark
                        readonly property int slot: 32
                        source: "qrc:/icons/faraday_mark.png"
                        // Decode at device resolution (not at the logical slot
                        // size), so the mark stays crisp on HiDPI displays
                        // instead of being scaled up from a 32px decode.
                        sourceSize.width: slot * Math.max(2, Screen.devicePixelRatio)
                        sourceSize.height: slot * Math.max(2, Screen.devicePixelRatio)
                        Layout.preferredWidth: slot
                        Layout.preferredHeight: slot
                        smooth: true
                        mipmap: true
                        fillMode: Image.PreserveAspectFit
                    }
                    Text {
                        text: qsTr("Faraday")
                        color: Theme.text
                        font.pixelSize: 19
                        font.bold: true
                    }
                }

                Repeater {
                    id: nav
                    model: [
                        { label: qsTr("Dashboard"), icon: "▦" },
                        { label: qsTr("Live monitor"), icon: "∿" },
                        { label: qsTr("History"), icon: "◷" },
                        { label: qsTr("Alerts"), icon: "🔔" },
                        { label: qsTr("Calibration"), icon: "🎯" },
                        { label: qsTr("Settings"), icon: "⚙" }
                    ]

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 40
                        radius: 8
                        color: pages.currentIndex === index
                               ? Qt.alpha(Theme.accent, 0.16)
                               : navMouse.containsMouse ? Qt.alpha(Theme.accent, 0.07) : "transparent"

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            spacing: 10
                            Text {
                                text: modelData.icon
                                color: pages.currentIndex === index ? Theme.accent : Theme.textDim
                                font.pixelSize: 14
                            }
                            Text {
                                text: modelData.label
                                color: pages.currentIndex === index ? Theme.text : Theme.textDim
                                font.pixelSize: 14
                                font.bold: pages.currentIndex === index
                            }
                        }
                        MouseArea {
                            id: navMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: pages.currentIndex = index
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // Theme toggle
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 38
                    radius: 8
                    color: themeMouse.containsMouse ? Qt.alpha(Theme.accent, 0.07) : "transparent"
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        spacing: 10
                        Text { text: Theme.dark ? "🌙" : "☀"; font.pixelSize: 13 }
                        Text {
                            text: Theme.dark ? qsTr("Dark theme") : qsTr("Light theme")
                            color: Theme.textDim
                            font.pixelSize: 13
                        }
                    }
                    MouseArea {
                        id: themeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: battery.theme = Theme.dark ? "light" : "dark"
                    }
                }

                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    text: Qt.application.version
                    color: Theme.textDim
                    font.pixelSize: 10
                }
            }

            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.border
            }
        }

        // ------- Pages -------
        StackLayout {
            id: pages
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: 0

            DashboardPage { }
            MonitorPage { }
            HistoryPage { }
            AlertsPage { }
            CalibrationPage { }
            SettingsPage { }
        }
    }
}
