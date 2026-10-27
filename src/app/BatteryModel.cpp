#include "BatteryModel.h"

#include "app/AlertManager.h"
#include "app/Autostart.h"
#include "app/Calibration.h"
#include "app/ChargeCap.h"
#include "app/Exporter.h"
#include "app/Sampler.h"
#include "core/Metrics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>

namespace faraday {

namespace {

QVariant optToVariant(const std::optional<quint32> &v)
{
    return v.has_value() ? QVariant(static_cast<qint64>(*v)) : QVariant();
}

QVariant optToVariant(const std::optional<qint32> &v)
{
    return v.has_value() ? QVariant(static_cast<qint64>(*v)) : QVariant();
}

QVariant optToVariant(const std::optional<bool> &v)
{
    return v.has_value() ? QVariant(*v) : QVariant();
}

const int kMaxLivePoints = 7200;

} // namespace

BatteryModel::BatteryModel(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<BatterySnapshot>();
    m_calibration = new Calibration(this);
}

BatteryModel::~BatteryModel()
{
    if (m_started) {
        m_workerThread.quit();
        m_workerThread.wait(5000);
    }
}

void BatteryModel::initialize(const QString &dataDir)
{
    if (m_started)
        return;
    m_started = true;

    m_dataDir = dataDir;
    if (m_dataDir.isEmpty())
        m_dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(m_dataDir);

    m_settings = Settings(m_dataDir);
    m_settings.load(); // falls back to defaults if missing/corrupt
    // First run: persist the defaults immediately so the data directory is
    // complete from the start (matters for the portable ZIP, where the
    // folder itself is the whole footprint).
    if (!QFileInfo::exists(QDir(m_dataDir).filePath(QStringLiteral("settings.json"))))
        m_settings.save();

    if (m_database.open(QDir(m_dataDir).filePath(QStringLiteral("faraday.sqlite"))))
        m_database.initSchema();

    m_sessionOrigin = QDateTime::currentDateTime();
    m_alerts = new AlertManager(&m_settings, this);
    connect(m_alerts, &AlertManager::alertRaised, this, &BatteryModel::alertRaised);

    startSampler();
    ingestReportAsync();
    probeChargeCapAsync();
    emit settingsChanged();
}

void BatteryModel::probeChargeCapAsync()
{
    QPointer<BatteryModel> guard(this);
    QThreadPool::globalInstance()->start([guard]() {
        const ChargeCapInfo info = ChargeCap::probe();
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, info]() {
                if (!guard)
                    return;
                guard->m_chargeCapSupported = info.supported;
                guard->m_chargeCapDetail = info.detail;
                emit guard->chargeCapChanged();
            }, Qt::QueuedConnection);
        }
    });
}

void BatteryModel::startSampler()
{
    m_sampler = new Sampler();
    m_sampler->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_sampler, &QObject::deleteLater);
    connect(m_sampler, &Sampler::snapshotReady, this, &BatteryModel::applySnapshot,
            Qt::QueuedConnection);
    connect(m_sampler, &Sampler::acquisitionError, this, &BatteryModel::acquisitionError,
            Qt::QueuedConnection);
    m_workerThread.setObjectName(QStringLiteral("faraday-sampler"));
    m_workerThread.start();
    QMetaObject::invokeMethod(m_sampler, "start", Qt::QueuedConnection,
                              Q_ARG(int, m_settings.sampleIntervalSec()));
}

void BatteryModel::ingestReportAsync()
{
    const QString workDir = m_dataDir;
    QPointer<BatteryModel> guard(this);
    QThreadPool::globalInstance()->start([workDir, guard]() {
        const PowercfgReportData data = PowercfgReport::generate(workDir);
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, data]() {
                if (guard)
                    guard->applyReport(data);
            }, Qt::QueuedConnection);
        }
    });
}

void BatteryModel::applySnapshot(const BatterySnapshot &snapshot)
{
    m_snapshot = snapshot;
    m_ready = true;

    if (m_database.isOpen()) {
        const qint64 nowMs = snapshot.timestamp.toMSecsSinceEpoch();
        for (const BatteryDevice &device : snapshot.batteries)
            m_database.upsertStatic(device, nowMs);
        m_database.insertSample(snapshot);
    }

    appendLivePoint(snapshot);

    if (m_calibration && snapshot.batteryPresent)
        m_calibration->update(chargePercent(), onAcPower(), snapshot.timestamp);

    if (m_alerts && snapshot.batteryPresent)
        m_alerts->process(currentAlertInput());

    emit snapshotChanged();
    emit staticInfoChanged();
}

AlertInput BatteryModel::currentAlertInput() const
{
    AlertInput input;
    input.percent = chargePercent();
    if (voltageV() > 0)
        input.voltageV = voltageV();
    // Temperature feeds the alert engine ONLY when a real battery thermal
    // sensor backs it. A system-zone estimate can sit tens of degrees away
    // from the pack; alerting on it would be a false alarm generator.
    if (temperatureAlertAvailable())
        input.temperatureC = temperatureC();
    input.charging = charging();
    input.onAcPower = onAcPower();
    const BatteryDevice *dev = primary();
    input.discharging = dev ? dev->discharging.value_or(false) : false;
    return input;
}

void BatteryModel::applyReport(const PowercfgReportData &report)
{
    m_report = report;
    if (report.ok) {
        if (m_database.isOpen())
            m_database.ingestCapacityHistory(report.history);
        m_habitInsights = HealthVerdict::chargingHabitInsights(report.usage);

        const QList<metrics::StateDrain> drains = metrics::perStateDrain(report.usage);
        m_expectedDrainW = 0.0;
        for (const metrics::StateDrain &d : drains) {
            if (d.state.compare(QStringLiteral("Active"), Qt::CaseInsensitive) == 0) {
                m_expectedDrainW = d.avgDrainmW / 1000.0;
                break;
            }
        }
    }
    emit reportChanged();
    emit snapshotChanged(); // report can improve design capacity / cycles
    emit staticInfoChanged();
}

