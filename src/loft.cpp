#include "loft.h"
#include "certs.h"
#include "runtimes.h"
#include "system.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QLocalSocket>
#include <QSet>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("loft", text); }
QString tr(const char *text, int n) { return QCoreApplication::translate("loft", text, nullptr, n); }

// "php@8.3.12" and "8.3.12" both mean the 8.3 line; "node@24.1" means 24.
QString lineOf(const QString &tool, QString version) {
    if (version.startsWith(tool + "@"))
        version = version.mid(tool.length() + 1);
    if (version.startsWith('v'))
        version = version.mid(1);
    return tool == "php" ? version.section('.', 0, 1) : version.section('.', 0, 0);
}

// nginx reloads in the background: the command returns before new ports
// are bound. Wait for them, so a request right after loft secure works.
void waitForPort(quint16 port, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        if (socket.waitForConnected(50))
            return;
        QThread::msleep(50);
    }
}

// systemd reports PHP-FPM started before it has made its socket.
void waitForSocket(const QString &path, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        QLocalSocket socket;
        socket.connectToServer(path);
        if (socket.waitForConnected(50))
            return;
        QThread::msleep(50);
    }
}

QString procText(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// nginx's workers that still take new connections: children of the main
// process that aren't shutting down after a reload.
QSet<int> nginxWorkers() {
    QString output;
    System::systemctl({"show", "-p", "MainPID", "--value", "loft-nginx.service"}, &output);
    const int main = output.trimmed().toInt();
    QSet<int> workers;
    if (main <= 0)
        return workers;
    const auto entries = QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool numeric = false;
        const int pid = entry.toInt(&numeric);
        if (!numeric)
            continue;
        const QString status = procText("/proc/" + entry + "/status");
        const int at = status.indexOf("\nPPid:");
        if (at < 0 || status.mid(at + 7, status.indexOf('\n', at + 1) - at - 7).trimmed().toInt() != main)
            continue;
        const QString command = procText("/proc/" + entry + "/cmdline");
        if (command.contains("worker process") && !command.contains("shutting down"))
            workers << pid;
    }
    return workers;
}

// A reload returns as soon as nginx got the signal; the old workers keep
// answering with the old configuration until new ones replace them.
void waitForNewWorkers(const QSet<int> &old, int timeoutMs) {
    if (old.isEmpty())
        return;
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        const QSet<int> now = nginxWorkers();
        if (!now.isEmpty() && !now.intersects(old))
            return;
        QThread::msleep(50);
    }
}

QString defaultPage(const QString &tld) {
    return QString(R"(<!doctype html>
<html><head><meta charset="utf-8"><title>Loft</title>
<style>
  body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #1e1f24;
         color: #c9cbd3; font: 16px/1.6 ui-monospace, monospace; }
  main { max-width: 34rem; padding: 2rem; }
  h1 { color: #fff; font-size: 1.4rem; margin: 0 0 1rem; }
  code { color: #FFD60A; }
</style></head>
<body><main>
  <h1>%1</h1>
  <p>%2</p>
  <p><code>cd ~/my-project &amp;&amp; loft link</code></p>
</main></body></html>
)").arg(tr("Nothing is served here yet."),
         tr("Link a project folder and it becomes <em>folder-name</em>.%1:").arg(tld.toHtmlEscaped()));
}

} // namespace

Loft::Loft(const Paths &paths, bool manageServices) : m_paths(paths), m_services(manageServices) {
    reload();
}

void Loft::reload() { m_config = Config::load(m_paths.configFile()); }

QList<Site> Loft::sites() const {
    QList<Site> all = resolveSites(m_config);
    for (Site &site : all)
        site.customNginx = QFileInfo::exists(m_paths.nginxOverride(site.host));
    return all;
}

std::optional<Site> Loft::site(const QString &name) const {
    for (const Site &site : sites())
        if (site.name == name || site.host == name)
            return site;
    return std::nullopt;
}

std::optional<Site> Loft::siteAt(const QString &directory) const {
    const QList<Site> all = sites();
    if (const Site *found = siteContaining(all, directory))
        return *found;
    return std::nullopt;
}

std::optional<Site> Loft::target(const QString &name, QString *error) const {
    auto found = name.isEmpty() ? siteAt(QDir::currentPath()) : site(name);
    if (!found && error)
        *error = name.isEmpty() ? tr("This folder isn't a Loft site. Link it with: loft link")
                                : tr("There's no site called %1. See them with: loft links").arg(name);
    return found;
}

