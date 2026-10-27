#include "BatteryReader.h"

#include <QtGlobal>

namespace faraday {

namespace {

const quint32 kRuntimeUnknown = 0xFFFFFFFFu;
const quint32 kRuntimeMaxPlausibleSec = 60u * 60u * 24u * 7u; // one week

std::optional<quint32> optUInt(const QVariantMap &row, const QString &key)
{
    const auto it = row.constFind(key);
    if (it == row.constEnd() || !it->isValid())
        return std::nullopt;
    bool convOk = false;
    const quint32 v = it->toUInt(&convOk);
    if (!convOk)
        return std::nullopt;
    return v;
}

std::optional<qint32> optInt(const QVariantMap &row, const QString &key)
{
    const auto it = row.constFind(key);
    if (it == row.constEnd() || !it->isValid())
        return std::nullopt;
    bool convOk = false;
    const qint32 v = it->toInt(&convOk);
    if (!convOk)
        return std::nullopt;
    return v;
}

std::optional<bool> optBool(const QVariantMap &row, const QString &key)
{
    const auto it = row.constFind(key);
    if (it == row.constEnd() || !it->isValid())
        return std::nullopt;
    return it->toBool();
}

QString optString(const QVariantMap &row, const QString &key)
{
    const auto it = row.constFind(key);
    if (it == row.constEnd() || !it->isValid())
        return QString();
    return it->toString().trimmed();
}

} // namespace

BatteryReader::BatteryReader()
    : m_rootWmi(new WmiClient()),
      m_cimv2(new WmiClient())
{
}

BatteryReader::~BatteryReader() = default;

bool BatteryReader::initialize()
{
    m_rootWmiOk = m_rootWmi->connect(QStringLiteral("ROOT\\WMI"));
    m_cimv2Ok = m_cimv2->connect(QStringLiteral("ROOT\\CIMV2"));
    return m_rootWmiOk || m_cimv2Ok;
}

double BatteryReader::thermalRawToCelsius(quint32 raw)
{
    return raw / 10.0 - 273.15;
}

bool BatteryReader::thermalRawIsValid(quint32 raw)
{
    // Raw <= 2732 (i.e. <= 273.2 K ~ 0 C) reads as a firmware stub zone;
    // > 120 C is not a sane temperature for a battery monitor to report.
    // Compare on the raw integer to avoid floating-point edge cases.
    return raw > 2732 && thermalRawToCelsius(raw) <= 120.0;
}

std::optional<quint32> BatteryReader::sanitizeRuntime(quint32 seconds)
{
    if (seconds == kRuntimeUnknown || seconds == 0 || seconds > kRuntimeMaxPlausibleSec)
        return std::nullopt;
    return seconds;
}

std::optional<quint32> BatteryReader::sanitizeCycleCount(quint32 cycles)
{
    if (cycles == 0)
        return std::nullopt; // untracked-cycles firmware reports 0 (see header)
    return cycles;
}

QString BatteryReader::chemistryToString(int code)
{
    switch (code) {
    case 1: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Other"));
    case 2: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Unknown"));
    case 3: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Lead Acid"));
    case 4: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Nickel Cadmium"));
    case 5: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Nickel Metal Hydride"));
    case 6: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Lithium-ion"));
    case 7: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Zinc Air"));
    case 8: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Lithium Polymer"));
    default: return QString();
    }
}

QString BatteryReader::normalizeChemistryToken(const QString &token)
{
    const QString t = token.trimmed();
    if (t.isEmpty())
        return QString();
    const QString upper = t.toUpper();
    if (upper == QLatin1String("LION") || upper == QLatin1String("LI-I")
        || upper == QLatin1String("LI I") || upper == QLatin1String("LIION"))
        return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Lithium-ion"));
    if (upper == QLatin1String("LIP") || upper == QLatin1String("LIPO"))
        return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Lithium Polymer"));
    if (upper == QLatin1String("PBAC"))
        return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Lead Acid"));
    if (upper == QLatin1String("NICD"))
        return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Nickel Cadmium"));
    if (upper == QLatin1String("NIMH"))
        return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Nickel Metal Hydride"));
    if (upper == QLatin1String("NIZN"))
        return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Nickel Zinc"));
    if (upper == QLatin1String("RAM"))
        return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Rechargeable Alkaline"));
    // Unknown but real hardware token: pass it through rather than guess.
    return t;
}

QString BatteryReader::chemistryFourCCToString(quint32 code)
{
    if (code == 0)
        return QString();
    // Little-endian packed ASCII, e.g. 0x6E6F694C -> 'L','i','o','n'.
    QString ascii;
    for (int shift = 0; shift <= 24; shift += 8) {
        const char c = static_cast<char>((code >> shift) & 0xFFu);
        if (c == '\0')
            break;
        if (c < 0x20 || c > 0x7E)
            return QString(); // not a printable FourCC; refuse to guess
        ascii.append(QLatin1Char(c));
    }
    if (ascii.isEmpty())
        return QString();
    return normalizeChemistryToken(ascii);
}

QString BatteryReader::manufactureDateToIso(const QString &cimDateTime)
{
    // CIM datetime: "yyyymmddHHMMSS.mmmmmm+UUU"; unknown fields are '*'.
    const QString s = cimDateTime.trimmed();
    if (s.size() < 8)
        return QString();
    const QString yearStr = s.left(4);
    const QString monthStr = s.mid(4, 2);
    const QString dayStr = s.mid(6, 2);
    bool okY = false, okM = false, okD = false;
    const int year = yearStr.toInt(&okY);
    const int month = monthStr.toInt(&okM);
    const int day = dayStr.toInt(&okD);
    if (!okY || !okM || !okD)
        return QString(); // asterisks or garbage: the pack reports no date
    if (!QDate(year, month, day).isValid() || year < 1990 || year > 2100)
        return QString();
    return QStringLiteral("%1-%2-%3")
        .arg(year, 4, 10, QLatin1Char('0'))
        .arg(month, 2, 10, QLatin1Char('0'))
        .arg(day, 2, 10, QLatin1Char('0'));
}

QString BatteryReader::win32StatusToString(int code)
{
    switch (code) {
    case 1: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Discharging"));
    case 2: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "On AC power"));
    case 3: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Fully charged"));
    case 4: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Low"));
    case 5: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Critical"));
    case 6: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Charging"));
    case 7: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Charging (high)"));
    case 8: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Charging (low)"));
    case 9: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Charging (critical)"));
    case 10: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Undefined"));
    case 11: return QStringLiteral(QT_TRANSLATE_NOOP("BatteryReader", "Partially charged"));
    default: return QString();
    }
}

