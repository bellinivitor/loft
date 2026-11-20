#include "backend.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>

namespace {


QString tilde(const QString &path) {
    const QString home = QDir::homePath();
    return path == home || path.startsWith(home + "/") ? "~" + path.mid(home.length()) : path;
}

} // namespace

Backend::Backend(QObject *parent) : QObject(parent) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(150);
    connect(&m_debounce, &QTimer::timeout, this, &Backend::refresh);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });

    connect(&m_servicesProcess, &QProcess::finished, this, [this] {
        const QStringList states = QString::fromUtf8(m_servicesProcess.readAllStandardOutput()).split('\n');
        QVariantMap services{{"nginx", states.value(0).trimmed()}, {"dns", states.value(1).trimmed()},
                             {"php", m_loft.config().php.isEmpty() ? QString() : states.value(2).trimmed()}};
        if (services != m_services) {
            m_services = services;
            emit servicesChanged();
        }
    });
    m_servicesTimer.setInterval(5000);
    connect(&m_servicesTimer, &QTimer::timeout, this, &Backend::pollServices);
    m_servicesTimer.start();

    connect(&m_picker, &FolderPicker::picked, this, [this](const QString &path) {
        emit folderPicked(m_pickPurpose, path);
    });
    connect(&m_picker, &FolderPicker::failed, this, [this](const QString &message) { emit notice(message, true); });

    reloadState();
    pollServices();
    runDoctor();
    refreshCatalog();
}

Backend::~Backend() {
    for (QProcess *process : m_installs)
        process->kill();
}

QString Backend::lastLine(const QString &text) {
    const QStringList lines = text.trimmed().split('\n', Qt::SkipEmptyParts);
    QString line = lines.isEmpty() ? QString() : lines.last().trimmed();
    if (line.startsWith("loft: "))
        line = line.mid(6);
    return line;
}

void Backend::watch() {
    const Paths &paths = m_loft.paths();
    QDir().mkpath(paths.config);
    QStringList wanted{paths.config, paths.configFile(), paths.data + "/php", paths.data + "/node"};
    for (const QString &parked : m_loft.config().parked)
        wanted << parked;
    const QStringList current = m_watcher.files() + m_watcher.directories();
    for (const QString &path : wanted)
        if (!current.contains(path) && QFileInfo::exists(path))
            m_watcher.addPath(path);
}

void Backend::reloadState() {
    m_loft.reload();
    m_catalog = Runtimes::cachedCatalog(m_loft.paths());
    const auto php = m_loft.installedPhp();
    m_sites.clear();
    for (const Site &site : m_loft.sites()) {
        const QString node = site.node.isEmpty() ? m_loft.config().node : site.node;
        m_sites.append(QVariantMap{
            {"name", site.name}, {"host", site.host}, {"url", site.url()}, {"path", site.path},
            {"shortPath", tilde(site.path)}, {"kind", site.kind}, {"source", site.source},
            {"proxy", site.proxy}, {"secure", site.secure}, {"php", site.php}, {"node", site.node},
            {"phpVersion", site.phpVersion}, {"nodeVersion", node}, {"missing", site.missing},
            {"phpInstalled", php.contains(site.phpVersion)},
            {"parkedIn", site.source == "parked" ? tilde(QFileInfo(site.path).absolutePath()) : QString()},
        });
    }
    watch();
    emit changed();
}

void Backend::refresh() {
    reloadState();
    pollServices();
}

void Backend::pollServices() {
    if (m_servicesProcess.state() != QProcess::NotRunning)
        return;
    const QString php = m_loft.config().php.isEmpty() ? "loft-php@none.service"
                                                      : "loft-php@" + m_loft.config().php + ".service";
    m_servicesProcess.start("systemctl", {"--user", "is-active", "loft-nginx.service", "loft-dns.service", php});
}

QVariantMap Backend::settings() const {
    const Config &config = m_loft.config();
    QVariantList parked;
    for (const QString &path : config.parked)
        parked.append(QVariantMap{{"path", path}, {"shortPath", tilde(path)}});
    return QVariantMap{
        {"tld", config.tld},
        {"php", config.php},
        {"node", config.node},
        {"phpVersion", m_loft.installedPhp().value(config.php)},
        {"nodeVersion", m_loft.installedNode().value(config.node)},
        {"phpBuild", config.phpBuild},
        {"checkUpdates", config.checkUpdates},
        {"parked", parked},
        {"configDir", tilde(m_loft.paths().config)},
    };
}

