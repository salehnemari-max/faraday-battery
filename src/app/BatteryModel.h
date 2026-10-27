#pragma once

#include "acquisition/BatteryTypes.h"
#include "acquisition/PowercfgReport.h"
#include "core/HealthVerdict.h"
#include "data/Database.h"
#include "data/Settings.h"

#include <QObject>
#include <QThread>
#include <QVariantList>

#include <memory>

namespace faraday {

class Sampler;

// UI-thread facade over the whole engine: owns the worker-thread sampler,
// the database and the settings store, and exposes everything QML needs.
class BatteryModel : public QObject
{
    Q_OBJECT

    // Availability / status
    Q_PROPERTY(bool ready READ ready NOTIFY snapshotChanged)
    Q_PROPERTY(bool batteryPresent READ batteryPresent NOTIFY snapshotChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY snapshotChanged)
    Q_PROPERTY(bool onAcPower READ onAcPower NOTIFY snapshotChanged)
    Q_PROPERTY(bool charging READ charging NOTIFY snapshotChanged)

    // Live metrics (negative sentinel = unknown)
    Q_PROPERTY(double chargePercent READ chargePercent NOTIFY snapshotChanged)
    Q_PROPERTY(double healthPercent READ healthPercent NOTIFY snapshotChanged)
    Q_PROPERTY(double wearPercent READ wearPercent NOTIFY snapshotChanged)
    Q_PROPERTY(int cycleCount READ cycleCount NOTIFY snapshotChanged)
    Q_PROPERTY(double temperatureC READ temperatureC NOTIFY snapshotChanged)
    Q_PROPERTY(bool temperatureKnown READ temperatureKnown NOTIFY snapshotChanged)
    // True when the reading is a system-zone estimate, not a battery sensor.
    Q_PROPERTY(bool temperatureIsEstimate READ temperatureIsEstimate NOTIFY snapshotChanged)
    Q_PROPERTY(QString temperatureSourceText READ temperatureSourceText NOTIFY snapshotChanged)
    // Temperature ALERTS are only armed against a true battery sensor;
    // firing thresholds against an estimate would be dishonest.
    Q_PROPERTY(bool temperatureAlertAvailable READ temperatureAlertAvailable
                   NOTIFY snapshotChanged)
    Q_PROPERTY(double netPowerW READ netPowerW NOTIFY snapshotChanged)
    Q_PROPERTY(bool powerKnown READ powerKnown NOTIFY snapshotChanged)
    Q_PROPERTY(double voltageV READ voltageV NOTIFY snapshotChanged)
    Q_PROPERTY(qint64 designCapacitymWh READ designCapacitymWh NOTIFY snapshotChanged)
    Q_PROPERTY(qint64 fullChargeCapacitymWh READ fullChargeCapacitymWh NOTIFY snapshotChanged)
    Q_PROPERTY(qint64 remainingCapacitymWh READ remainingCapacitymWh NOTIFY snapshotChanged)
    Q_PROPERTY(QString timeEstimateText READ timeEstimateText NOTIFY snapshotChanged)

    // Verdict
    Q_PROPERTY(QString gradeName READ gradeName NOTIFY snapshotChanged)
    Q_PROPERTY(QString gradeColor READ gradeColor NOTIFY snapshotChanged)
    Q_PROPERTY(QString verdictHeadline READ verdictHeadline NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList verdictDetails READ verdictDetails NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList habitInsights READ habitInsights NOTIFY reportChanged)

    // Detail / diagnostics
    // Per-pack static identity resolved through an explicit source-precedence
    // chain (powercfg wins on design capacity; BatteryStaticData wins on
    // identity fields). Rows: pack, field, value, source, available.
    Q_PROPERTY(QVariantList staticInfo READ staticInfo NOTIFY staticInfoChanged)
    Q_PROPERTY(QVariantList batteries READ batteries NOTIFY snapshotChanged)
    Q_PROPERTY(QVariantList rawStreams READ rawStreams NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList unavailableSources READ unavailableSources NOTIFY snapshotChanged)
    Q_PROPERTY(QVariantList thermalZones READ thermalZones NOTIFY snapshotChanged)