QMap<QString, QString> Loft::installedPhp() const { return Runtimes::installedPhp(m_paths); }
QMap<QString, QString> Loft::installedNode() const { return Runtimes::installedNode(m_paths); }

RuntimeChoice Loft::runtimeFor(const QString &directory) const {
    RuntimeChoice choice;
    const auto found = siteAt(directory);
    if (found)
        choice.site = found->name;

    const auto php = installedPhp();
    choice.phpFrom = found && !found->php.isEmpty() ? "site" : "default";
    choice.php = choice.phpFrom == "site" ? found->php : m_config.php;
    if (php.contains(choice.php)) {
        choice.phpVersion = php.value(choice.php);
        choice.phpBinary = m_paths.phpDir(choice.php) + "/php";
    }

    const auto node = installedNode();
    choice.nodeFrom = "default";
    choice.node = m_config.node;
    if (found && !found->node.isEmpty()) {
        choice.nodeFrom = "site";
        choice.node = found->node;
    } else {
        // The nearest .nvmrc or .node-version wins when Loft has that major.
        QDir dir(QDir(directory).absolutePath());
        bool searching = true;
        while (searching) {
            for (const QString &file : {QString(".nvmrc"), QString(".node-version")}) {
                QFile pin(dir.filePath(file));
                if (!pin.open(QIODevice::ReadOnly))
                    continue;
                QString wanted = QString::fromUtf8(pin.readLine()).trimmed().remove(' ').remove('\t');
                if (wanted.startsWith('v') || wanted.startsWith('V'))
                    wanted = wanted.mid(1);
                wanted = wanted.section('.', 0, 0);
                if (node.contains(wanted)) {
                    choice.node = wanted;
                    choice.nodeFrom = file;
                }
                searching = false;
                break;
            }
            if (searching && !dir.cdUp())
                searching = false;
        }
    }
    if (node.contains(choice.node)) {
        choice.nodeVersion = node.value(choice.node);
        choice.nodeBinary = m_paths.nodeDir(choice.node) + "/bin/node";
    }
    return choice;
}

std::unique_ptr<QLockFile> Loft::lock(int timeoutMs) {
    QDir().mkpath(m_paths.state);
    auto file = std::make_unique<QLockFile>(m_paths.state + "/lock");
    // Never stale by age; QLockFile still takes over from a process that died.
    file->setStaleLockTime(0);
    if (!file->tryLock(timeoutMs))
        return nullptr;
    reload();
    return file;
}

Catalog Loft::catalog(bool refresh, QString *error) const {
    Catalog cached = Runtimes::cachedCatalog(m_paths);
    if (!refresh && cached.fetched.isValid() && cached.fetched.secsTo(QDateTime::currentDateTimeUtc()) < 6 * 3600)
        return cached;
    Catalog fresh = Runtimes::fetchCatalog(m_config.phpBuild, error);
    if (fresh.php.isEmpty())
        return cached;
    Runtimes::saveCatalog(m_paths, fresh);
    return fresh;
}

Result Loft::install(const QString &tool, const QString &requested, bool force,
                     const std::function<void(const QString &label)> &starting,
                     const Runtimes::Progress &progress) {
    QString line = requested;
    if (line.startsWith(tool + "@"))
        line = line.mid(tool.length() + 1);
    if (line.startsWith('v'))
        line = line.mid(1);
    line = tool == "php" ? line.section('.', 0, 1) : line.section('.', 0, 0);
    QString error;
    const Catalog list = catalog(true, &error);
    if (tool == "php") {
        if (!list.php.contains(line))
            return Result::fail(error.isEmpty() ? tr("There's no PHP %1 build. Available: %2")
                                                      .arg(line, QStringList(list.php.keys()).join(", "))
                                                : error);
        const PhpRelease release = list.php.value(line);
        if (installedPhp().value(line) == release.version && !force)
            return Result::done(tr("PHP %1 is already the newest").arg(release.version));
        if (starting)
            starting(tr("Downloading PHP %1…").arg(release.version));
        if (!Runtimes::installPhp(m_paths, release, m_config.phpBuild, progress, &error))
            return Result::fail(error);
    } else if (tool == "node") {
        const NodeRelease *release = list.nodeRelease(line);
        if (!release) {
            QStringList majors;
            for (const NodeRelease &node : list.node)
                majors << node.major;
            return Result::fail(error.isEmpty()
                                    ? tr("There's no Node %1 release. Available: %2").arg(line, majors.join(", "))
                                    : error);
        }
        if (installedNode().value(line) == release->version && !force)
            return Result::done(tr("Node %1 is already the newest").arg(release->version));
        if (starting)
            starting(tr("Downloading Node %1…").arg(release->version));
        if (!Runtimes::installNode(m_paths, *release, progress, &error))
            return Result::fail(error);
    } else {
        return Result::fail(tr("The tool is either php or node"));
    }
    const auto held = lock();
    if (!held)
        return Result::fail(tr("Another loft command is still running"));
    return installedRuntime(tool, line);
}