QVariantList Backend::runtimeRows(const QString &tool) const {
    const bool php = tool == "php";
    const auto installed = php ? m_loft.installedPhp() : m_loft.installedNode();
    const QString fallback = php ? m_loft.config().php : m_loft.config().node;
    QMap<QString, QString> latest, lts;
    if (php) {
        for (const PhpRelease &release : m_catalog.php)
            latest.insert(release.minor, release.version);
    } else {
        for (const NodeRelease &release : m_catalog.node) {
            latest.insert(release.major, release.version);
            lts.insert(release.major, release.lts);
        }
    }
    QStringList lines = installed.keys();
    for (const QString &line : latest.keys())
        if (!lines.contains(line))
            lines << line;
    std::sort(lines.begin(), lines.end(), [](const QString &a, const QString &b) { return compareVersions(a, b) > 0; });

    QMap<QString, int> uses;
    for (const QVariant &value : m_sites) {
        const QVariantMap site = value.toMap();
        if (site.value("kind") == "proxy")
            continue;
        if (php && site.value("kind") != "static")
            uses[site.value("phpVersion").toString()] += 1;
        if (!php)
            uses[site.value("nodeVersion").toString()] += 1;
    }

    QVariantList rows;
    for (const QString &line : lines) {
        const QString key = tool + ":" + line;
        const QString version = installed.value(line);
        rows.append(QVariantMap{
            {"tool", tool},
            {"line", line},
            {"installed", version},
            {"latest", latest.value(line)},
            {"lts", lts.value(line)},
            {"isDefault", line == fallback},
            {"sites", uses.value(line)},
            {"update", !version.isEmpty() && latest.contains(line) && compareVersions(latest[line], version) > 0},
            {"busy", m_jobs.contains(key)},
            {"progress", m_jobs.value(key).toMap().value("progress", -1)},
        });
    }
    return rows;
}

int Backend::updateCount() const { return int(Runtimes::updates(m_loft.paths(), m_catalog).size()); }

QString Backend::catalogFetched() const {
    return m_catalog.fetched.isValid() ? m_catalog.fetched.toLocalTime().toString(Qt::ISODate) : QString();
}

bool Backend::setupNeeded() const {
    for (const QVariant &value : m_checks) {
        const QVariantMap check = value.toMap();
        if (!check.value("ok").toBool() && check.value("needsSetup").toBool())
            return true;
    }
    return false;
}

void Backend::runDoctor() {
    if (m_checking)
        return;
    m_checking = true;
    emit checksChanged();
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process] {
        process->deleteLater();
        const QJsonArray array = QJsonDocument::fromJson(process->readAllStandardOutput()).array();
        m_checks = array.toVariantList();
        m_checking = false;
        emit checksChanged();
    });
    process->start(QCoreApplication::applicationFilePath(), {"doctor", "--json"});
}

void Backend::runSetup() {
    if (m_settingUp)
        return;
    m_settingUp = true;
    emit checksChanged();
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this, [this, process](int code) {
        process->deleteLater();
        const QString output = QString::fromUtf8(process->readAll());
        m_settingUp = false;
        emit checksChanged();
        QStringList failures;
        for (const QString &line : output.split('\n'))
            if (line.trimmed().startsWith("✗"))
                failures << line.trimmed().mid(1).trimmed();
        emit notice(code == 0 ? lastLine(output) : failures.isEmpty() ? lastLine(output) : failures.join('\n'),
                    code != 0);
        refresh();
        runDoctor();
    });
    process->start(QCoreApplication::applicationFilePath(), {"setup"});
}

void Backend::refreshCatalog() {
    if (m_catalogLoading)
        return;
    m_catalogLoading = true;
    emit changed();
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process] {
        process->deleteLater();
        m_catalogLoading = false;
        reloadState();
    });
    process->start(QCoreApplication::applicationFilePath(), {"catalog"});
}

void Backend::checkUpdates() {
    if (m_catalogLoading)
        return;
    m_catalogLoading = true;
    emit changed();
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process](int code) {
        process->deleteLater();
        m_catalogLoading = false;
        const QString output = QString::fromUtf8(code == 0 ? process->readAllStandardOutput()
                                                           : process->readAllStandardError());
        emit notice(lastLine(output), code != 0);
        reloadState();
    });
    process->start(QCoreApplication::applicationFilePath(), {"check-updates"});
}

void Backend::command(const QStringList &arguments, const QString &directory) {
    m_queue.enqueue({arguments, directory});
    emit jobsChanged();
    next();
}

