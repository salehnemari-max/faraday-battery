import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Faraday

Item {
    id: page

    property string exportMessage: ""
    property string exportPath: ""

    function reportExport(path) {
        if (path.length > 0) {
            exportPath = path
            exportMessage = qsTr("Saved to %1").arg(path)
        } else {
            exportPath = ""
            exportMessage = qsTr("Export failed: %1").arg(battery.lastExportError())
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
                text: qsTr("Calibration & reports")
                color: Theme.text
                font.pixelSize: 20
                font.bold: true
            }

            // ---- Drift status ----
            Rectangle {
                Layout.fillWidth: true
                color: Theme.surface
                border.color: Theme.border
                border.width: 1
                radius: Theme.radius
                implicitHeight: driftCol.implicitHeight + 30

                ColumnLayout {
                    id: driftCol
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 15 }
                    spacing: 6

                    Text {
                        text: qsTr("GAUGE DRIFT")
                        color: Theme.textDim
                        font.pixelSize: 11
                        font.letterSpacing: 1.2
                    }
                    RowLayout {
                        spacing: 14
                        Text {
                            text: battery.calibrationDriftKnown
                                  ? (battery.calibrationDrift >= 0 ? "+" : "")
                                    + battery.calibrationDrift.toFixed(1) + " %"
                                  : "—"
                            color: battery.calibrationRecommended ? Theme.warn : Theme.good
                            font.pixelSize: 26
                            font.bold: true
                        }
                        Text {
                            Layout.fillWidth: true
                            text: battery.calibrationDriftKnown
                                  ? (battery.calibrationRecommended
                                     ? qsTr("The Windows gauge and the measured energy disagree noticeably — running the conditioning cycle below is recommended.")
                                     : qsTr("The reported percentage matches the measured energy well. No calibration needed."))
                                  : qsTr("Drift cannot be evaluated on this system (per-mWh data unavailable).")
                            color: Theme.textDim
                            font.pixelSize: 13
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // ---- Guided conditioning ----
            Rectangle {
                Layout.fillWidth: true
                color: Theme.surface
                border.color: Theme.border
                border.width: 1
                radius: Theme.radius
                implicitHeight: calCol.implicitHeight + 30

                ColumnLayout {
                    id: calCol
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 15 }
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("GUIDED CONDITIONING")
                            color: Theme.textDim
                            font.pixelSize: 11
                            font.letterSpacing: 1.2
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: battery.calibration.active ? qsTr("Cancel") : qsTr("Start")
                            enabled: battery.batteryPresent
                            onClicked: battery.calibration.active
                                       ? battery.calibration.cancel()
                                       : battery.calibration.start()
                        }
                    }

                    // Step indicator
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: [
                                qsTr("Charge to full"),
                                qsTr("Rest 30 min"),
                                qsTr("Discharge to 10%"),
                                qsTr("Recharge")
                            ]
                            RowLayout {
                                spacing: 6
                                Rectangle {
                                    width: 26; height: 26; radius: 13
                                    color: battery.calibration.step > index + 1
                                           || battery.calibration.step === 5
                                           ? Theme.good
                                           : battery.calibration.step === index + 1
                                             ? Theme.accent : Theme.chartGrid
                                    Text {
                                        anchors.centerIn: parent
                                        text: battery.calibration.step > index + 1
                                              || battery.calibration.step === 5 ? "✓" : (index + 1)
                                        color: "#ffffff"
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                }
                                Text {
                                    text: modelData
                                    color: battery.calibration.step === index + 1
                                           ? Theme.text : Theme.textDim
                                    font.pixelSize: 12
                                    font.bold: battery.calibration.step === index + 1
                                }
                                Rectangle {
                                    visible: index < 3
                                    Layout.preferredWidth: 18
                                    height: 2
                                    color: Theme.chartGrid
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: battery.calibration.stepName + " — " + battery.calibration.instruction
                        color: Theme.text
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ---- Exports ----
            Rectangle {
                Layout.fillWidth: true
                color: Theme.surface
                border.color: Theme.border
                border.width: 1
                radius: Theme.radius
                implicitHeight: exportCol.implicitHeight + 30

                ColumnLayout {
                    id: exportCol
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 15 }
                    spacing: 10

                    Text {
                        // The path stays outside qsTr: lupdate drops the escaped backslash,
                        // which would make the extracted key differ from the runtime one.
                        text: qsTr("EXPORTS — written to %1").arg("Documents\\Faraday exports")
                        color: Theme.textDim
                        font.pixelSize: 11
                        font.letterSpacing: 1.2
                    }
                    RowLayout {
                        spacing: 10
                        Button {
                            text: qsTr("Samples → CSV")
                            onClicked: page.reportExport(battery.exportSamplesCsv(30))
                        }
                        Button {
                            text: qsTr("Samples → JSON")
                            onClicked: page.reportExport(battery.exportSamplesJson(30))
                        }
                        Button {
                            text: qsTr("HTML health report")
                            onClicked: page.reportExport(battery.exportHtmlReport())
                        }
                        Button {
                            visible: page.exportPath.length > 0
                            text: qsTr("Open")
                            onClicked: Qt.openUrlExternally("file:///" + page.exportPath.replace(/\\/g, "/"))
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: page.exportMessage.length > 0
                        text: page.exportMessage
                        color: page.exportPath.length > 0 ? Theme.good : Theme.bad
                        font.pixelSize: 12
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }
        }
    }
}