void BatteryModel::appendLivePoint(const BatterySnapshot &snapshot)
{
    if (!m_sessionOrigin.isValid())
        m_sessionOrigin = snapshot.timestamp;

    QVariantMap point;
    point.insert(QStringLiteral("t"),
                 static_cast<double>(m_sessionOrigin.secsTo(snapshot.timestamp)));
    point.insert(QStringLiteral("percent"), chargePercent());
    point.insert(QStringLiteral("remainingmWh"), remainingCapacitymWh());
    point.insert(QStringLiteral("powerW"), powerKnown() ? netPowerW() : 0.0);
    point.insert(QStringLiteral("charging"), charging());

    // Critical-discharge marker: firmware critical flag, or a draw far above
    // the typical active drain from the usage history (min 25 W).
    const BatteryDevice *dev = primary();
    const double criticalDrawW =
        qMax(25.0, m_expectedDrainW > 0 ? m_expectedDrainW * 2.0 : 25.0);
    const bool critical = (dev && dev->critical.value_or(false))
                          || (!charging() && powerKnown() && netPowerW() < -criticalDrawW);
    point.insert(QStringLiteral("critical"), critical);
    m_liveSeries.append(point);
    while (m_liveSeries.size() > kMaxLivePoints)
        m_liveSeries.removeFirst();
    emit liveSeriesChanged();
}

const BatteryDevice *BatteryModel::primary() const
{
    if (m_snapshot.batteries.isEmpty())
        return nullptr;
    for (const BatteryDevice &b : m_snapshot.batteries) {
        if (b.fullChargedCapacitymWh.has_value() && *b.fullChargedCapacitymWh > 0)
            return &b;
    }
    return &m_snapshot.batteries.first();
}

QString BatteryModel::statusText() const
{
    if (!m_ready)
        return tr("Reading sensors…");
    if (!m_snapshot.batteryPresent)
        return tr("No battery detected");
    const BatteryDevice *dev = primary();
    if (!dev)
        return tr("Unknown");
    if (dev->charging.value_or(false))
        return tr("Charging");
    if (dev->discharging.value_or(false))
        return tr("Discharging");
    if (dev->powerOnline.value_or(false)) {
        if (chargePercent() >= 99.5)
            return tr("Fully charged");
        return tr("On AC power");
    }
    if (dev->win32Status.has_value()) {
        const QString s = BatteryReader::win32StatusToString(*dev->win32Status);
        if (!s.isEmpty())
            return QCoreApplication::translate("BatteryReader", s.toUtf8().constData());
    }
    return tr("Idle");
}

bool BatteryModel::onAcPower() const
{
    const BatteryDevice *dev = primary();
    return dev ? dev->powerOnline.value_or(false) : false;
}

bool BatteryModel::charging() const
{
    const BatteryDevice *dev = primary();
    return dev ? dev->charging.value_or(false) : false;
}

double BatteryModel::chargePercent() const
{
    // Aggregate across packs when possible.
    qint64 remaining = 0, full = 0;
    for (const BatteryDevice &b : m_snapshot.batteries) {
        if (b.remainingCapacitymWh.has_value() && b.fullChargedCapacitymWh.has_value()
            && *b.fullChargedCapacitymWh > 0) {
            remaining += *b.remainingCapacitymWh;
            full += *b.fullChargedCapacitymWh;
        }
    }
    if (full > 0)
        return std::clamp(100.0 * static_cast<double>(remaining) / static_cast<double>(full),
                          0.0, 100.0);
    const BatteryDevice *dev = primary();
    if (dev && dev->estimatedChargeRemainingPct.has_value())
        return static_cast<double>(*dev->estimatedChargeRemainingPct);
    return -1.0;
}

double BatteryModel::healthPercent() const
{
    const qint64 full = fullChargeCapacitymWh();
    const qint64 design = designCapacitymWh();
    if (full <= 0 || design <= 0)
        return -1.0;
    return std::clamp(100.0 * static_cast<double>(full) / static_cast<double>(design),
                      0.0, 100.0);
}

double BatteryModel::wearPercent() const
{
    const double health = healthPercent();
    return health < 0 ? -1.0 : 100.0 - health;
}

int BatteryModel::cycleCount() const
{
    // Sentinel policy (field defect F1): zero cycle counts mean "firmware
    // does not track cycles", never a measured value — treat as absent so
    // nothing downstream can reason from a phantom zero.
    const BatteryDevice *dev = primary();
    if (dev && dev->cycleCount.has_value() && *dev->cycleCount > 0)
        return static_cast<int>(*dev->cycleCount);
    if (m_report.ok && !m_report.batteries.isEmpty()) {
        const auto &cycles = m_report.batteries.first().cycleCount;
        if (cycles.has_value() && *cycles > 0)
            return static_cast<int>(*cycles);
    }
    return -1;
}

double BatteryModel::temperatureC() const
{
    return m_snapshot.temperatureC.value_or(-1000.0);
}

bool BatteryModel::temperatureKnown() const
{
    return m_snapshot.temperatureC.has_value();
}

bool BatteryModel::temperatureIsEstimate() const
{
    return m_snapshot.temperatureIsEstimate;
}

QString BatteryModel::temperatureSourceText() const
{
    if (!temperatureKnown())
        return tr("No usable thermal zone on this machine");
    if (m_snapshot.temperatureIsEstimate)
        return tr("System-zone estimate — this machine exposes no battery thermal sensor");
    for (const ThermalZone &zone : m_snapshot.thermalZones) {
        if (zone.valid && zone.isBatteryZone)
            return tr("Battery thermal sensor (%1)").arg(zone.instanceName);
    }
    return tr("Battery thermal sensor");
}

bool BatteryModel::temperatureAlertAvailable() const
{
    return temperatureKnown() && !m_snapshot.temperatureIsEstimate;
}

