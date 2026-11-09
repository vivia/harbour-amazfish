import QtQuick 2.0
import uk.co.piggz.amazfish 1.0
import "../components"
import "../components/platform"

PagePL {
    id: page
    title: qsTr("Analysis")
    property alias day: nav.day

    pageMenu: PageMenuPL {
        PageMenuItemPL {
            iconSource: styler.iconDownloadData !== undefined ? styler.iconDownloadData : ""
            text: qsTr("Download All Data")
            onClicked: DaemonInterfaceInstance.fetchData(Amazfish.TYPE_HRV | Amazfish.TYPE_TEMPERATURE | Amazfish.TYPE_ACTIVITY);
        }
    }

    // Place our content in a Column.  The PageHeader is always placed at the top
    // of the page, followed by our content.
    Column {
        id: column
        width: parent.width
        anchors.top: parent.top
        anchors.margins: styler.themePaddingMedium
        spacing: styler.themePaddingLarge

        DateNavigation {
            id: nav
            onBackward: {
                var d = new Date(day);
                d.setDate(day.getDate() - 1);
                day = d;
            }
            onForward: {
                var d = new Date(day);
                d.setDate(day.getDate() + 1);
                day = d;
            }
            onDayChanged: {
                updateGraphs();
            }
        }


        Graph {
            id: graphHeartrate
            graphTitle: qsTr("Heartrate")
            graphHeight: 300

            axisY.units: qsTr("BPM")
            type: DataSource.Heartrate
            visible: supportsFeatureRefresh(Amazfish.TYPE_HEART_RATE)

            minY: 0
            maxY: 200
            valueConverter: function(value) {
                return value.toFixed(0);
            }
            onClicked: {
                updateGraph(day);
            }
        }
        Graph {
            id: graphSteps
            graphTitle: qsTr("Steps")
            graphHeight: 300

            axisY.units: qsTr("Steps")
            type: DataSource.Steps

            minY: 0
            maxY: 200
            valueConverter: function(value) {
                return value.toFixed(0);
            }
            onClicked: {
                updateGraph(day);
            }
        }
        Graph {
            id: graphIntensity
            graphTitle: qsTr("Intensity")
            graphHeight: 300

            axisY.units: "%"
            type: DataSource.Intensity

            minY: 0
            maxY: 100
            valueConverter: function(value) {
                return value.toFixed(0);
            }
            onClicked: {
                updateGraph(day);
            }
        }
        Graph {
            id: graphHRV
            graphTitle: qsTr("HRV")
            graphHeight: 300

            axisY.units: ""
            type: DataSource.HRV

            visible: supportsDataRefresh(Amazfish.TYPE_HRV)

            minY: 0
            maxY: 100

            onClicked: {
                updateGraph(day);
            }
        }

        Graph {
            id: graphVo2max
            graphTitle: qsTr("VO2max")
            graphHeight: 300

            axisX.mask: "MM/dd"
            axisY.units: qsTr("mL/kg/min")
            type: DataSource.Vo2maxSummary
            graphType: line

            // Bands depend on age and gender, so they follow the profile.
            property var bands: AmazfishConfig.vo2maxBands()

            // The axis is pinned to the bands, not to the readings. A scale of
            // its own would put every band boundary off the top of the chart
            // and the colours would have no visible reference.
            minY: 0
            maxY: bands.length === 4 ? Math.ceil(Math.max(suggestedMaxY, bands[0] * 1.1) / 10) * 10
                                     : Math.max(20, Math.ceil(suggestedMaxY / 10) * 10)

            visible: supportsDataRefresh(Amazfish.TYPE_VO2MAX)

            // getColor() stops at the first limit a value is at or under, while
            // the band table gives the lower bound of each band: a value
            // exactly on a bound is coloured with the band below it. The top
            // entry uses maxY, which is above the highest band, so a superior
            // reading does not fall through to defaultColor.
            colorMap: bands.length === 4 ? [
                          {"limit": bands[3], "color": "grey"},
                          {"limit": bands[2], "color": "orange"},
                          {"limit": bands[1], "color": "lightgreen"},
                          {"limit": bands[0], "color": "green"},
                          {"limit": maxY, "color": "darkgreen"},
                      ] : []
            defaultColor: "blue"

            valueConverter: function(value) {
                return value.toFixed(1);
            }
            onClicked: {
                updateGraph(day);
            }
        }

        Graph {
            id: graphBodyTemperature
            graphTitle: qsTr("Body Temperature")
            graphHeight: 300

            axisY.units: "°C"
            axisX.mask: "hh:mm"

            visible: supportsDataRefresh(Amazfish.TYPE_TEMPERATURE)

            type: DataSource.BodyTemperature
            graphType: line

            minY: -20
            maxY: 50

            onClicked: {
                updateGraph(day);
            }
        }

    }

    function updateGraphs() {
        graphHeartrate.updateGraph(day);
        graphSteps.updateGraph(day);
        graphIntensity.updateGraph(day);
        graphHRV.updateGraph(day);
        graphVo2max.updateGraph(day);
        graphBodyTemperature.updateGraph(day);
    }

    Component.onCompleted: {
        day = new Date();
    }
}
