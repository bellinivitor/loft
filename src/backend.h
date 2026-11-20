#pragma once

#include "loft.h"
#include "portal.h"
#include "runtimes.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

// The window's view of Loft. It reads state straight from disk, and every
// change runs the loft command in the background, so the window never
// blocks and behaves exactly like the terminal.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sites READ sites NOTIFY changed)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY changed)
    Q_PROPERTY(QVariantList php READ php NOTIFY changed)
    Q_PROPERTY(QVariantList node READ node NOTIFY changed)
    Q_PROPERTY(int updateCount READ updateCount NOTIFY changed)
    Q_PROPERTY(QString catalogFetched READ catalogFetched NOTIFY changed)
    Q_PROPERTY(bool catalogLoading READ catalogLoading NOTIFY changed)
    Q_PROPERTY(QVariantList checks READ checks NOTIFY checksChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checksChanged)
    Q_PROPERTY(bool setupNeeded READ setupNeeded NOTIFY checksChanged)
    Q_PROPERTY(bool settingUp READ settingUp NOTIFY checksChanged)
    Q_PROPERTY(QVariantMap services READ services NOTIFY servicesChanged)
    Q_PROPERTY(QVariantMap jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(bool working READ working NOTIFY jobsChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    QVariantList sites() const { return m_sites; }
    QVariantMap settings() const;
    QVariantList php() const { return runtimeRows("php"); }
    QVariantList node() const { return runtimeRows("node"); }
    int updateCount() const;
    QString catalogFetched() const;
    bool catalogLoading() const { return m_catalogLoading; }
    QVariantList checks() const { return m_checks; }
    bool checking() const { return m_checking; }
    bool setupNeeded() const;
    bool settingUp() const { return m_settingUp; }
    QVariantMap services() const { return m_services; }
    QVariantMap jobs() const { return m_jobs; }
    bool working() const { return m_quick != nullptr || !m_queue.isEmpty(); }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void runDoctor();
    Q_INVOKABLE void runSetup();
    Q_INVOKABLE void checkUpdates();
    Q_INVOKABLE void refreshCatalog();

    Q_INVOKABLE void link(const QString &path, const QString &name, bool secure, const QString &php);
    Q_INVOKABLE void unlink(const QString &name);
    Q_INVOKABLE void setSecure(const QString &name, bool on);
    Q_INVOKABLE void isolate(const QString &name, const QString &tool, const QString &line);
    Q_INVOKABLE void proxy(const QString &name, const QString &url, bool secure);
    Q_INVOKABLE void unproxy(const QString &name);
    Q_INVOKABLE void park(const QString &path);
    Q_INVOKABLE void forget(const QString &path);

    Q_INVOKABLE void install(const QString &tool, const QString &line);
    Q_INVOKABLE void uninstall(const QString &tool, const QString &line);
    Q_INVOKABLE void makeDefault(const QString &tool, const QString &line);
    Q_INVOKABLE void restartPhp(const QString &line);
    Q_INVOKABLE void restartServices();
    Q_INVOKABLE void setCheckUpdates(bool on);
    Q_INVOKABLE void setPhpBuild(const QString &build);

    Q_INVOKABLE QString suggestName(const QString &path) const;
    Q_INVOKABLE void pickFolder(const QString &purpose, const QString &title);
    Q_INVOKABLE void openUrl(const QString &url);
    Q_INVOKABLE void openFolder(const QString &path);
    Q_INVOKABLE void openPhpIni(const QString &line);
    Q_INVOKABLE void copy(const QString &text);
    Q_INVOKABLE void editNginx(const QString &name);
    Q_INVOKABLE void resetNginx(const QString &name);

    // Brings the window forward on a page ("sites", "php", "node", "settings").
    void activate(const QString &page);

signals:
    void changed();
    void checksChanged();
    void servicesChanged();
    void jobsChanged();
    void notice(const QString &message, bool error);
    void folderPicked(const QString &purpose, const QString &path);
    void activated(const QString &page);

private:
    void command(const QStringList &arguments, const QString &directory = {});
    void next();
    void reloadState();
    void pollServices();
    void watch();
    QVariantList runtimeRows(const QString &tool) const;
    static QString lastLine(const QString &text);

    Loft m_loft;
    Catalog m_catalog;
    QVariantList m_sites;
    QVariantList m_checks;
    QVariantMap m_services;
    QVariantMap m_jobs;
    bool m_checking = false;
    bool m_settingUp = false;
    bool m_catalogLoading = false;

    QQueue<QPair<QStringList, QString>> m_queue;
    QProcess *m_quick = nullptr;
    QHash<QString, QProcess *> m_installs;
    QProcess m_servicesProcess;
    QTimer m_servicesTimer;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    FolderPicker m_picker;
    QString m_pickPurpose;
};