double BatteryModel::netPowerW() const
{
    double total = 0.0;
    bool known = false;
    for (const BatteryDevice &b : m_snapshot.batteries) {
        const auto p = metrics::netPowerW(b.chargeRatemW, b.dischargeRatemW);
        if (p.has_value()) {
            total += *p;
            known = true;
        }
    }
    return known ? total : 0.0;
}

bool BatteryModel::powerKnown() const
{
    for (const BatteryDevice &b : m_snapshot.batteries) {
        if (metrics::netPowerW(b.chargeRatemW, b.dischargeRatemW).has_value())
            return true;
    }
    return false;
}

double BatteryModel::voltageV() const
{
    // Voltage 0 is a firmware stub, not a measurement — a pack at 0 V could
    // not be powering the machine. Same sentinel policy as cycle counts.
    const BatteryDevice *dev = primary();
    if (dev && dev->voltagemV.has_value() && *dev->voltagemV > 0)
        return static_cast<double>(*dev->voltagemV) / 1000.0;
    return -1.0;
}

qint64 BatteryModel::resolvedDesignForPack(int index) const
{
    // Design-capacity precedence: powercfg > BatteryStaticData >
    // Win32_PortableBattery. powercfg wins because the firmware's own
    // report is the value Windows itself uses, and Win32_PortableBattery
    // has been observed to disagree with it on real hardware.
    if (m_report.ok && index < m_report.batteries.size()) {
        const auto &info = m_report.batteries.at(index);
        if (info.designCapacitymWh.has_value() && *info.designCapacitymWh > 0)
            return *info.designCapacitymWh;
    }
    if (index < m_snapshot.batteries.size()) {
        const BatteryDevice &b = m_snapshot.batteries.at(index);
        if (b.designedCapacitymWh.has_value() && *b.designedCapacitymWh > 0)
            return static_cast<qint64>(*b.designedCapacitymWh);
    }
    return 0;
}

qint64 BatteryModel::designCapacitymWh() const
{
    const int packs = qMax(m_snapshot.batteries.size(),
                           m_report.ok ? m_report.batteries.size() : 0);
    qint64 total = 0;
    for (int i = 0; i < packs; ++i)
        total += resolvedDesignForPack(i);
    return total > 0 ? total : -1;
}

qint64 BatteryModel::fullChargeCapacitymWh() const
{
    qint64 total = 0;
    for (const BatteryDevice &b : m_snapshot.batteries) {
        if (b.fullChargedCapacitymWh.has_value() && *b.fullChargedCapacitymWh > 0)
            total += *b.fullChargedCapacitymWh;
    }
    if (total > 0)
        return total;
    if (m_report.ok) {
        for (const auto &b : m_report.batteries)
            total += b.fullChargeCapacitymWh.value_or(0);
    }
    return total > 0 ? total : -1;
}

qint64 BatteryModel::remainingCapacitymWh() const
{
    qint64 total = 0;
    bool known = false;
    for (const BatteryDevice &b : m_snapshot.batteries) {
        if (b.remainingCapacitymWh.has_value()) {
            total += *b.remainingCapacitymWh;
            known = true;
        }
    }
    return known ? total : -1;
}

QString BatteryModel::formatDuration(qint64 seconds) const
{
    if (seconds < 0)
        return QString();
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    if (hours > 0)
        return tr("%1 h %2 min").arg(hours).arg(minutes);
    return tr("%1 min").arg(minutes);
}

QString BatteryModel::timeEstimateText() const
{
    const BatteryDevice *dev = primary();
    if (!dev)
        return QString();

    if (dev->charging.value_or(false)) {
        const auto ttf = metrics::timeToFullSec(dev->remainingCapacitymWh,
                                                dev->fullChargedCapacitymWh,
                                                dev->chargeRatemW);
        if (ttf.has_value())
            return tr("%1 to full charge").arg(formatDuration(*ttf));
        return QString();
    }

    if (dev->discharging.value_or(false)) {
        // Prefer the trend extrapolated from this session's samples.
        QVector<QPointF> pts;
        pts.reserve(m_liveSeries.size());
        for (const QVariant &v : m_liveSeries) {
            const QVariantMap m = v.toMap();
            const double remaining = m.value(QStringLiteral("remainingmWh")).toDouble();
            if (remaining > 0 && !m.value(QStringLiteral("charging")).toBool())
                pts.append(QPointF(m.value(QStringLiteral("t")).toDouble(), remaining));
        }
        if (pts.size() >= 3) {
            const auto tte = metrics::extrapolatedTimeToEmptySec(pts);
            if (tte.has_value())
                return tr("%1 remaining (trend)").arg(formatDuration(*tte));
        }
        const auto tte = metrics::timeToEmptySec(dev->remainingCapacitymWh,
                                                 dev->dischargeRatemW);
        if (tte.has_value())
            return tr("%1 remaining").arg(formatDuration(*tte));
        if (dev->estimatedRuntimeSec.has_value())
            return tr("%1 remaining").arg(formatDuration(*dev->estimatedRuntimeSec));
    }
    return QString();
}

Verdict BatteryModel::currentVerdict() const
{
    VerdictInput input;
    const double health = healthPercent();
    if (health >= 0)
        input.healthPercent = health;
    const int cycles = cycleCount();
    if (cycles >= 0)
        input.cycleCount = static_cast<quint32>(cycles);
    if (temperatureKnown())
        input.temperatureC = temperatureC();

    const BatteryDevice *dev = primary();
    if (dev) {
        input.calibrationDriftPercent = metrics::calibrationDriftPercent(
            dev->estimatedChargeRemainingPct, dev->remainingCapacitymWh,
            dev->fullChargedCapacitymWh);
    }

    if (m_report.ok && m_report.history.size() >= 3) {
        QList<QPair<QDate, double>> history;
        for (const auto &h : m_report.history) {
            if (h.fullChargeCapacitymWh.has_value() && *h.fullChargeCapacitymWh > 0)
                history.append({ h.start.date(),
                                 static_cast<double>(*h.fullChargeCapacitymWh) });
        }
        input.degradation = metrics::degradationCurve(history);
        const qint64 design = designCapacitymWh();
        if (design > 0)
            input.endOfLife = metrics::endOfLifeProjection(history,
                                                           static_cast<double>(design));
    }
    return HealthVerdict::generate(input);
}