bool BatteryReader::snapshotHasRealBattery(const BatterySnapshot &snapshot)
{
    for (const BatteryDevice &b : snapshot.batteries) {
        if (b.fullChargedCapacitymWh.has_value() && *b.fullChargedCapacitymWh > 0)
            return true;
        if (b.designedCapacitymWh.has_value() && *b.designedCapacitymWh > 0)
            return true;
        // Win32_Battery-only systems: a live percentage still counts.
        if (b.estimatedChargeRemainingPct.has_value())
            return true;
    }
    return false;
}

BatteryDevice &BatteryReader::deviceFor(BatterySnapshot &snapshot, const QString &instanceName)
{
    for (BatteryDevice &b : snapshot.batteries) {
        if (b.instanceName == instanceName)
            return b;
    }
    BatteryDevice fresh;
    fresh.instanceName = instanceName;
    snapshot.batteries.append(fresh);
    return snapshot.batteries.last();
}

void BatteryReader::applyStaticRows(BatterySnapshot &snapshot, const QList<QVariantMap> &rows)
{
    for (const QVariantMap &row : rows) {
        const QString instance = optString(row, QStringLiteral("InstanceName"));
        if (instance.isEmpty())
            continue;
        BatteryDevice &dev = deviceFor(snapshot, instance);

        const QString kStatic = QStringLiteral("BatteryStaticData");
        dev.designedCapacitymWh = optUInt(row, QStringLiteral("DesignedCapacity"));
        if (dev.designedCapacitymWh.has_value())
            dev.fieldSources.insert(QStringLiteral("designCapacity"), kStatic);
        const QString mfg = optString(row, QStringLiteral("ManufactureName"));
        if (!mfg.isEmpty()) {
            dev.manufacturer = mfg;
            dev.fieldSources.insert(QStringLiteral("manufacturer"), kStatic);
        }
        const QString serial = optString(row, QStringLiteral("SerialNumber"));
        if (!serial.isEmpty()) {
            dev.serialNumber = serial;
            dev.fieldSources.insert(QStringLiteral("serialNumber"), kStatic);
        }
        const QString devName = optString(row, QStringLiteral("DeviceName"));
        if (!devName.isEmpty()) {
            dev.name = devName;
            dev.fieldSources.insert(QStringLiteral("name"), kStatic);
        }
        const QString uid = optString(row, QStringLiteral("UniqueID"));
        if (!uid.isEmpty()) {
            dev.uniqueId = uid;
            dev.fieldSources.insert(QStringLiteral("uniqueId"), kStatic);
        }
        dev.manufactureDate =
            manufactureDateToIso(optString(row, QStringLiteral("ManufactureDate")));
        if (!dev.manufactureDate.isEmpty())
            dev.fieldSources.insert(QStringLiteral("manufactureDate"), kStatic);
        const std::optional<quint32> chemCode = optUInt(row, QStringLiteral("Chemistry"));
        if (chemCode.has_value()) {
            const QString chem = chemistryFourCCToString(*chemCode);
            if (!chem.isEmpty()) {
                dev.chemistry = chem;
                dev.fieldSources.insert(QStringLiteral("chemistry"), kStatic);
            }
        }
    }
}

