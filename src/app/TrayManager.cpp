#include "TrayManager.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSystemTrayIcon>

namespace faraday {

namespace {

// The brand mark, with a runtime-drawn battery glyph as a fallback for
// builds where the icon resource is unavailable (e.g. test binaries that
// link the library without the QML/resource module).
QIcon makeTrayIcon()
{
    // The multi-resolution ICO: the tray renders at 16/24/32 px, where the
    // purpose-drawn simplified mark reads and the artwork would not.
    const QIcon brand(QStringLiteral(":/icons/faraday.ico"));
    if (!brand.isNull())
        return brand;

    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);

    // Battery body
    p.setPen(QPen(QColor("#e6edf3"), 4));
    p.setBrush(QColor(22, 27, 34, 200));
    p.drawRoundedRect(QRectF(6, 18, 44, 30), 6, 6);
    // Terminal nub
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#e6edf3"));
    p.drawRoundedRect(QRectF(52, 27, 7, 12), 2, 2);
    // Lightning bolt
    QPainterPath bolt;
    bolt.moveTo(31, 20);
    bolt.lineTo(20, 36);
    bolt.lineTo(27, 36);
    bolt.lineTo(24, 47);
    bolt.lineTo(36, 31);
    bolt.lineTo(29, 31);
    bolt.closeSubpath();
    p.setBrush(QColor("#58a6ff"));
    p.drawPath(bolt);
    p.end();

    return QIcon(pixmap);
}

} // namespace

TrayManager::TrayManager(QObject *parent)
    : QObject(parent)
{
    if (!qobject_cast<QApplication *>(QCoreApplication::instance()))
        return; // no widgets application: tray unavailable
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_tray = new QSystemTrayIcon(makeTrayIcon(), this);
    m_menu = new QMenu();

    m_openAction = m_menu->addAction(QString());
    connect(m_openAction, &QAction::triggered, this, &TrayManager::openRequested);
    m_sampleAction = m_menu->addAction(QString());
    connect(m_sampleAction, &QAction::triggered, this, &TrayManager::sampleNowRequested);
    m_menu->addSeparator();
    m_quitAction = m_menu->addAction(QString());
    connect(m_quitAction, &QAction::triggered, this, &TrayManager::quitRequested);

    m_tray->setContextMenu(m_menu);
    retranslate();
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick) {
                    emit openRequested();
                }
            });
    m_tray->show();
    m_available = true;
}

void TrayManager::retranslate()
{
    if (!m_tray)
        return;
    m_openAction->setText(tr("Open Faraday"));
    m_sampleAction->setText(tr("Sample now"));
    m_quitAction->setText(tr("Quit"));
    m_tray->setToolTip(tr("Faraday — Battery Intelligence Suite"));
}

TrayManager::~TrayManager()
{
    delete m_menu;
}

void TrayManager::notifyMinimized()
{
    if (!m_available || m_minimizeNoticeShown)
        return;
    m_minimizeNoticeShown = true;
    m_tray->showMessage(tr("Faraday keeps running"),
                        tr("Monitoring continues in the tray. "
                           "Right-click the icon to quit."),
                        QSystemTrayIcon::Information, 4000);
}

void TrayManager::showAlert(const QString &title, const QString &message)
{
    if (m_available)
        m_tray->showMessage(title, message, QSystemTrayIcon::Warning, 8000);
}

void TrayManager::setTooltip(const QString &text)
{
    if (m_available)
        m_tray->setToolTip(text);
}

} // namespace faraday