QString BatteryModel::gradeName() const
{
    return HealthVerdict::gradeName(HealthVerdict::gradeForHealth(
        healthPercent() >= 0 ? std::optional<double>(healthPercent()) : std::nullopt));
}

QString BatteryModel::gradeColor() const
{
    switch (HealthVerdict::gradeForHealth(
        healthPercent() >= 0 ? std::optional<double>(healthPercent()) : std::nullopt)) {
    case HealthGrade::Excellent:   return QStringLiteral("#3fb950");
    case HealthGrade::Good:        return QStringLiteral("#7ee787");
    case HealthGrade::Fair:        return QStringLiteral("#d29922");
    case HealthGrade::Worn:        return QStringLiteral("#f0883e");
    case HealthGrade::ReplaceSoon: return QStringLiteral("#f85149");
    case HealthGrade::Unknown:     break;
    }
    return QStringLiteral("#8b949e");
}

QString BatteryModel::verdictHeadline() const
{
    return currentVerdict().headline;
}

QStringList BatteryModel::verdictDetails() const
{
    return currentVerdict().details;
}

bool BatteryModel::looksLikeAcpiIdentifier(const QString &name)
{
    if (name.isEmpty())
        return false;
    for (const QChar &c : name) {
        // Any lowercase letter or space reads as human text, not an ACPI id.
        if (c.isLower() || c.isSpace())
            return false;
        if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
            return false;
    }
    // ACPI object paths carry underscores ("BIF0_9") or are short
    // letters-then-digits object names ("BAT1", "BIF0"). Part numbers like
    // "45N1127" (digit-first, no underscore) stay visible.
    if (name.contains(QLatin1Char('_')))
        return true;
    static const QRegularExpression shortObject(
        QStringLiteral("^[A-Z]{2,4}[0-9]{1,2}$"));
    return shortObject.match(name).hasMatch();
}

QVariantMap BatteryModel::resolvedStaticFor(int index) const
{
    QVariantMap out;
    const BatteryDevice *dev =
        index < m_snapshot.batteries.size() ? &m_snapshot.batteries.at(index) : nullptr;
    const PowercfgReportData::BatteryInfo *info =
        (m_report.ok && index < m_report.batteries.size()) ? &m_report.batteries.at(index)
                                                           : nullptr;

    auto source = [dev](const char *field) {
        return dev ? dev->fieldSources.value(QLatin1String(field)) : QString();
    };
    auto put = [&out](const char *key, const QString &value, const QString &src) {
        out.insert(QLatin1String(key), value);
        out.insert(QLatin1String(key) + QStringLiteral("Source"),
                   value.isEmpty() ? QString() : src);
    };

    // Identity fields: BatteryStaticData is authoritative when it delivered;
    // powercfg is next (same firmware data, different transport); the CIMV2
    // classes are last-resort metadata.
    const QString kPowercfg = QStringLiteral("powercfg");

    // A raw ACPI object identifier is not a product name (field defect F3);
    // it stays visible in the Advanced drawer but never as the primary value.
    QString name = dev ? dev->name : QString();
    if (looksLikeAcpiIdentifier(name))
        name.clear();
    put("name", name, name.isEmpty() ? QString() : source("name"));

    QString manufacturer, manufacturerSrc;
    if (dev && !dev->manufacturer.isEmpty()
        && source("manufacturer") == QLatin1String("BatteryStaticData")) {
        manufacturer = dev->manufacturer;
        manufacturerSrc = source("manufacturer");
    } else if (info && !info->manufacturer.isEmpty()) {
        manufacturer = info->manufacturer;
        manufacturerSrc = kPowercfg;
    } else if (dev && !dev->manufacturer.isEmpty()) {
        manufacturer = dev->manufacturer;
        manufacturerSrc = source("manufacturer");
    }
    put("manufacturer", manufacturer, manufacturerSrc);

    QString serial, serialSrc;
    if (dev && !dev->serialNumber.isEmpty()) {
        serial = dev->serialNumber;
        serialSrc = source("serialNumber");
    } else if (info && !info->serialNumber.isEmpty()) {
        serial = info->serialNumber;
        serialSrc = kPowercfg;
    }
    put("serialNumber", serial, serialSrc);

    // Only BatteryStaticData carries a manufacture date; packs commonly
    // report an all-wildcard CIM datetime, which parses to empty here.
    put("manufactureDate", dev ? dev->manufactureDate : QString(),
        source("manufactureDate"));

    QString chemistry, chemistrySrc;
    if (dev && !dev->chemistry.isEmpty()
        && source("chemistry") == QLatin1String("BatteryStaticData")) {
        chemistry = dev->chemistry;
        chemistrySrc = source("chemistry");
    } else if (info && !info->chemistry.isEmpty()) {
        chemistry = BatteryReader::normalizeChemistryToken(info->chemistry);
        chemistrySrc = kPowercfg;
    } else if (dev && !dev->chemistry.isEmpty()) {
        chemistry = dev->chemistry;
        chemistrySrc = source("chemistry");
    }
    // Known chemistry names are marked for translation in BatteryReader;
    // unknown hardware tokens pass through unchanged.
    if (!chemistry.isEmpty())
        chemistry = QCoreApplication::translate("BatteryReader", chemistry.toUtf8().constData());
    put("chemistry", chemistry, chemistrySrc);

    QString uniqueId, uniqueIdSrc;
    if (dev && !dev->uniqueId.isEmpty()) {
        uniqueId = dev->uniqueId;
        uniqueIdSrc = source("uniqueId");
    } else if (info && !info->id.isEmpty()) {
        uniqueId = info->id;
        uniqueIdSrc = kPowercfg;
    }
    put("uniqueId", uniqueId, uniqueIdSrc);

    const qint64 design = resolvedDesignForPack(index);
    out.insert(QStringLiteral("designCapacitymWh"), design > 0 ? QVariant(design) : QVariant());
    QString designSrc;
    if (design > 0) {
        if (info && info->designCapacitymWh.has_value() && *info->designCapacitymWh == design)
            designSrc = kPowercfg;
        else
            designSrc = source("designCapacity");
    }
    out.insert(QStringLiteral("designCapacitymWhSource"), designSrc);
    return out;
}