void Backend::next() {
    if (m_quick || m_queue.isEmpty())
        return;
    m_quick = new QProcess(this);
    connect(m_quick, &QProcess::finished, this, [this](int code) {
        QProcess *process = m_quick;
        m_quick = nullptr;
        process->deleteLater();
        const QString output = QString::fromUtf8(code == 0 ? process->readAllStandardOutput()
                                                           : process->readAllStandardError());
        // A failed command's whole message matters; a success needs one line.
        const QString message = code == 0 ? lastLine(output) : output.trimmed().replace("loft: ", "");
        if (!message.isEmpty())
            emit notice(message, code != 0);
        refresh();
        emit jobsChanged();
        next();
    });
    const auto [arguments, directory] = m_queue.dequeue();
    if (!directory.isEmpty())
        m_quick->setWorkingDirectory(directory);
    m_quick->start(QCoreApplication::applicationFilePath(), arguments);
    emit jobsChanged();
}

void Backend::link(const QString &path, const QString &name, bool secure, const QString &php) {
    const QString folder = QDir::cleanPath(path.trimmed());
    if (!QFileInfo(folder).isDir()) {
        emit notice(tr("%1 isn't a folder").arg(path), true);
        return;
    }
    // link serves the current directory, the same as in a terminal.
    QStringList arguments{"link"};
    if (!name.trimmed().isEmpty())
        arguments << name.trimmed();
    if (secure)
        arguments << "--secure";
    if (!php.isEmpty())
        arguments << "--isolate=" + php;
    command(arguments, folder);
}

void Backend::unlink(const QString &name) { command({"unlink", name}); }
void Backend::setSecure(const QString &name, bool on) { command({on ? "secure" : "unsecure", name}); }

void Backend::isolate(const QString &name, const QString &tool, const QString &line) {
    if (line.isEmpty())
        command(tool == "node" ? QStringList{"unisolate", "--node", "--site=" + name}
                               : QStringList{"unisolate", "--site=" + name});
    else
        command({"isolate", tool == "node" ? "node@" + line : line, "--site=" + name});
}

void Backend::proxy(const QString &name, const QString &url, bool secure) {
    QStringList arguments{"proxy", name.trimmed(), url.trimmed()};
    if (secure)
        arguments << "--secure";
    command(arguments);
}

void Backend::unproxy(const QString &name) { command({"unproxy", name}); }
void Backend::park(const QString &path) { command({"park", path}); }
void Backend::forget(const QString &path) { command({"forget", path}); }
void Backend::uninstall(const QString &tool, const QString &line) { command({tool + ":uninstall", line}); }

void Backend::makeDefault(const QString &tool, const QString &line) {
    command({"use", tool == "node" ? "node@" + line : line});
}

void Backend::restartPhp(const QString &line) { command({"php:restart", line}); }
void Backend::restartServices() { command({"restart"}); }
void Backend::setCheckUpdates(bool on) { command({"set", "check-updates", on ? "on" : "off"}); }
void Backend::setPhpBuild(const QString &build) { command({"set", "php-build", build}); }

void Backend::install(const QString &tool, const QString &line) {
    const QString key = tool + ":" + line;
    if (m_installs.contains(key))
        return;
    auto *process = new QProcess(this);
    m_installs.insert(key, process);
    m_jobs.insert(key, QVariantMap{{"progress", 0.0}});
    emit jobsChanged();
    emit changed();
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, key] {
        while (process->canReadLine()) {
            const QString line = QString::fromUtf8(process->readLine()).trimmed();
            if (!line.startsWith("progress "))
                continue;
            const double received = line.section(' ', 1, 1).toDouble(), total = line.section(' ', 2, 2).toDouble();
            if (total > 0) {
                m_jobs.insert(key, QVariantMap{{"progress", received / total}});
                emit jobsChanged();
                emit changed();
            }
        }
    });
    connect(process, &QProcess::finished, this, [this, process, key](int code) {
        process->deleteLater();
        m_installs.remove(key);
        m_jobs.remove(key);
        const QString output = QString::fromUtf8(code == 0 ? process->readAllStandardOutput()
                                                           : process->readAllStandardError());
        emit notice(lastLine(output), code != 0);
        emit jobsChanged();
        refresh();
        runDoctor();
    });
    process->start(QCoreApplication::applicationFilePath(), {tool + ":install", line, "--progress"});
}

QString Backend::suggestName(const QString &path) const {
    return siteName(QFileInfo(QDir::cleanPath(path)).fileName());
}

void Backend::pickFolder(const QString &purpose, const QString &title) {
    m_pickPurpose = purpose;
    m_picker.pick(title);
}

void Backend::openUrl(const QString &url) { QDesktopServices::openUrl(QUrl(url)); }
void Backend::openFolder(const QString &path) { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); }

void Backend::openPhpIni(const QString &line) {
    const QString ini = m_loft.paths().phpConfigDir(line) + "/php.ini";
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo::exists(ini) ? ini : m_loft.paths().phpConfigDir(line)));
}

void Backend::copy(const QString &text) { QGuiApplication::clipboard()->setText(text); }