    // Live monitor series
    Q_PROPERTY(QVariantList liveSeries READ liveSeries NOTIFY liveSeriesChanged)
    Q_PROPERTY(double expectedDrainW READ expectedDrainW NOTIFY reportChanged)
    Q_PROPERTY(double expectedDischargeSlopePctPerHour READ expectedDischargeSlopePctPerHour
                   NOTIFY reportChanged)

    // Preferences
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY settingsChanged)
    // UI language code ("en", "ar", ...). Changing it emits languageChanged;
    // main.cpp swaps the translator and retranslates the UI live.
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(int sampleIntervalSec READ sampleIntervalSec WRITE setSampleIntervalSec
                   NOTIFY settingsChanged)
    Q_PROPERTY(bool minimizeToTray READ minimizeToTray NOTIFY settingsChanged)
    Q_PROPERTY(QString temperatureUnit READ temperatureUnit NOTIFY settingsChanged)
    Q_PROPERTY(QString energyUnit READ energyUnit NOTIFY settingsChanged)
    Q_PROPERTY(QString dataDirPath READ dataDirPath NOTIFY settingsChanged)
    Q_PROPERTY(bool autostartEnabled READ autostartEnabled WRITE setAutostartEnabled
                   NOTIFY settingsChanged)
    Q_PROPERTY(bool chargeCapSupported READ chargeCapSupported NOTIFY chargeCapChanged)
    Q_PROPERTY(QString chargeCapDetail READ chargeCapDetail NOTIFY chargeCapChanged)

    // Calibration & reports (Phase 8)
    Q_PROPERTY(QObject *calibration READ calibrationObject CONSTANT)
    Q_PROPERTY(double calibrationDrift READ calibrationDrift NOTIFY snapshotChanged)
    Q_PROPERTY(bool calibrationDriftKnown READ calibrationDriftKnown NOTIFY snapshotChanged)
    Q_PROPERTY(bool calibrationRecommended READ calibrationRecommended NOTIFY snapshotChanged)

public:
    explicit BatteryModel(QObject *parent = nullptr);
    ~BatteryModel() override;

    // Starts the worker thread, database and initial powercfg ingest.
    // dataDir override is for tests; empty means the app-data location.
    Q_INVOKABLE void initialize(const QString &dataDir = QString());

    bool ready() const { return m_ready; }
    bool batteryPresent() const { return m_snapshot.batteryPresent; }
    QString statusText() const;
    bool onAcPower() const;
    bool charging() const;

    double chargePercent() const;
    double healthPercent() const;
    double wearPercent() const;
    int cycleCount() const;
    double temperatureC() const;
    bool temperatureKnown() const;
    bool temperatureIsEstimate() const;
    QString temperatureSourceText() const;
    bool temperatureAlertAvailable() const;
    double netPowerW() const;
    bool powerKnown() const;
    double voltageV() const;
    qint64 designCapacitymWh() const;
    qint64 fullChargeCapacitymWh() const;
    qint64 remainingCapacitymWh() const;
    QString timeEstimateText() const;

    QString gradeName() const;
    QString gradeColor() const;
    QString verdictHeadline() const;
    QStringList verdictDetails() const;
    QStringList habitInsights() const { return m_habitInsights; }

    QVariantList staticInfo() const;
    QVariantList batteries() const;
    QVariantList rawStreams() const;
    QStringList unavailableSources() const { return m_snapshot.unavailable; }
    QVariantList thermalZones() const;

    QVariantList liveSeries() const { return m_liveSeries; }
    double expectedDrainW() const { return m_expectedDrainW; }
    double expectedDischargeSlopePctPerHour() const;

    // Regression over this session's samples matching the current
    // charge/discharge state. Keys: valid, slopePctPerHour, lastT, lastY.
    Q_INVOKABLE QVariantMap liveTrend() const;

    // History & timeline (Phase 6)
    Q_INVOKABLE QVariantList capacityHistoryList() const;
    Q_INVOKABLE QVariantList usageLog(int maxEntries = 200) const;
    Q_INVOKABLE QVariantMap degradationInfo() const;