Result Loft::commit(const QString &message) {
    QString error;
    if (!m_config.save(m_paths.configFile(), &error))
        return Result::fail(tr("Can't save %1: %2").arg(m_paths.configFile(), error));
    const Result applied = apply();
    if (!applied.ok)
        return Result::fail(message + "\n" + applied.message);
    return Result::done(message);
}

Result Loft::link(const QString &directory, QString name, bool secure, const QString &php) {
    const QString folder = QDir::cleanPath(QDir(directory).absolutePath());
    if (!QFileInfo(folder).isDir())
        return Result::fail(tr("%1 isn't a folder").arg(folder));
    if (folder.contains('$') || folder.contains('\n'))
        return Result::fail(tr("Loft can't serve a folder whose path has $ or a line break"));
    if (name.isEmpty())
        name = siteName(QFileInfo(folder).fileName());
    name = name.toLower();
    if (name.endsWith("." + m_config.tld))
        name.chop(m_config.tld.length() + 1);
    if (!validSiteName(name))
        return Result::fail(tr("%1 can't be a site name: use letters, digits and dashes").arg(name));
    if (m_config.proxies.contains(name))
        return Result::fail(tr("%1 is already a proxy. Remove it with: loft unproxy %1").arg(name));
    const QString minor = php.isEmpty() ? QString() : lineOf("php", php);
    if (!minor.isEmpty() && !installedPhp().contains(minor))
        return Result::fail(tr("PHP %1 isn't installed. Install it with: loft php:install %1").arg(minor));

    m_config.links.insert(name, folder);
    SiteSettings &settings = m_config.sites[name];
    settings.secure = settings.secure || secure;
    if (!minor.isEmpty())
        settings.php = minor;
    const auto linked = resolveSites(m_config);
    QString url = "http://" + name + "." + m_config.tld;
    for (const Site &site : linked)
        if (site.name == name)
            url = site.url();
    return commit(tr("%1 now serves %2").arg(url, folder));
}

Result Loft::unlink(const QString &name) {
    if (!m_config.links.contains(name)) {
        const auto found = site(name);
        if (found && found->source == "parked")
            return Result::fail(tr("%1 comes from a parked folder. Move the folder out of it, or stop parking it "
                                   "with: loft forget %2").arg(found->host, QFileInfo(found->path).absolutePath()));
        if (found && found->source == "proxy")
            return Result::fail(tr("%1 is a proxy. Remove it with: loft unproxy %2").arg(found->host, name));
        return Result::fail(tr("There's no linked site called %1").arg(name));
    }
    m_config.links.remove(name);
    m_config.sites.remove(name);
    Certs::removeSite(m_paths, name + "." + m_config.tld);
    QFile::remove(m_paths.nginxOverride(name + "." + m_config.tld));
    return commit(tr("Unlinked %1").arg(name + "." + m_config.tld));
}

Result Loft::park(const QString &directory) {
    const QString folder = QDir::cleanPath(QDir(directory).absolutePath());
    if (!QFileInfo(folder).isDir())
        return Result::fail(tr("%1 isn't a folder").arg(folder));
    if (folder.contains('$') || folder.contains('\n'))
        return Result::fail(tr("Loft can't serve a folder whose path has $ or a line break"));
    if (m_config.parked.contains(folder))
        return Result::done(tr("%1 is already parked").arg(folder));
    m_config.parked.append(folder);
    return commit(tr("Every folder in %1 is now a site at <folder>.%2").arg(folder, m_config.tld));
}

Result Loft::forget(const QString &directory) {
    const QString folder = QDir::cleanPath(QDir(directory).absolutePath());
    if (!m_config.parked.removeAll(folder))
        return Result::fail(tr("%1 isn't parked").arg(folder));
    return commit(tr("Stopped parking %1").arg(folder));
}

