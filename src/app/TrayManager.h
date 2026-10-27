#pragma once

#include <QObject>

class QSystemTrayIcon;
class QMenu;
class QAction;

namespace faraday {

// System tray icon, menu and balloon notifications.
// Requires a QApplication (widgets); degrades to unavailable otherwise.
class TrayManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    explicit TrayManager(QObject *parent = nullptr);
    ~TrayManager() override;

    bool available() const { return m_available; }

    Q_INVOKABLE void notifyMinimized();

public slots:
    void showAlert(const QString &title, const QString &message);
    void setTooltip(const QString &text);
    // Re-applies the translated menu labels after a live language switch.
    void retranslate();

signals:
    void openRequested();
    void quitRequested();
    void sampleNowRequested();

private:
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_sampleAction = nullptr;
    QAction *m_quitAction = nullptr;
    bool m_available = false;
    bool m_minimizeNoticeShown = false;
};

} // namespace faraday