QVariantList BatteryModel::staticInfo() const
{
    QVariantList rows;
    const int packs = qMax(m_snapshot.batteries.size(),
                           m_report.ok ? m_report.batteries.size() : 0);

    for (int i = 0; i < packs; ++i) {
        const QVariantMap resolved = resolvedStaticFor(i);
        auto addRow = [&rows, &resolved, i, packs](const char *key, const QString &label,
                                                   const QString &displayOverride) {
            const QString value = displayOverride.isEmpty()
                                      ? resolved.value(QLatin1String(key)).toString()
                                      : displayOverride;
            const bool available = !resolved.value(QLatin1String(key)).isNull()
                                   && resolved.value(QLatin1String(key)).isValid()
                                   && !resolved.value(QLatin1String(key)).toString().isEmpty();
            QVariantMap row;
            row.insert(QStringLiteral("pack"), i);
            row.insert(QStringLiteral("packLabel"),
                       packs > 1 ? tr("Pack %1").arg(i + 1) : QString());
            row.insert(QStringLiteral("field"), label);
            row.insert(QStringLiteral("value"), available ? value : QString());
            row.insert(QStringLiteral("source"),
                       resolved.value(QLatin1String(key) + QStringLiteral("Source")).toString());
            row.insert(QStringLiteral("available"), available);
            rows.append(row);
        };

        addRow("name", tr("Device name"), QString());
        addRow("manufacturer", tr("Manufacturer"), QString());
        addRow("serialNumber", tr("Serial number"), QString());
        addRow("manufactureDate", tr("Manufacture date"), QString());
        addRow("chemistry", tr("Chemistry"), QString());
        // Unique ID (a raw firmware identifier) moved to the Advanced
        // drawer in the F3 fix — it is diagnostics, not identity.
        const QVariant design = resolved.value(QStringLiteral("designCapacitymWh"));
        addRow("designCapacitymWh", tr("Design capacity"),
               design.isValid() && design.toLongLong() > 0
                   ? formatEnergy(design.toDouble())
                   : QString());
    }
    return rows;
}

QVariantList BatteryModel::batteries() const
{
    QVariantList list;
    for (const BatteryDevice &b : m_snapshot.batteries) {
        QVariantMap map;
        map.insert(QStringLiteral("instanceName"), b.instanceName);
        map.insert(QStringLiteral("name"), b.name);
        map.insert(QStringLiteral("manufacturer"), b.manufacturer);
        map.insert(QStringLiteral("serialNumber"), b.serialNumber);
        map.insert(QStringLiteral("chemistry"), b.chemistry);
        map.insert(QStringLiteral("manufactureDate"), b.manufactureDate);
        map.insert(QStringLiteral("uniqueId"), b.uniqueId);
        map.insert(QStringLiteral("designCapacitymWh"), optToVariant(b.designedCapacitymWh));
        map.insert(QStringLiteral("fullChargeCapacitymWh"),
                   optToVariant(b.fullChargedCapacitymWh));
        map.insert(QStringLiteral("remainingmWh"), optToVariant(b.remainingCapacitymWh));
        map.insert(QStringLiteral("cycleCount"), optToVariant(b.cycleCount));
        map.insert(QStringLiteral("voltagemV"), optToVariant(b.voltagemV));
        map.insert(QStringLiteral("chargeRatemW"), optToVariant(b.chargeRatemW));
        map.insert(QStringLiteral("dischargeRatemW"), optToVariant(b.dischargeRatemW));
        map.insert(QStringLiteral("charging"), optToVariant(b.charging));
        map.insert(QStringLiteral("discharging"), optToVariant(b.discharging));
        map.insert(QStringLiteral("powerOnline"), optToVariant(b.powerOnline));
        list.append(map);
    }
    return list;
}