Result Loft::secure(const QString &name, bool on) {
    QString error;
    const auto found = target(name, &error);
    if (!found)
        return Result::fail(error);
    m_config.sites[found->name].secure = on;
    if (!on)
        Certs::removeSite(m_paths, found->host);
    Result result = commit(on ? tr("%1 is now served over HTTPS").arg("https://" + found->host)
                              : tr("%1 is back to plain HTTP").arg("http://" + found->host));
    if (result.ok && found->customNginx)
        result.message += "\n" + tr("Its nginx config is your own, so this only takes effect after: loft nginx:reset %1")
                                      .arg(found->name);
    return result;
}

Result Loft::isolate(const QString &name, const QString &tool, const QString &version) {
    QString error;
    const auto found = target(name, &error);
    if (!found)
        return Result::fail(error);
    if (found->kind == "proxy")
        return Result::fail(tr("%1 is a proxy; it runs whatever you started yourself").arg(found->host));
    const QString line = version.isEmpty() ? QString() : lineOf(tool, version);
    const auto installed = tool == "php" ? installedPhp() : installedNode();
    const QString label = tool == "php" ? "PHP" : "Node";
    if (!line.isEmpty() && !installed.contains(line))
        return Result::fail(tr("%1 %2 isn't installed. Install it with: loft %3:install %2").arg(label, line, tool));

    SiteSettings &settings = m_config.sites[found->name];
    (tool == "php" ? settings.php : settings.node) = line;
    const QString fallback = tool == "php" ? m_config.php : m_config.node;
    Result result = commit(line.isEmpty()
                               ? tr("%1 follows the default %2 again (%3)").arg(found->host, label, fallback)
                               : tr("%1 now uses %2 %3").arg(found->host, label, line));
    if (result.ok && tool == "php" && found->customNginx)
        result.message += "\n" + tr("Its nginx config is your own, so this only takes effect after: loft nginx:reset %1")
                                      .arg(found->name);
    return result;
}

Result Loft::useDefault(const QString &tool, const QString &version) {
    const QString line = lineOf(tool, version);
    const QString label = tool == "php" ? "PHP" : "Node";
    const auto installed = tool == "php" ? installedPhp() : installedNode();
    if (!installed.contains(line))
        return Result::fail(tr("%1 %2 isn't installed. Install it with: loft %3:install %2").arg(label, line, tool));
    (tool == "php" ? m_config.php : m_config.node) = line;
    return commit(tr("%1 %2 is now the default").arg(label, installed.value(line)));
}

Result Loft::proxy(const QString &name, const QString &url, bool secure) {
    const QString site = siteName(name);
    if (!validSiteName(site) || site != name.toLower())
        return Result::fail(tr("%1 can't be a site name: use letters, digits and dashes").arg(name));
    const QUrl upstream(url.contains("://") ? url : "http://" + url, QUrl::StrictMode);
    static const QString unsafe = " \t\n;{}\"'$\\";
    bool clean = upstream.isValid() && !upstream.host().isEmpty()
              && (upstream.scheme() == "http" || upstream.scheme() == "https");
    for (const QChar c : upstream.toString())
        clean = clean && !unsafe.contains(c);
    if (!clean)
        return Result::fail(tr("%1 isn't an http:// address Loft can proxy to").arg(url));
    if (m_config.links.contains(site))
        return Result::fail(tr("%1 is already a linked folder. Unlink it first: loft unlink %1").arg(site));
    m_config.proxies.insert(site, upstream.toString());
    if (secure)
        m_config.sites[site].secure = true;
    const QString host = site + "." + m_config.tld;
    return commit(tr("%1 now forwards to %2").arg((m_config.sites.value(site).secure ? "https://" : "http://") + host,
                                                  upstream.toString()));
}

Result Loft::unproxy(const QString &name) {
    if (!m_config.proxies.remove(name))
        return Result::fail(tr("There's no proxy called %1").arg(name));
    m_config.sites.remove(name);
    Certs::removeSite(m_paths, name + "." + m_config.tld);
    QFile::remove(m_paths.nginxOverride(name + "." + m_config.tld));
    return commit(tr("Removed the proxy %1").arg(name + "." + m_config.tld));
}

Result Loft::setCheckUpdates(bool on) {
    m_config.checkUpdates = on;
    return commit(on ? tr("Loft checks for new PHP and Node releases daily")
                     : tr("Loft no longer checks for new releases"));
}

Result Loft::setPhpBuild(const QString &build) {
    if (build != "bulk" && build != "common")
        return Result::fail(tr("The PHP build is either bulk or common"));
    m_config.phpBuild = build;
    QFile::remove(m_paths.catalogFile());
    return commit(tr("New PHP installs use the %1 build").arg(build));
}