    QString theme() const;
    void setTheme(const QString &theme);
    QString language() const;
    void setLanguage(const QString &language);
    int sampleIntervalSec() const;
    void setSampleIntervalSec(int seconds);
    bool minimizeToTray() const;
    bool autostartEnabled() const;
    void setAutostartEnabled(bool enabled);
    bool chargeCapSupported() const { return m_chargeCapSupported; }
    QString chargeCapDetail() const { return m_chargeCapDetail; }

    // Generic settings access for the preference pages.
    Q_INVOKABLE QVariant settingValue(const QString &key) const;
    Q_INVOKABLE void setSetting(const QString &key, const QVariant &value);
    Q_INVOKABLE QString tryToggleChargeCap(bool enable);

    QObject *calibrationObject() const;
    double calibrationDrift() const;
    bool calibrationDriftKnown() const;
    bool calibrationRecommended() const;

    // Exports: write into <Documents>/Faraday exports (or exportDir when
    // set for tests). Return the written file path, or empty on failure
    // (details land in lastExportError).
    Q_INVOKABLE QString exportSamplesCsv(int days = 30);
    Q_INVOKABLE QString exportSamplesJson(int days = 30);
    Q_INVOKABLE QString exportHtmlReport();
    Q_INVOKABLE QString lastExportError() const { return m_lastExportError; }
    void setExportDirOverride(const QString &dir) { m_exportDirOverride = dir; }

    Q_INVOKABLE void resetSessionOrigin();
    Q_INVOKABLE void sampleNow();
    Q_INVOKABLE QString formatDuration(qint64 seconds) const;

    // Unit-aware formatting (Phase 9)
    QString temperatureUnit() const;
    QString energyUnit() const;
    QString dataDirPath() const { return m_dataDir; }
    Q_INVOKABLE QString formatTemperature(double celsius) const;
    Q_INVOKABLE QString formatEnergy(double mWh) const;

    // Testing hooks (also used by later phases)
    void applySnapshot(const BatterySnapshot &snapshot);
    void applyReport(const PowercfgReportData &report);
    // The exact input applySnapshot() hands to the alert manager — exposed
    // so the estimate-gating of the temperature alert is unit-testable.
    struct AlertInput currentAlertInput() const;

    // Field defect F3: raw ACPI object names ("BIF0_9", "BAT1") must not be
    // presented as product names. True when the string is an internal
    // identifier rather than human-readable text.
    static bool looksLikeAcpiIdentifier(const QString &name);
    Settings *settings() { return &m_settings; }
    Database *database() { return &m_database; }
    const BatterySnapshot &snapshot() const { return m_snapshot; }

signals:
    void snapshotChanged();
    void staticInfoChanged();
    void reportChanged();
    void liveSeriesChanged();
    void settingsChanged();
    void languageChanged();
    void chargeCapChanged();
    void alertRaised(const QString &title, const QString &message);
    void acquisitionError(const QString &message);

private:
    void startSampler();
    void ingestReportAsync();
    void appendLivePoint(const BatterySnapshot &snapshot);
    const BatteryDevice *primary() const;
    Verdict currentVerdict() const;

    // Source-precedence resolution for one pack (see METRICS.md):
    // keys: name, manufacturer, serialNumber, manufactureDate, chemistry,
    // uniqueId, designCapacitymWh — each paired with <key>Source.
    QVariantMap resolvedStaticFor(int index) const;
    qint64 resolvedDesignForPack(int index) const;

    void probeChargeCapAsync();

    QString exportDir() const;

    QThread m_workerThread;
    Sampler *m_sampler = nullptr;
    class AlertManager *m_alerts = nullptr;
    class Calibration *m_calibration = nullptr;
    bool m_chargeCapSupported = false;
    QString m_chargeCapDetail;
    QString m_lastExportError;
    QString m_exportDirOverride;

    Settings m_settings;
    Database m_database;
    QString m_dataDir;

    BatterySnapshot m_snapshot;
    PowercfgReportData m_report;
    QStringList m_habitInsights;
    double m_expectedDrainW = 0.0;

    QVariantList m_liveSeries;
    QDateTime m_sessionOrigin;
    bool m_ready = false;
    bool m_started = false;
};

} // namespace faraday