QVariantList BatteryModel::rawStreams() const
{
    QVariantList list;
    auto add = [&list](const QString &source, const QString &field, const QVariant &value) {
        QVariantMap map;
        map.insert(QStringLiteral("source"), source);
        map.insert(QStringLiteral("field"), field);
        map.insert(QStringLiteral("value"),
                   value.isValid() ? value.toString() : QStringLiteral("—"));
        list.append(map);
    };

    for (const BatteryDevice &b : m_snapshot.batteries) {
        const QString src = b.instanceName;
        add(src, QStringLiteral("RemainingCapacity (mWh)"), optToVariant(b.remainingCapacitymWh));
        add(src, QStringLiteral("FullChargedCapacity (mWh)"),
            optToVariant(b.fullChargedCapacitymWh));
        add(src, QStringLiteral("DesignedCapacity (mWh)"), optToVariant(b.designedCapacitymWh));
        add(src, QStringLiteral("ChargeRate (mW)"), optToVariant(b.chargeRatemW));
        add(src, QStringLiteral("DischargeRate (mW)"), optToVariant(b.dischargeRatemW));
        add(src, QStringLiteral("Voltage (mV)"), optToVariant(b.voltagemV));
        add(src, QStringLiteral("CycleCount"), optToVariant(b.cycleCount));
        add(src, QStringLiteral("PowerOnline"), optToVariant(b.powerOnline));
        add(src, QStringLiteral("Charging"), optToVariant(b.charging));
        add(src, QStringLiteral("Discharging"), optToVariant(b.discharging));
        add(src, QStringLiteral("Critical"), optToVariant(b.critical));
        add(src, QStringLiteral("EstimatedRuntime (s)"), optToVariant(b.estimatedRuntimeSec));
        add(src, QStringLiteral("Win32 BatteryStatus"),
            b.win32Status.has_value()
                ? QVariant(QStringLiteral("%1 (%2)").arg(*b.win32Status)
                               .arg(BatteryReader::win32StatusToString(*b.win32Status)))
                : QVariant());
        add(src, QStringLiteral("EstimatedChargeRemaining (%)"),
            b.estimatedChargeRemainingPct.has_value()
                ? QVariant(*b.estimatedChargeRemainingPct) : QVariant());
        add(src, QStringLiteral("ManufactureDate"),
            b.manufactureDate.isEmpty() ? QVariant() : QVariant(b.manufactureDate));
        add(src, QStringLiteral("UniqueID"),
            b.uniqueId.isEmpty() ? QVariant() : QVariant(b.uniqueId));
        add(src, QStringLiteral("DeviceName (raw)"),
            b.name.isEmpty() ? QVariant() : QVariant(b.name));
    }
    for (const ThermalZone &zone : m_snapshot.thermalZones) {
        add(zone.instanceName, QStringLiteral("Temperature (°C)"),
            zone.valid ? QVariant(QString::number(zone.celsius, 'f', 1))
                       : QVariant(QStringLiteral("stub/invalid")));
    }
    return list;
}

QVariantList BatteryModel::thermalZones() const
{
    QVariantList list;
    for (const ThermalZone &zone : m_snapshot.thermalZones) {
        QVariantMap map;
        map.insert(QStringLiteral("instanceName"), zone.instanceName);
        map.insert(QStringLiteral("celsius"), zone.celsius);
        map.insert(QStringLiteral("valid"), zone.valid);
        map.insert(QStringLiteral("isBatteryZone"), zone.isBatteryZone);
        list.append(map);
    }
    return list;
}

QString BatteryModel::theme() const
{
    return m_settings.theme();
}

void BatteryModel::setTheme(const QString &theme)
{
    if (theme == m_settings.theme())
        return;
    m_settings.setValue(QStringLiteral("theme"), theme);
    m_settings.save();
    emit settingsChanged();
}

QString BatteryModel::language() const
{
    return m_settings.language();
}

void BatteryModel::setLanguage(const QString &language)
{
    if (language.isEmpty() || language == m_settings.language())
        return;
    m_settings.setValue(QStringLiteral("language"), language);
    m_settings.save();
    emit languageChanged();
    // Strings built in C++ (verdict, status, insights) are re-read by QML.
    emit snapshotChanged();
    emit reportChanged();
    emit staticInfoChanged();
}

int BatteryModel::sampleIntervalSec() const
{
    return m_settings.sampleIntervalSec();
}

void BatteryModel::setSampleIntervalSec(int seconds)
{
    const int clamped = qBound(1, seconds, 3600);
    if (clamped == m_settings.sampleIntervalSec())
        return;
    m_settings.setValue(QStringLiteral("sampleIntervalSec"), clamped);
    m_settings.save();
    if (m_sampler) {
        QMetaObject::invokeMethod(m_sampler, "setIntervalSec", Qt::QueuedConnection,
                                  Q_ARG(int, clamped));
    }
    emit settingsChanged();
}

double BatteryModel::expectedDischargeSlopePctPerHour() const
{
    const qint64 fcc = fullChargeCapacitymWh();
    if (m_expectedDrainW <= 0.0 || fcc <= 0)
        return 0.0;
    return -100.0 * (m_expectedDrainW * 1000.0) / static_cast<double>(fcc);
}

QVariantMap BatteryModel::liveTrend() const
{
    QVariantMap result;
    result.insert(QStringLiteral("valid"), false);
    if (m_liveSeries.size() < 3)
        return result;

    const bool nowCharging = m_liveSeries.last().toMap()
                                 .value(QStringLiteral("charging")).toBool();
    QVector<QPointF> pts;
    pts.reserve(m_liveSeries.size());
    for (const QVariant &v : m_liveSeries) {
        const QVariantMap m = v.toMap();
        const double pct = m.value(QStringLiteral("percent")).toDouble();
        if (pct < 0)
            continue;
        if (m.value(QStringLiteral("charging")).toBool() != nowCharging)
            continue;
        pts.append(QPointF(m.value(QStringLiteral("t")).toDouble(), pct));
    }
    const metrics::RegressionResult fit = metrics::linearRegression(pts);
    if (!fit.valid)
        return result;

    const double lastT = pts.last().x();
    result.insert(QStringLiteral("valid"), true);
    result.insert(QStringLiteral("slopePctPerHour"), fit.slope * 3600.0);
    result.insert(QStringLiteral("lastT"), lastT);
    result.insert(QStringLiteral("lastY"), fit.slope * lastT + fit.intercept);
    return result;
}