Result Loft::installedRuntime(const QString &tool, const QString &line) {
    QString &fallback = tool == "php" ? m_config.php : m_config.node;
    const auto installed = tool == "php" ? installedPhp() : installedNode();
    if (fallback.isEmpty() || !installed.contains(fallback))
        fallback = line;
    if (tool == "php" && m_services && System::enabledPhpVersions(m_paths).contains(line))
        System::systemctl({"restart", "loft-php@" + line + ".service"});
    const QString label = tool == "php" ? "PHP" : "Node";
    return commit(tr("%1 %2 is installed").arg(label, installed.value(line, line)));
}

Result Loft::uninstallRuntime(const QString &tool, const QString &line) {
    const auto installed = tool == "php" ? installedPhp() : installedNode();
    const QString label = tool == "php" ? "PHP" : "Node";
    if (!installed.contains(line))
        return Result::fail(tr("%1 %2 isn't installed").arg(label, line));
    QString &fallback = tool == "php" ? m_config.php : m_config.node;
    if (fallback == line && installed.size() > 1)
        return Result::fail(tr("%1 %2 is the default. Make another version the default first.").arg(label, line));
    QStringList isolated;
    for (auto it = m_config.sites.begin(); it != m_config.sites.end(); ++it)
        if ((tool == "php" ? it->php : it->node) == line)
            isolated << it.key();
    if (!isolated.isEmpty())
        return Result::fail(tr("These sites use %1 %2: %3. Move them to another version first.")
                                .arg(label, line, isolated.join(", ")));

    if (tool == "php") {
        if (m_services)
            System::systemctl({"disable", "--now", "loft-php@" + line + ".service"});
        Runtimes::removePhp(m_paths, line);
    } else {
        Runtimes::removeNode(m_paths, line);
    }
    if (fallback == line)
        fallback.clear();
    return commit(tr("Removed %1 %2").arg(label, installed.value(line)));
}

Result Loft::restartPhp(const QString &minor) {
    if (System::systemctl({"restart", "loft-php@" + minor + ".service"}) != 0)
        return Result::fail(tr("PHP %1 didn't restart; see: journalctl --user -u loft-php@%1").arg(minor));
    return Result::done(tr("Restarted PHP %1").arg(minor));
}

Result Loft::nginxEdit(const QString &name, QString *file) {
    QString error;
    const auto found = target(name, &error);
    if (!found)
        return Result::fail(error);
    const QString path = m_paths.nginxOverride(found->host);
    if (file)
        *file = path;
    if (QFileInfo::exists(path))
        return Result::done(path);
    const QString header = QStringList{
        "# " + tr("Your nginx config for %1. Loft serves this file instead of its own,").arg(found->host),
        "# " + tr("reloading nginx each time you save. Back to Loft's: loft nginx:reset %1").arg(found->name),
        "", ""}.join('\n');
    Site site = *found;
    if (site.secure && !Certs::ensureSite(m_paths, site.host, &error))
        site.secure = false;
    if (!System::writeIfChanged(path, (header + nginxSiteConfig(site, m_paths, System::ipv6Loopback())).toUtf8()))
        return Result::fail(tr("Can't write %1").arg(path));
    return Result::done(path);
}

Result Loft::nginxReset(const QString &name) {
    QString error;
    const auto found = target(name, &error);
    if (!found)
        return Result::fail(error);
    if (!QFile::remove(m_paths.nginxOverride(found->host)))
        return Result::done(tr("%1 already uses Loft's nginx config").arg(found->host));
    return commit(tr("%1 is back on Loft's nginx config").arg(found->host));
}