void BatteryReader::applyRootWmiClassRows(BatterySnapshot &snapshot, const QByteArray &className,
                                          const QList<QVariantMap> &rows)
{
    for (const QVariantMap &row : rows) {
        const QString instance = optString(row, QStringLiteral("InstanceName"));
        if (instance.isEmpty())
            continue;
        BatteryDevice &dev = deviceFor(snapshot, instance);

        if (className == "BatteryFullChargedCapacity") {
            dev.fullChargedCapacitymWh = optUInt(row, QStringLiteral("FullChargedCapacity"));
        } else if (className == "BatteryCycleCount") {
            const std::optional<quint32> raw = optUInt(row, QStringLiteral("CycleCount"));
            dev.cycleCount = raw.has_value() ? sanitizeCycleCount(*raw) : std::nullopt;
        } else if (className == "BatteryStatus") {
            dev.remainingCapacitymWh = optUInt(row, QStringLiteral("RemainingCapacity"));
            dev.chargeRatemW = optInt(row, QStringLiteral("ChargeRate"));
            dev.dischargeRatemW = optInt(row, QStringLiteral("DischargeRate"));
            dev.voltagemV = optUInt(row, QStringLiteral("Voltage"));
            dev.powerOnline = optBool(row, QStringLiteral("PowerOnline"));
            dev.charging = optBool(row, QStringLiteral("Charging"));
            dev.discharging = optBool(row, QStringLiteral("Discharging"));
            dev.critical = optBool(row, QStringLiteral("Critical"));
        } else if (className == "BatteryRuntime") {
            const std::optional<quint32> raw = optUInt(row, QStringLiteral("EstimatedRuntime"));
            dev.estimatedRuntimeSec = raw.has_value() ? sanitizeRuntime(*raw) : std::nullopt;
        }
    }
}