QVariantList BatteryModel::capacityHistoryList() const
{
    // Merge DB rows (past ingests) with the fresh report, deduplicated on
    // (start date, source). QMap keeps them date-sorted.
    QMap<QString, QVariantMap> merged;

    auto addPoint = [&merged](const QDate &start, const QDate &end,
                              const QVariant &design, const QVariant &full,
                              const QVariant &cyclesIn, const QString &source) {
        if (!start.isValid())
            return;
        // Cycle sentinel policy: zero = untracked, not a data point.
        const QVariant cycles =
            (cyclesIn.isValid() && cyclesIn.toLongLong() > 0) ? cyclesIn : QVariant();
        const QString key = start.toString(Qt::ISODate) + QLatin1Char('|') + source;
        QVariantMap map;
        map.insert(QStringLiteral("dateMs"),
                   QDateTime(start, QTime(0, 0)).toMSecsSinceEpoch());
        map.insert(QStringLiteral("endMs"),
                   end.isValid() ? QDateTime(end, QTime(0, 0)).toMSecsSinceEpoch()
                                 : QVariant());
        map.insert(QStringLiteral("designmWh"), design);
        map.insert(QStringLiteral("fullChargemWh"), full);
        map.insert(QStringLiteral("cycleCount"), cycles);
        map.insert(QStringLiteral("source"), source);
        merged.insert(key, map);
    };

    if (m_database.isOpen()) {
        // capacityHistory() is const-safe: it only reads.
        const auto rows = const_cast<Database &>(m_database).capacityHistory();
        for (const CapacityHistoryRow &row : rows) {
            addPoint(row.startDate, row.endDate,
                     row.designmWh.has_value() ? QVariant(*row.designmWh) : QVariant(),
                     row.fullChargemWh.has_value() ? QVariant(*row.fullChargemWh) : QVariant(),
                     row.cycleCount.has_value() ? QVariant(*row.cycleCount) : QVariant(),
                     row.source);
        }
    }
    if (m_report.ok) {
        for (const auto &h : m_report.history) {
            addPoint(h.start.date(), h.end.date(),
                     h.designCapacitymWh.has_value() ? QVariant(*h.designCapacitymWh)
                                                     : QVariant(),
                     h.fullChargeCapacitymWh.has_value() ? QVariant(*h.fullChargeCapacitymWh)
                                                         : QVariant(),
                     h.cycleCount.has_value() ? QVariant(*h.cycleCount) : QVariant(),
                     QStringLiteral("powercfg"));
        }
    }

    QVariantList list;
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it)
        list.append(it.value());
    return list;
}

QVariantList BatteryModel::usageLog(int maxEntries) const
{
    QVariantList list;
    if (!m_report.ok)
        return list;

    const int total = m_report.usage.size();
    const int first = qMax(0, total - qMax(1, maxEntries));
    for (int i = total - 1; i >= first; --i) { // newest first
        const auto &e = m_report.usage.at(i);
        QVariantMap map;
        map.insert(QStringLiteral("tsMs"), e.timestamp.toMSecsSinceEpoch());
        map.insert(QStringLiteral("type"), e.entryType);
        map.insert(QStringLiteral("ac"), e.ac);
        map.insert(QStringLiteral("durationSec"), e.duration100ns / 10000000ll);
        map.insert(QStringLiteral("deltamWh"),
                   e.dischargemWh.has_value() ? QVariant(-*e.dischargemWh) : QVariant());
        double pct = -1;
        if (e.chargeCapacitymWh.has_value() && e.fullChargeCapacitymWh.has_value()
            && *e.fullChargeCapacitymWh > 0) {
            pct = 100.0 * static_cast<double>(*e.chargeCapacitymWh)
                        / static_cast<double>(*e.fullChargeCapacitymWh);
        }
        map.insert(QStringLiteral("percent"), pct);
        list.append(map);
    }
    return list;
}

QVariantMap BatteryModel::degradationInfo() const
{
    QVariantMap result;
    result.insert(QStringLiteral("valid"), false);

    QList<QPair<QDate, double>> history;
    const QVariantList points = capacityHistoryList();
    for (const QVariant &v : points) {
        const QVariantMap m = v.toMap();
        const QVariant full = m.value(QStringLiteral("fullChargemWh"));
        if (!full.isValid() || full.toDouble() <= 0)
            continue;
        history.append({ QDateTime::fromMSecsSinceEpoch(
                             m.value(QStringLiteral("dateMs")).toLongLong()).date(),
                         full.toDouble() });
    }
    if (history.size() < 3)
        return result;

    const metrics::RegressionResult fit = metrics::degradationCurve(history);
    if (!fit.valid)
        return result;

    result.insert(QStringLiteral("valid"), true);
    result.insert(QStringLiteral("slopeMWhPerDay"), fit.slope);
    result.insert(QStringLiteral("r2"), fit.r2);
    const qint64 design = designCapacitymWh();
    if (design > 0) {
        const auto eol = metrics::endOfLifeProjection(history,
                                                      static_cast<double>(design));
        if (eol.has_value())
            result.insert(QStringLiteral("eolDate"),
                          QLocale().toString(*eol, QStringLiteral("MMMM yyyy")));
    }
    return result;
}

bool BatteryModel::minimizeToTray() const
{
    return m_settings.minimizeToTray();
}

bool BatteryModel::autostartEnabled() const
{
    return Autostart::isEnabled();
}

void BatteryModel::setAutostartEnabled(bool enabled)
{
    QString error;
    if (!Autostart::setEnabled(enabled, QString(), QString(), &error) && !error.isEmpty())
        emit acquisitionError(tr("Autostart change failed: %1").arg(error));
    // Mirror the preference in settings for transparency (the shortcut
    // itself remains the source of truth).
    m_settings.setValue(QStringLiteral("launchWithWindows"), Autostart::isEnabled());
    m_settings.save();
    emit settingsChanged();
}

QVariant BatteryModel::settingValue(const QString &key) const
{
    return m_settings.value(key);
}

void BatteryModel::setSetting(const QString &key, const QVariant &value)
{
    m_settings.setValue(key, value);
    m_settings.save();
    if (key == QLatin1String("sampleIntervalSec") && m_sampler) {
        QMetaObject::invokeMethod(m_sampler, "setIntervalSec", Qt::QueuedConnection,
                                  Q_ARG(int, m_settings.sampleIntervalSec()));
    }
    emit settingsChanged();
}

QString BatteryModel::tryToggleChargeCap(bool enable)
{
    QString error;
    if (ChargeCap::setEnabled(enable, &error))
        return tr("Charge cap updated.");
    return error;
}