Result Loft::apply() {
    QStringList problems;
    const Paths &p = m_paths;
    for (const QString &directory : {p.data, p.run(), p.logs(), p.nginx() + "/sites", p.nginx() + "/temp",
                                     p.nginx() + "/default", p.bin(), p.certs(), p.npmGlobal(), p.nginxOverrides()})
        QDir().mkpath(directory);

    const bool ipv6 = System::ipv6Loopback();
    QList<Site> all = sites();
    const auto php = installedPhp();

    // A site whose certificate can't be made is served over plain HTTP rather
    // than taking nginx down for every other site.
    for (Site &site : all) {
        QString error;
        if (site.secure && !Certs::ensureSite(p, site.host, &error)) {
            problems << tr("No certificate for %1: %2").arg(site.host, error);
            site.secure = false;
        }
    }

    bool nginxChanged = false, changed = false;
    System::writeIfChanged(p.nginx() + "/nginx.conf", nginxMainConfig(p, ipv6).toUtf8(), false, &changed);
    nginxChanged = nginxChanged || changed;
    System::writeIfChanged(p.nginx() + "/default/index.html", defaultPage(m_config.tld).toUtf8());
    QSet<QString> wanted;
    for (const Site &site : all) {
        if (site.missing) {
            problems << tr("%1 points at %2, which is gone").arg(site.host, site.path);
            continue;
        }
        if (site.usesPhp() && !php.contains(site.phpVersion))
            problems << (site.phpVersion.isEmpty()
                             ? tr("%1 needs PHP, and none is installed: loft php:install 8.5").arg(site.host)
                             : tr("%1 needs PHP %2, which isn't installed: loft php:install %2")
                                   .arg(site.host, site.phpVersion));
        const QString file = site.host + ".conf";
        wanted << file;
        QByteArray content = nginxSiteConfig(site, p, ipv6).toUtf8();
        QFile custom(p.nginxOverride(site.host));
        if (custom.open(QIODevice::ReadOnly))
            content = custom.readAll();
        System::writeIfChanged(p.nginx() + "/sites/" + file, content, false, &changed);
        nginxChanged = nginxChanged || changed;
    }
    for (const QString &file : QDir(p.nginx() + "/sites").entryList({"*.conf"}, QDir::Files))
        if (!wanted.contains(file)) {
            QFile::remove(p.nginx() + "/sites/" + file);
            nginxChanged = true;
        }

    QSet<QString> fpmChanged;
    for (const QString &minor : php.keys()) {
        System::writeIfChanged(p.phpDir(minor) + "/php-fpm.conf", phpFpmConfig(p, minor).toUtf8(), false, &changed);
        if (changed)
            fpmChanged << minor;
    }
    System::writeIfChanged(p.versionsFile(), versionsTable(m_config, all).toUtf8());
    System::installShims(p, php.keys());

    // Validate before anything reloads. Without nginx (tests on a machine
    // that lacks it) there's nothing to check against.
    bool nginxValid = true;
    QString nginxOutput;
    if (!System::which("nginx").isEmpty()) {
        const QStringList test{"-t", "-q", "-e", "stderr", "-p", p.nginx(), "-c", p.nginx() + "/nginx.conf"};
        int code = System::run(System::nginxExecutable(), test, &nginxOutput);
        // A mistake in one of your own site configs shouldn't hold every
        // other site back: serve Loft's config for that site until it's fixed.
        QSet<QString> fallen;
        while (code != 0) {
            const Site *culprit = nullptr;
            for (const Site &site : all)
                if (site.customNginx && !fallen.contains(site.host)
                    && nginxOutput.contains("/sites/" + site.host + ".conf"))
                    culprit = &site;
            if (!culprit)
                break;
            fallen << culprit->host;
            problems << tr("Your nginx config for %1 has an error, so Loft serves its own until you fix %2: %3")
                            .arg(culprit->host, p.nginxOverride(culprit->host), nginxOutput.trimmed());
            System::writeIfChanged(p.nginx() + "/sites/" + culprit->host + ".conf",
                                   nginxSiteConfig(*culprit, p, ipv6).toUtf8());
            nginxChanged = true;
            code = System::run(System::nginxExecutable(), test, &nginxOutput);
        }
        nginxValid = code == 0;
    }

    if (!m_services)
        return problems.isEmpty() ? Result::done() : Result::fail(problems.join('\n'));

    QString error;
    if (!System::installUnits(p, m_config, &error))
        problems << error;

    QSet<QString> needed;
    if (php.contains(m_config.php))
        needed << m_config.php;
    for (const Site &site : all)
        if (site.usesPhp() && php.contains(site.phpVersion))
            needed << site.phpVersion;
    const QSet<QString> enabled = System::enabledPhpVersions(p);

    QStringList units{"loft-nginx.service", "loft-dns.service", "loft-updates.timer"};
    for (const QString &minor : needed)
        units << "loft-php@" + minor + ".service";
    const auto states = System::unitStates(units);
    auto active = [&](const QString &unit) { return states.value(unit) == "active"; };

    // New PHP versions start before nginx points at them, and unused ones
    // stop only after nginx has let go of them.
    for (const QString &minor : needed) {
        const QString unit = "loft-php@" + minor + ".service";
        if (!enabled.contains(minor) || !active(unit))
            System::systemctl({"enable", "--now", unit});
        else if (fpmChanged.contains(minor))
            System::systemctl({"restart", unit});
        else
            continue;
        waitForSocket(p.phpSocket(minor), 5000);
    }

    if (!active("loft-dns.service"))
        System::systemctl({"enable", "--now", "loft-dns.service"});
    if (m_config.checkUpdates != active("loft-updates.timer"))
        System::systemctl({m_config.checkUpdates ? "enable" : "disable", "--now", "loft-updates.timer"});

    if (System::which("nginx").isEmpty()) {
        problems << tr("nginx isn't installed: sudo pacman -S nginx");
    } else {
        if (!nginxValid) {
            problems << tr("nginx rejected the configuration: %1").arg(nginxOutput.trimmed());
        } else if (!active("loft-nginx.service")) {
            System::systemctl({"enable", "loft-nginx.service"});
            if (System::systemctl({"restart", "loft-nginx.service"}) != 0)
                problems << tr("nginx didn't start; see: journalctl --user -u loft-nginx");
        } else if (nginxChanged) {
            const QSet<int> old = nginxWorkers();
            System::systemctl({"reload", "loft-nginx.service"});
            waitForNewWorkers(old, 5000);
        }
        bool anySecure = false;
        for (const Site &site : all)
            anySecure = anySecure || (site.secure && !site.missing);
        waitForPort(80, 2000);
        if (anySecure)
            waitForPort(443, 2000);
    }

    for (const QString &minor : enabled - needed)
        System::systemctl({"disable", "--now", "loft-php@" + minor + ".service"});
    return problems.isEmpty() ? Result::done() : Result::fail(problems.join('\n'));
}