void BatteryReader::applyCimv2Rows(BatterySnapshot &snapshot, const QList<QVariantMap> &win32,
                                   const QList<QVariantMap> &portable)
{
    // ROOT\WMI and CIMV2 use unrelated instance keys, so align by ordinal;
    // in practice enumeration order matches on multi-battery systems.
    const int count = qMax(win32.size(), portable.size());
    for (int i = 0; i < count; ++i) {
        BatteryDevice *dev = nullptr;
        if (i < snapshot.batteries.size()) {
            dev = &snapshot.batteries[i];
        } else {
            QString key;
            if (i < win32.size())
                key = optString(win32.at(i), QStringLiteral("DeviceID"));
            if (key.isEmpty() && i < portable.size())
                key = optString(portable.at(i), QStringLiteral("DeviceID"));
            if (key.isEmpty())
                key = QStringLiteral("Battery %1").arg(i);
            dev = &deviceFor(snapshot, key);
        }

        if (i < win32.size()) {
            const QVariantMap &row = win32.at(i);
            const QString kWin32 = QStringLiteral("Win32_Battery");
            if (dev->name.isEmpty()) {
                dev->name = optString(row, QStringLiteral("Name"));
                if (!dev->name.isEmpty())
                    dev->fieldSources.insert(QStringLiteral("name"), kWin32);
            }
            dev->win32Status = optInt(row, QStringLiteral("BatteryStatus"));
            dev->estimatedChargeRemainingPct =
                optInt(row, QStringLiteral("EstimatedChargeRemaining"));
            if (!dev->designVoltagemV.has_value())
                dev->designVoltagemV = optUInt(row, QStringLiteral("DesignVoltage"));
            if (dev->chemistry.isEmpty()) {
                const std::optional<qint32> chem = optInt(row, QStringLiteral("Chemistry"));
                if (chem.has_value()) {
                    dev->chemistry = chemistryToString(*chem);
                    if (!dev->chemistry.isEmpty())
                        dev->fieldSources.insert(QStringLiteral("chemistry"), kWin32);
                }
            }
        }

        if (i < portable.size()) {
            const QVariantMap &row = portable.at(i);
            const QString kPortable = QStringLiteral("Win32_PortableBattery");
            if (dev->manufacturer.isEmpty()) {
                dev->manufacturer = optString(row, QStringLiteral("Manufacturer"));
                if (!dev->manufacturer.isEmpty())
                    dev->fieldSources.insert(QStringLiteral("manufacturer"), kPortable);
            }
            if (dev->name.isEmpty()) {
                dev->name = optString(row, QStringLiteral("Name"));
                if (!dev->name.isEmpty())
                    dev->fieldSources.insert(QStringLiteral("name"), kPortable);
            }
            if (!dev->designedCapacitymWh.has_value()) {
                dev->designedCapacitymWh = optUInt(row, QStringLiteral("DesignCapacity"));
                if (dev->designedCapacitymWh.has_value())
                    dev->fieldSources.insert(QStringLiteral("designCapacity"), kPortable);
            }
            if (!dev->fullChargedCapacitymWh.has_value())
                dev->fullChargedCapacitymWh = optUInt(row, QStringLiteral("FullChargeCapacity"));
            if (dev->chemistry.isEmpty()) {
                const std::optional<qint32> chem = optInt(row, QStringLiteral("Chemistry"));
                if (chem.has_value()) {
                    dev->chemistry = chemistryToString(*chem);
                    if (!dev->chemistry.isEmpty())
                        dev->fieldSources.insert(QStringLiteral("chemistry"), kPortable);
                }
            }
        }
    }
}

ThermalZone BatteryReader::zoneFromRow(const QVariantMap &row)
{
    ThermalZone zone;
    zone.instanceName = optString(row, QStringLiteral("InstanceName"));
    const std::optional<quint32> raw = optUInt(row, QStringLiteral("CurrentTemperature"));
    if (raw.has_value()) {
        zone.celsius = thermalRawToCelsius(*raw);
        zone.valid = thermalRawIsValid(*raw);
    }
    zone.isBatteryZone = zone.instanceName.contains(QStringLiteral("BAT"), Qt::CaseInsensitive);
    return zone;
}

void BatteryReader::applyStaticDataResult(BatterySnapshot &snapshot, bool ok,
                                          const QList<QVariantMap> &rows, const QString &error)
{
    if (ok && !rows.isEmpty()) {
        m_staticCache = rows;
        applyStaticRows(snapshot, rows);
    } else if (!m_staticCache.isEmpty()) {
        applyStaticRows(snapshot, m_staticCache);
    } else if (!ok) {
        snapshot.unavailable << QStringLiteral("BatteryStaticData: %1").arg(error);
    }
}

