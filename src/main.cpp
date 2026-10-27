#include "app/BatteryModel.h"
#include "app/PortableMode.h"
#include "app/TrayManager.h"

#include <QApplication>
#include <QIcon>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTranslator>

int main(int argc, char *argv[])
{
    // QApplication (not QGuiApplication) so QSystemTrayIcon works; the UI
    // itself is pure Qt Quick.
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Faraday"));
    app.setOrganizationName(QStringLiteral("Faraday Project"));
    app.setApplicationVersion(QStringLiteral("1.0.6"));
    app.setQuitOnLastWindowClosed(false); // tray keeps us alive
    // Window + taskbar icon: the multi-resolution ICO, so Windows picks the
    // purpose-drawn 16/24/32 px marks for the title bar and taskbar rather
    // than rescaling one large PNG into mush.
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/faraday.ico")));

    faraday::BatteryModel model;
    // Portable ZIP: a portable.txt marker next to the exe keeps all data
    // in <exeDir>\data so the app travels with its folder (USB included).
    // Empty means the default per-user location.
    model.initialize(
        faraday::PortableMode::dataDirFor(QCoreApplication::applicationDirPath()));

    // English is the source language; other languages load from the
    // embedded :/i18n resources. Right-to-left languages (Arabic) also
    // mirror the layout. Switching language in Settings applies live.
    QTranslator translator;
    const QLocale systemLocale; // restored when switching back to English
    const auto applyLanguage = [&](const QString &language) {
        QCoreApplication::removeTranslator(&translator);
        const bool translated = language != QLatin1String("en")
            && translator.load(QStringLiteral(":/i18n/faraday_%1").arg(language));
        // Arabic uses the ar_AE locale: Arabic month names with Western
        // digits, so spin boxes and dates match the numbers the rest of the
        // UI prints (plain "ar" would mix in Arabic-Indic digits).
        const QLocale uiLocale = language == QLatin1String("ar")
            ? QLocale(QLocale::Arabic, QLocale::UnitedArabEmirates)
            : QLocale(language);
        if (translated) {
            QCoreApplication::installTranslator(&translator);
            QLocale::setDefault(uiLocale);
        } else {
            QLocale::setDefault(systemLocale);
        }
        app.setLayoutDirection(translated && uiLocale.textDirection() == Qt::RightToLeft
                                   ? Qt::RightToLeft
                                   : Qt::LeftToRight);
    };
    applyLanguage(model.settings()->language());

    faraday::TrayManager tray;
    QObject::connect(&model, &faraday::BatteryModel::alertRaised,
                     &tray, &faraday::TrayManager::showAlert);
    QObject::connect(&tray, &faraday::TrayManager::quitRequested,
                     &app, &QCoreApplication::quit);
    QObject::connect(&tray, &faraday::TrayManager::sampleNowRequested,
                     &model, &faraday::BatteryModel::sampleNow);
    QObject::connect(&model, &faraday::BatteryModel::snapshotChanged, &tray, [&]() {
        if (model.batteryPresent() && model.chargePercent() >= 0) {
            tray.setTooltip(QCoreApplication::translate("main", "Faraday — %1% · %2")
                                .arg(QString::number(model.chargePercent(), 'f', 0),
                                     model.statusText()));
        }
    });

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("battery"), &model);
    engine.rootContext()->setContextProperty(QStringLiteral("tray"), &tray);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Faraday", "Main");

    QObject::connect(&model, &faraday::BatteryModel::languageChanged, &app, [&]() {
        applyLanguage(model.language());
        tray.retranslate();
        engine.retranslate();
    });

    // objectCreationFailed does NOT fire when a QML module *plugin* fails to
    // load (e.g. a missing Qt DLL in a broken deployment) — the engine just
    // produces no root objects. Without this check the tray-driven event
    // loop would keep a windowless zombie process alive. Verified against a
    // deliberately broken dist.
    if (engine.rootObjects().isEmpty())
        return 1;

    // Without a tray icon, closing the window must quit the app.
    if (!tray.available())
        app.setQuitOnLastWindowClosed(true);

    return app.exec();
}
