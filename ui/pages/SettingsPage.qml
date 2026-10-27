import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Faraday

Item {
    id: page

    component SettingRow: RowLayout {
        property string label: ""
        property string hint: ""
        Layout.fillWidth: true
        spacing: 14
        ColumnLayout {
            Layout.preferredWidth: 280
            spacing: 0
            Text { text: label; color: Theme.text; font.pixelSize: 13 }
            Text {
                visible: hint.length > 0
                text: hint
                color: Theme.textDim
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 280
            }
        }
    }

    component Section: Rectangle {
        default property alias content: body.data
        property string heading: ""
        Layout.fillWidth: true
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
        radius: Theme.radius
        implicitHeight: body.implicitHeight + 46

        Text {
            id: head
            anchors { top: parent.top; left: parent.left; margins: 14 }
            text: parent.heading
            color: Theme.textDim
            font.pixelSize: 11
            font.letterSpacing: 1.2
        }
        ColumnLayout {
            id: body
            anchors { top: head.bottom; left: parent.left; right: parent.right; margins: 14; topMargin: 8 }
            spacing: 10
        }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.height + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        ColumnLayout {
            id: column
            x: 20
            y: 16
            width: parent.width - 40
            spacing: Theme.spacing

            Text {
                text: qsTr("Settings")
                color: Theme.text
                font.pixelSize: 20
                font.bold: true
            }

            Section {
                heading: qsTr("APPEARANCE")
                SettingRow {
                    label: qsTr("Theme")
                    ComboBox {
                        model: [qsTr("Dark"), qsTr("Light")]
                        currentIndex: battery.theme === "dark" ? 0 : 1
                        onActivated: battery.theme = currentIndex === 0 ? "dark" : "light"
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            Section {
                heading: qsTr("UNITS")
                SettingRow {
                    label: qsTr("Temperature")
                    ComboBox {
                        model: ["°C", "°F"]
                        currentIndex: battery.temperatureUnit === "F" ? 1 : 0
                        onActivated: battery.setSetting("temperatureUnit",
                                                        currentIndex === 1 ? "F" : "C")
                    }
                    Item { Layout.fillWidth: true }
                }
                SettingRow {
                    label: qsTr("Energy")
                    ComboBox {
                        model: ["mWh", "Wh"]
                        currentIndex: battery.energyUnit === "Wh" ? 1 : 0
                        onActivated: battery.setSetting("energyUnit",
                                                        currentIndex === 1 ? "Wh" : "mWh")
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            Section {
                heading: qsTr("SAMPLING")
                SettingRow {
                    label: qsTr("Sample interval")
                    hint: qsTr("How often live sensors are read (1–3600 s). Longer intervals use less power.")
                    SpinBox {
                        from: 1; to: 3600
                        value: battery.sampleIntervalSec
                        editable: true
                        onValueModified: battery.sampleIntervalSec = value
                    }
                    Text { text: qsTr("seconds"); color: Theme.textDim; font.pixelSize: 12 }
                    Item { Layout.fillWidth: true }
                }
                SettingRow {
                    label: qsTr("Thresholds & reminders")
                    hint: qsTr("Alert levels are configured on the Alerts page.")
                    Item { Layout.fillWidth: true }
                }
            }

            Section {
                heading: qsTr("LANGUAGE")
                SettingRow {
                    label: qsTr("Language")
                    hint: qsTr("Applies immediately. Arabic switches the whole interface to right-to-left.")
                    ComboBox {
                        // Language names stay in their own script so anyone
                        // can find theirs, whatever the current UI language.
                        readonly property var codes: ["en", "ar"]
                        model: ["English", "العربية"]
                        currentIndex: Math.max(0, codes.indexOf(battery.language))
                        onActivated: (index) => battery.language = codes[index]
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            Section {
                heading: qsTr("DATA")
                SettingRow {
                    label: qsTr("Data folder")
                    hint: qsTr("Settings (JSON) and the sample database live here. Faraday never writes to the registry.")
                    Text {
                        Layout.fillWidth: true
                        text: battery.dataDirPath
                        color: Theme.accent
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }
                    Button {
                        text: qsTr("Open")
                        onClicked: Qt.openUrlExternally("file:///" + battery.dataDirPath.replace(/\\/g, "/"))
                    }
                }
            }
        }
    }
}