QList<Check> Loft::doctor() const {
    QList<Check> checks;
    const QString tld = m_config.tld;

    const bool nginx = !System::which("nginx").isEmpty();
    checks.append({"nginx", tr("nginx is installed"), nginx,
                   nginx ? System::which("nginx") : tr("Install it with: sudo pacman -S nginx"), false});

    const int start = System::unprivilegedPortStart();
    checks.append({"ports", tr("Ports 80 and 443 are open to your user"), start <= 80,
                   start <= 80 ? tr("Unprivileged ports start at %1").arg(start)
                               : tr("Only root can serve them now; setup changes that once"), true});

    QStringList units{"loft-nginx.service", "loft-dns.service"};
    if (!m_config.php.isEmpty())
        units << "loft-php@" + m_config.php + ".service";
    const auto states = System::unitStates(units);
    const bool web = states.value("loft-nginx.service") == "active";
    checks.append({"web", tr("The web server is running"), web,
                   web ? tr("nginx on 127.0.0.1:80 and :443") : tr("See: journalctl --user -u loft-nginx"), true});

    bool resolves = false;
    if (System::dnsConfigured(tld) && states.value("loft-dns.service") == "active") {
        const QHostInfo info = QHostInfo::fromName("loft-check." + tld);
        for (const QHostAddress &address : info.addresses())
            resolves = resolves || address.isLoopback();
    }
    checks.append({"dns", tr("Every .%1 name points to this machine").arg(tld), resolves,
                   resolves ? tr("Through NetworkManager's \"loft\" connection") : tr("Setup configures it"), true});

    const bool system = Certs::trustedBySystem(m_paths);
    checks.append({"ca-system", tr("The system trusts Loft's certificates"), system,
                   system ? tr("curl, PHP and Node accept https://*.%1").arg(tld)
                          : tr("Setup adds the CA (asks for your password once)"), true});

    const bool browsers = Certs::trustedByBrowsers(m_paths);
    checks.append({"ca-browsers", tr("Chromium and Firefox trust Loft's certificates"), browsers,
                   browsers ? tr("In %n certificate database(s)", int(Certs::nssDatabases(m_paths).size()))
                            : tr("Setup adds the CA to them"), true});

    const bool shell = System::shellConfigured(m_paths);
    checks.append({"shell", tr("php, composer and node work in your terminal"), shell,
                   shell ? tr("Through ~/.bashrc") : tr("Setup adds one line to ~/.bashrc"), true});

    const auto php = installedPhp();
    const bool phpOk = php.contains(m_config.php);
    const bool fpm = states.value("loft-php@" + m_config.php + ".service") == "active";
    checks.append({"php", tr("A default PHP is installed and running"), phpOk && fpm,
                   !phpOk ? tr("Install one: loft php:install 8.5")
                   : fpm ? tr("PHP %1").arg(php.value(m_config.php))
                         : tr("PHP-FPM isn't running; see: journalctl --user -u loft-php@%1").arg(m_config.php),
                   false});

    const auto node = installedNode();
    const bool nodeOk = node.contains(m_config.node);
    checks.append({"node", tr("A default Node is installed"), nodeOk,
                   nodeOk ? tr("Node %1").arg(node.value(m_config.node)) : tr("Install one: loft node:install 24"),
                   false});

    if (QFileInfo(m_paths.home + "/.config/omarchy").isDir()) {
        const bool bar = System::barWidgetInstalled(m_paths);
        checks.append({"bar", tr("Loft is in the Omarchy bar"), bar,
                       bar ? tr("Start, stop and switch PHP from the top bar") : tr("Setup adds it"), true});
    }

    const bool composer = QFileInfo::exists(m_paths.composer());
    checks.append({"composer", tr("Composer is installed"), composer,
                   composer ? m_paths.composer() : tr("Setup downloads it"), true});
    return checks;
}

