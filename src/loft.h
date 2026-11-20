#pragma once

#include "core.h"
#include "runtimes.h"

#include <QLockFile>
#include <memory>

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

struct Result {
    bool ok = true;
    QString message;
    static Result fail(const QString &message) { return {false, message}; }
    static Result done(const QString &message = {}) { return {true, message}; }
};

// The PHP and Node a folder runs, and why: a site's isolation, a project's
// .nvmrc or .node-version (Node only, when that major is installed), or the
// default. Mirrors what the php and node shims pick.
struct RuntimeChoice {
    QString site;                 // the site holding the folder, if any
    QString php, phpVersion, phpBinary, phpFrom;     // phpFrom: "site" or "default"
    QString node, nodeVersion, nodeBinary, nodeFrom; // nodeFrom: "site", ".nvmrc", ".node-version" or "default"
};

struct Check {
    QString id;
    QString label;
    bool ok = false;
    QString detail;
    bool needsSetup = false; // loft setup fixes it
};

// Everything Loft does, for the command line and the window alike. Each
// change saves the config and brings generated files and services in line.
class Loft {
public:
    // Tests pass manageServices = false: files are written, systemd and
    // NetworkManager are left alone.
    explicit Loft(const Paths &paths = Paths::current(), bool manageServices = true);

    const Paths &paths() const { return m_paths; }
    const Config &config() const { return m_config; }
    void reload();

    QList<Site> sites() const;
    std::optional<Site> site(const QString &name) const;
    std::optional<Site> siteAt(const QString &directory) const;
    // The site a command means: the name given, else the one holding the
    // current directory.
    std::optional<Site> target(const QString &name, QString *error) const;

    QMap<QString, QString> installedPhp() const;
    QMap<QString, QString> installedNode() const;
    RuntimeChoice runtimeFor(const QString &directory) const;

    // One change at a time across every loft process (the window, the bar
    // and agents all run loft too), each starting from the config as the
    // last one left it. Null when another holds it for too long.
    std::unique_ptr<QLockFile> lock(int timeoutMs = 120000);

    // The versions on offer, from the cache when it's under six hours old.
    Catalog catalog(bool refresh, QString *error) const;
    // Downloads, checks and installs the newest release of a line ("8.4",
    // "24"), or does nothing when it's already installed and force is off.
    // Locks for the final step itself, so call it unlocked.
    Result install(const QString &tool, const QString &line, bool force,
                   const std::function<void(const QString &label)> &starting = {},
                   const Runtimes::Progress &progress = {});

    Result link(const QString &directory, QString name, bool secure, const QString &php);
    Result unlink(const QString &name);
    Result park(const QString &directory);
    Result forget(const QString &directory);
    Result secure(const QString &name, bool on);
    // tool is "php" or "node"; an empty version follows the default again.
    Result isolate(const QString &name, const QString &tool, const QString &version);
    Result useDefault(const QString &tool, const QString &version);
    Result proxy(const QString &name, const QString &url, bool secure);
    Result unproxy(const QString &name);
    Result setCheckUpdates(bool on);
    Result setPhpBuild(const QString &build);

    // After installing or removing a runtime.
    Result installedRuntime(const QString &tool, const QString &line);
    Result uninstallRuntime(const QString &tool, const QString &line);
    Result restartPhp(const QString &minor);

    // Copies the site's generated nginx config to ~/.config/loft/nginx the
    // first time, so it can be edited; Loft serves that file from then on.
    Result nginxEdit(const QString &name, QString *file);
    Result nginxReset(const QString &name);

    // Writes nginx, PHP-FPM, shims and units, then starts, reloads or stops
    // services to match.
    Result apply();

    QList<Check> doctor() const;
    using Step = std::function<void(const QString &label, bool ok, const QString &detail)>;
    // Every one-time piece: CA, DNS, shell, services, Composer, and the root
    // part through pkexec when askRoot is set.
    Result setup(bool askRoot, const Step &step);
    Result uninstall(bool purge, const Step &step);

private:
    Result commit(const QString &message);

    Paths m_paths;
    Config m_config;
    bool m_services;
};
