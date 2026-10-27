#include "HealthVerdict.h"

#include <QCoreApplication>
#include <QLocale>

namespace faraday {

namespace {

// lupdate files the bare tr() calls made inside HealthVerdict:: members
// under the namespace-qualified class context, so the lookup must use the
// same context or the grade names and verdict text never get translated.
QString tr(const char *text)
{
    return QCoreApplication::translate("faraday::HealthVerdict", text);
}

} // namespace

HealthGrade HealthVerdict::gradeForHealth(std::optional<double> healthPercent)
{
    if (!healthPercent.has_value())
        return HealthGrade::Unknown;
    const double h = *healthPercent;
    if (h >= 90.0)
        return HealthGrade::Excellent;
    if (h >= 80.0)
        return HealthGrade::Good;
    if (h >= 65.0)
        return HealthGrade::Fair;
    if (h >= 50.0)
        return HealthGrade::Worn;
    return HealthGrade::ReplaceSoon;
}

QString HealthVerdict::gradeName(HealthGrade grade)
{
    switch (grade) {
    case HealthGrade::Excellent:   return tr("Excellent");
    case HealthGrade::Good:        return tr("Good");
    case HealthGrade::Fair:        return tr("Fair");
    case HealthGrade::Worn:        return tr("Worn");
    case HealthGrade::ReplaceSoon: return tr("Replace soon");
    case HealthGrade::Unknown:     break;
    }
    return tr("Unknown");
}

Verdict HealthVerdict::generate(const VerdictInput &input)
{
    Verdict verdict;
    verdict.grade = gradeForHealth(input.healthPercent);

    switch (verdict.grade) {
    case HealthGrade::Excellent:
        verdict.headline = tr("Your battery is in excellent shape — it still holds "
                              "nearly all of its original capacity.");
        break;
    case HealthGrade::Good:
        verdict.headline = tr("Your battery is in good shape with only normal wear "
                              "for its age.");
        break;
    case HealthGrade::Fair:
        verdict.headline = tr("Your battery shows noticeable wear; runtime is "
                              "meaningfully shorter than when it was new.");
        break;
    case HealthGrade::Worn:
        verdict.headline = tr("Your battery is significantly worn; expect much "
                              "shorter runtime and plan for a replacement.");
        break;
    case HealthGrade::ReplaceSoon:
        verdict.headline = tr("Your battery has lost more than half of its original "
                              "capacity and should be replaced.");
        break;
    case HealthGrade::Unknown:
        verdict.headline = tr("Not enough data to judge battery health yet.");
        break;
    }

    if (input.healthPercent.has_value()) {
        verdict.details << tr("Current full-charge capacity is %1% of the design capacity.")
                               .arg(QString::number(*input.healthPercent, 'f', 1));
    }

    // Cycle-derived statements require a MEASURED, non-zero cycle count.
    // Zero is the untracked-firmware sentinel (field defect F1): inferring
    // "early in its life" from it contradicted a measured 33% wear on real
    // hardware. Absent or zero => say nothing about cycles at all.
    if (input.cycleCount.has_value() && *input.cycleCount > 0) {
        const bool visiblyWorn =
            input.healthPercent.has_value() && *input.healthPercent < 80.0;
        if (*input.cycleCount < 100) {
            if (visiblyWorn) {
                // Low cycles + real wear is calendar aging — claiming the
                // battery is "early in its life" would contradict the
                // capacity line above it.
                verdict.details << tr("Only %1 charge cycles are recorded, yet capacity "
                                      "has already dropped noticeably — consistent with "
                                      "calendar aging or sustained heat rather than "
                                      "heavy use.")
                                       .arg(*input.cycleCount);
            } else {
                verdict.details << tr("With %1 charge cycles, the battery is still early in "
                                      "its life (most packs are rated for 500–1000 cycles).")
                                       .arg(*input.cycleCount);
            }
        } else if (*input.cycleCount < 500) {
            verdict.details << tr("%1 charge cycles is moderate use; most packs are rated "
                                  "for 500–1000 cycles.")
                                   .arg(*input.cycleCount);
        } else {
            verdict.details << tr("%1 charge cycles is high; capacity loss will "
                                  "accelerate from here.")
                                   .arg(*input.cycleCount);
        }
    }

    if (input.temperatureC.has_value() && *input.temperatureC > 45.0) {
        verdict.details << tr("The system is running warm (%1°C); sustained heat is the "
                              "biggest single cause of battery ageing.")
                               .arg(QString::number(*input.temperatureC, 'f', 0));
    }

    if (input.calibrationDriftPercent.has_value()
        && qAbs(*input.calibrationDriftPercent) > 5.0) {
        verdict.details << tr("The charge gauge disagrees with the measured energy by "
                              "%1%; a calibration cycle would make estimates more accurate.")
                               .arg(QString::number(qAbs(*input.calibrationDriftPercent), 'f', 1));
    }

    if (input.degradation.valid && input.degradation.slope < 0.0) {
        const double mWhPerMonth = -input.degradation.slope * 30.44;
        if (mWhPerMonth >= 1.0) {
            verdict.details << tr("Capacity is declining by roughly %1 mWh per month.")
                                   .arg(QString::number(mWhPerMonth, 'f', 0));
        }
    }

    if (input.endOfLife.has_value()) {
        verdict.details << tr("At the current rate of wear the battery will reach its "
                              "service threshold around %1.")
                               .arg(QLocale().toString(*input.endOfLife, QStringLiteral("MMMM yyyy")));
    }

    return verdict;
}

QStringList HealthVerdict::chargingHabitInsights(
    const QList<PowercfgReportData::UsageEntry> &usage)
{
    QStringList insights;
    if (usage.isEmpty())
        return insights;

    qint64 totalSec = 0;
    qint64 acSec = 0;
    qint64 highChargeAcSec = 0; // plugged in while >= 95 %
    int deepDischargeEntries = 0;
    int dcEntries = 0;

    for (const auto &entry : usage) {
        const qint64 durationSec = entry.duration100ns / 10000000ll;
        if (durationSec <= 0)
            continue;
        totalSec += durationSec;
        std::optional<double> pct;
        if (entry.chargeCapacitymWh.has_value() && entry.fullChargeCapacitymWh.has_value()
            && *entry.fullChargeCapacitymWh > 0) {
            pct = 100.0 * static_cast<double>(*entry.chargeCapacitymWh)
                        / static_cast<double>(*entry.fullChargeCapacitymWh);
        }
        if (entry.ac) {
            acSec += durationSec;
            if (pct.has_value() && *pct >= 95.0)
                highChargeAcSec += durationSec;
        } else {
            ++dcEntries;
            if (pct.has_value() && *pct <= 15.0)
                ++deepDischargeEntries;
        }
    }

    if (totalSec <= 0)
        return insights;

    const double acShare = 100.0 * static_cast<double>(acSec) / static_cast<double>(totalSec);
    insights << QCoreApplication::translate("HealthVerdict",
                    "Over the recorded period the machine was plugged in %1% of the time.")
                    .arg(QString::number(acShare, 'f', 0));

    const double highDwellShare =
        100.0 * static_cast<double>(highChargeAcSec) / static_cast<double>(totalSec);
    if (highDwellShare > 40.0) {
        insights << QCoreApplication::translate("HealthVerdict",
                        "The battery sits at or near full charge for long stretches while "
                        "plugged in; if your firmware offers a charge cap (e.g. 80%), "
                        "enabling it would slow wear.");
    }

    if (dcEntries > 0 && deepDischargeEntries * 4 >= dcEntries) {
        insights << QCoreApplication::translate("HealthVerdict",
                        "Deep discharges below 15% happen regularly; lithium batteries "
                        "last longer when recharged before dropping that low.");
    }

    return insights;
}

} // namespace faraday