Result Loft::setup(bool askRoot, const Step &step) {
    bool ok = true;
    auto report = [&](const QString &label, bool success, const QString &detail) {
        ok = ok && success;
        if (step)
            step(label, success, detail);
    };
    QDir().mkpath(m_paths.config);
    if (!QFileInfo::exists(m_paths.configFile()))
        m_config.save(m_paths.configFile());

    QString error;
    const bool ca = Certs::ensureCa(m_paths, &error);
    report(tr("Local certificate authority"), ca, ca ? Certs::caCert(m_paths) : error);
    if (ca) {
        error.clear();
        const bool browsers = Certs::trustInBrowsers(m_paths, &error);
        report(tr("Trusted by Chromium and Firefox"), browsers, error);
    }

    if (askRoot && ca && System::rootSetupNeeded(m_paths)) {
        error.clear();
        const bool root = System::runRootSetup(m_paths, false, &error);
        report(tr("System trust and ports 80/443 (your password)"), root, error);
        if (root && m_services)
            System::systemctl({"restart", "loft-nginx.service"});
    }

    error.clear();
    const bool dns = System::configureDns(m_config.tld, &error);
    report(tr("*.%1 points to this machine").arg(m_config.tld), dns, error);

    error.clear();
    const bool shell = System::configureShell(m_paths, &error);
    report(tr("php, composer and node in your terminal"), shell,
           shell ? tr("Open a new terminal to use them") : error);

    if (QFileInfo(m_paths.home + "/.config/omarchy").isDir()) {
        error.clear();
        const bool bar = System::installBarWidget(m_paths, &error);
        report(tr("Loft in the Omarchy bar"), bar, error);
    }

    if (!QFileInfo::exists(m_paths.composer())) {
        error.clear();
        const bool composer = Runtimes::installComposer(m_paths, &error);
        report(tr("Composer"), composer, error);
    }

    const Result applied = apply();
    report(tr("Web server and DNS services"), applied.ok, applied.message);
    return ok ? Result::done(tr("Loft is set up")) : Result::fail(tr("Some steps failed; run loft doctor"));
}

Result Loft::uninstall(bool purge, const Step &step) {
    auto report = [&](const QString &label, bool success, const QString &detail) {
        if (step)
            step(label, success, detail);
    };
    System::removeUnits(m_paths);
    report(tr("Stopped and removed the services"), true, {});
    System::removeDns();
    report(tr("Removed the .%1 DNS route").arg(m_config.tld), true, {});
    System::unconfigureShell(m_paths);
    report(tr("Removed Loft from ~/.bashrc"), true, {});
    System::removeBarWidget(m_paths);
    report(tr("Removed Loft from the Omarchy bar"), true, {});
    Certs::untrustInBrowsers(m_paths);
    report(tr("Removed the CA from Chromium and Firefox"), true, {});
    if (Certs::trustedBySystem(m_paths) || QFileInfo::exists("/etc/sysctl.d/60-loft.conf")) {
        QString error;
        const bool root = System::runRootSetup(m_paths, true, &error);
        report(tr("Removed the CA from the system (your password)"), root, error);
    }
    if (purge) {
        QDir(m_paths.data).removeRecursively();
        QDir(m_paths.config).removeRecursively();
        QDir(m_paths.state).removeRecursively();
        report(tr("Deleted PHP, Node, certificates and settings"), true, {});
    }
    return Result::done(purge ? tr("Loft is gone") : tr("Loft is switched off; your PHP and Node versions are kept"));
}