QString BatteryModel::temperatureUnit() const
{
    return m_settings.temperatureUnit();
}

QString BatteryModel::energyUnit() const
{
    return m_settings.energyUnit();
}

QString BatteryModel::formatTemperature(double celsius) const
{
    if (m_settings.temperatureUnit() == QLatin1String("F"))
        return QStringLiteral("%1 °F").arg(celsius * 9.0 / 5.0 + 32.0, 0, 'f', 1);
    return QStringLiteral("%1 °C").arg(celsius, 0, 'f', 1);
}

QString BatteryModel::formatEnergy(double mWh) const
{
    if (m_settings.energyUnit() == QLatin1String("Wh"))
        return QStringLiteral("%1 Wh").arg(mWh / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1 mWh").arg(qRound64(mWh));
}

QObject *BatteryModel::calibrationObject() const
{
    return m_calibration;
}

double BatteryModel::calibrationDrift() const
{
    const BatteryDevice *dev = primary();
    if (!dev)
        return 0.0;
    return metrics::calibrationDriftPercent(dev->estimatedChargeRemainingPct,
                                            dev->remainingCapacitymWh,
                                            dev->fullChargedCapacitymWh)
        .value_or(0.0);
}

bool BatteryModel::calibrationDriftKnown() const
{
    const BatteryDevice *dev = primary();
    if (!dev)
        return false;
    return metrics::calibrationDriftPercent(dev->estimatedChargeRemainingPct,
                                            dev->remainingCapacitymWh,
                                            dev->fullChargedCapacitymWh)
        .has_value();
}

bool BatteryModel::calibrationRecommended() const
{
    return calibrationDriftKnown()
           && Calibration::driftIndicatesCalibration(calibrationDrift());
}

QString BatteryModel::exportDir() const
{
    if (!m_exportDirOverride.isEmpty())
        return m_exportDirOverride;
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir(docs).filePath(QStringLiteral("Faraday exports"));
}

QString BatteryModel::exportSamplesCsv(int days)
{
    m_lastExportError.clear();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<SampleRow> samples = const_cast<Database &>(m_database)
        .samplesBetween(now - qint64(qMax(1, days)) * 86400000ll, now);
    const QString path = QDir(exportDir()).filePath(
        QStringLiteral("faraday_samples_%1.csv")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    if (!Exporter::exportCsv(samples, path, &m_lastExportError))
        return QString();
    return path;
}

QString BatteryModel::exportSamplesJson(int days)
{
    m_lastExportError.clear();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<SampleRow> samples = const_cast<Database &>(m_database)
        .samplesBetween(now - qint64(qMax(1, days)) * 86400000ll, now);
    const QString path = QDir(exportDir()).filePath(
        QStringLiteral("faraday_samples_%1.json")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    if (!Exporter::exportJson(samples, path, &m_lastExportError))
        return QString();
    return path;
}

QString BatteryModel::exportHtmlReport()
{
    m_lastExportError.clear();

    ReportInput input;
    input.appVersion = QCoreApplication::applicationVersion();
    input.generatedAt = QDateTime::currentDateTime();
    if (!m_snapshot.batteries.isEmpty() || (m_report.ok && !m_report.batteries.isEmpty())) {
        const QVariantMap resolved = resolvedStaticFor(0);
        input.batteryName = resolved.value(QStringLiteral("name")).toString();
        input.manufacturer = resolved.value(QStringLiteral("manufacturer")).toString();
        input.chemistry = resolved.value(QStringLiteral("chemistry")).toString();
        input.serialNumber = resolved.value(QStringLiteral("serialNumber")).toString();
    }
    input.healthPercent = healthPercent();
    input.wearPercent = wearPercent();
    input.cycleCount = cycleCount();
    input.designmWh = designCapacitymWh();
    input.fullChargemWh = fullChargeCapacitymWh();
    input.gradeName = gradeName();
    input.gradeColor = gradeColor();
    input.verdictHeadline = verdictHeadline();
    input.verdictDetails = verdictDetails();
    input.insights = m_habitInsights;

    const QVariantList points = capacityHistoryList();
    for (const QVariant &v : points) {
        const QVariantMap m = v.toMap();
        CapacityHistoryRow row;
        row.startDate = QDateTime::fromMSecsSinceEpoch(
                            m.value(QStringLiteral("dateMs")).toLongLong()).date();
        const QVariant full = m.value(QStringLiteral("fullChargemWh"));
        if (full.isValid())
            row.fullChargemWh = full.toLongLong();
        const QVariant design = m.value(QStringLiteral("designmWh"));
        if (design.isValid())
            row.designmWh = design.toLongLong();
        const QVariant cycles = m.value(QStringLiteral("cycleCount"));
        if (cycles.isValid())
            row.cycleCount = cycles.toLongLong();
        row.source = m.value(QStringLiteral("source")).toString();
        input.history.append(row);
    }

    const QString path = QDir(exportDir()).filePath(
        QStringLiteral("faraday_health_report_%1.html")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    if (!Exporter::writeHtmlReport(input, path, &m_lastExportError))
        return QString();
    return path;
}

void BatteryModel::resetSessionOrigin()
{
    m_sessionOrigin = QDateTime::currentDateTime();
    m_liveSeries.clear();
    if (m_database.isOpen()) {
        const BatteryDevice *dev = primary();
        std::optional<qint64> origin;
        if (dev && dev->remainingCapacitymWh.has_value())
            origin = static_cast<qint64>(*dev->remainingCapacitymWh);
        m_database.beginSession(m_sessionOrigin.toMSecsSinceEpoch(), origin,
                                QStringLiteral("session origin reset"));
    }
    emit liveSeriesChanged();
}

void BatteryModel::sampleNow()
{
    if (m_sampler)
        QMetaObject::invokeMethod(m_sampler, "sampleNow", Qt::QueuedConnection);
}

} // namespace faraday