void BatteryReader::mergeRootWmi(BatterySnapshot &snapshot)
{
    if (!m_rootWmiOk) {
        snapshot.unavailable << QStringLiteral("ROOT\\WMI: %1").arg(m_rootWmi->lastError());
        return;
    }

    // BatteryStaticData rides the flaky perf adapter; enumerate with the
    // CreateInstanceEnum fallback and reuse the cache across transient
    // failures (the data is static per boot).
    {
        bool ok = false;
        const QList<QVariantMap> rows =
            m_rootWmi->queryInstances(QStringLiteral("BatteryStaticData"), &ok);
        applyStaticDataResult(snapshot, ok, rows, m_rootWmi->lastError());
    }

    const QByteArray classes[] = {
        QByteArrayLiteral("BatteryFullChargedCapacity"),
        QByteArrayLiteral("BatteryCycleCount"),
        QByteArrayLiteral("BatteryStatus"),
        QByteArrayLiteral("BatteryRuntime"),
    };

    for (const QByteArray &className : classes) {
        bool ok = false;
        const QList<QVariantMap> rows =
            m_rootWmi->queryInstances(QLatin1String(className), &ok);
        if (!ok) {
            snapshot.unavailable << QStringLiteral("%1: %2")
                                        .arg(QLatin1String(className), m_rootWmi->lastError());
            continue;
        }
        applyRootWmiClassRows(snapshot, className, rows);
    }
}

void BatteryReader::mergeCimv2(BatterySnapshot &snapshot)
{
    if (!m_cimv2Ok) {
        snapshot.unavailable << QStringLiteral("ROOT\\CIMV2: %1").arg(m_cimv2->lastError());
        return;
    }

    bool ok = false;
    const QList<QVariantMap> win32 =
        m_cimv2->queryInstances(QStringLiteral("Win32_Battery"), &ok);
    if (!ok)
        snapshot.unavailable << QStringLiteral("Win32_Battery: %1").arg(m_cimv2->lastError());

    bool okPortable = false;
    const QList<QVariantMap> portable =
        m_cimv2->queryInstances(QStringLiteral("Win32_PortableBattery"), &okPortable);
    if (!okPortable)
        snapshot.unavailable << QStringLiteral("Win32_PortableBattery: %1").arg(m_cimv2->lastError());

    applyCimv2Rows(snapshot, win32, portable);
}

void BatteryReader::mergeThermal(BatterySnapshot &snapshot)
{
    if (!m_rootWmiOk)
        return; // already reported by mergeRootWmi

    bool ok = false;
    const QList<QVariantMap> rows =
        m_rootWmi->queryInstances(QStringLiteral("MsAcpi_ThermalZoneTemperature"), &ok);
    if (!ok) {
        snapshot.unavailable << QStringLiteral("MsAcpi_ThermalZoneTemperature: %1")
                                    .arg(m_rootWmi->lastError());
        return;
    }

    for (const QVariantMap &row : rows)
        snapshot.thermalZones.append(zoneFromRow(row));

    resolveTemperature(snapshot);
}

void BatteryReader::resolveTemperature(BatterySnapshot &snapshot)
{
    double validSum = 0.0;
    int validCount = 0;
    std::optional<double> batteryZoneTemp;

    for (const ThermalZone &zone : snapshot.thermalZones) {
        if (!zone.valid)
            continue;
        validSum += zone.celsius;
        ++validCount;
        if (zone.isBatteryZone && !batteryZoneTemp.has_value())
            batteryZoneTemp = zone.celsius;
    }

    if (batteryZoneTemp.has_value()) {
        snapshot.temperatureC = batteryZoneTemp;
        snapshot.temperatureIsEstimate = false;
    } else if (validCount > 0) {
        // No battery-specific zone exposed: report the mean of the valid
        // ACPI zones as a system thermal estimate, and say so.
        snapshot.temperatureC = validSum / validCount;
        snapshot.temperatureIsEstimate = true;
    } else if (!snapshot.thermalZones.isEmpty()) {
        snapshot.unavailable << QStringLiteral("MsAcpi_ThermalZoneTemperature: only stub zones present");
    } else {
        snapshot.unavailable << QStringLiteral("MsAcpi_ThermalZoneTemperature: no instances");
    }
}

BatterySnapshot BatteryReader::read()
{
    BatterySnapshot snapshot;
    snapshot.timestamp = QDateTime::currentDateTime();

    mergeRootWmi(snapshot);
    mergeCimv2(snapshot);
    mergeThermal(snapshot);

    snapshot.batteryPresent = snapshotHasRealBattery(snapshot);
    return snapshot;
}

} // namespace faraday
