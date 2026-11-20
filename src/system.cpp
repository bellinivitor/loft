#include "system.h"
#include "certs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <unistd.h>

namespace {

const char *Connection = "loft";
const char *Interface = "loft0";
// A /32 on the dummy interface: systemd-resolved only routes DNS through a
// link with a usable address, and a single host address adds no route.
const char *DummyAddress = "10.254.254.53/32";
const char *ShellMarker = "# Loft: PHP, Composer and Node on PATH";
const char *SysctlFile = "/etc/sysctl.d/60-loft.conf";
const char *BarWidget = "loft.panel";

QString readText(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString uwsmEnvFile(const Paths &paths) { return paths.home + "/.config/uwsm/env.d/60-loft"; }

QString tr(const char *text) { return QCoreApplication::translate("loft", text); }

} // namespace

namespace System {

int run(const QString &program, const QStringList &arguments, QString *output, int timeoutMs, bool mergeErrors) {
    QProcess process;
    process.setProcessChannelMode(mergeErrors ? QProcess::MergedChannels : QProcess::SeparateChannels);
    // Parse-friendly output from every tool, whatever the session language.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL", "C");
    process.setProcessEnvironment(environment);
    process.start(program, arguments);
    if (!process.waitForStarted(5000)) {
        if (output)
            *output = process.errorString();
        return -1;
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        if (output)
            *output = QString::fromUtf8(process.readAllStandardOutput());
        return -1;
    }
    if (output)
        *output = QString::fromUtf8(process.readAllStandardOutput());
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

QString which(const QString &program) { return QStandardPaths::findExecutable(program); }

QString loftExecutable() {
    if (QFileInfo("/usr/bin/loft").isExecutable())
        return "/usr/bin/loft";
    return QCoreApplication::applicationFilePath();
}

QString nginxExecutable() {
    const QString found = which("nginx");
    return found.isEmpty() ? QStringLiteral("/usr/bin/nginx") : found;
}

bool writeIfChanged(const QString &path, const QByteArray &content, bool executable, bool *changed) {
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == content) {
        if (changed)
            *changed = false;
        return true;
    }
    existing.close();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(content);
    if (!file.commit())
        return false;
    if (executable)
        QFile::setPermissions(path, QFile::permissions(path) | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
    if (changed)
        *changed = true;
    return true;
}

int systemctl(const QStringList &arguments, QString *output) {
    return run("systemctl", QStringList{"--user"} + arguments, output);
}

QMap<QString, QString> unitStates(const QStringList &requested) {
    QMap<QString, QString> states;
    // A template without an instance ("loft-php@.service") isn't a unit;
    // systemctl would print nothing for it and shift every line after it.
    QStringList units;
    for (const QString &unit : requested)
        if (!unit.contains("@.")) units << unit;
    if (units.isEmpty())
        return states;
    QString output;
    run("systemctl", QStringList{"--user", "is-active"} + units, &output, 60000, false);
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (int i = 0; i < units.size(); ++i)
        states.insert(units[i], i < lines.size() ? lines[i].trimmed() : QStringLiteral("unknown"));
    return states;
}

bool unitActive(const QString &unit) { return unitStates({unit}).value(unit) == "active"; }

QSet<QString> enabledPhpVersions(const Paths &paths) {
    QSet<QString> versions;
    const auto entries = QDir(paths.units + "/default.target.wants").entryList({"loft-php@*.service"});
    static const QRegularExpression instance("^loft-php@(.+)\\.service$");
    for (const QString &entry : entries) {
        const auto match = instance.match(entry);
        if (match.hasMatch())
            versions.insert(match.captured(1));
    }
    return versions;
}

QMap<QString, QString> unitFiles(const Paths &paths, const Config &config, const QString &loft) {
    const QString nginx = nginxExecutable();
    const QString prefix = paths.nginx(), conf = paths.nginx() + "/nginx.conf";
    const QString mkdirs = "ExecStartPre=/usr/bin/mkdir -p " + paths.run() + " " + paths.logs() + " " + prefix + "/temp";
    QMap<QString, QString> units;

    units.insert("loft-nginx.service", QStringList{
        "[Unit]",
        "Description=Loft web server (nginx)",
        "After=network.target",
        "",
        "[Service]",
        mkdirs,
        "ExecStartPre=" + nginx + " -t -q -e stderr -p " + prefix + " -c " + conf,
        "ExecStart=" + nginx + " -e stderr -p " + prefix + " -c " + conf,
        "ExecReload=" + nginx + " -t -q -e stderr -p " + prefix + " -c " + conf,
        "ExecReload=/bin/kill -HUP $MAINPID",
        "Restart=on-failure",
        "",
        "[Install]",
        "WantedBy=default.target",
        ""}.join('\n'));

    units.insert("loft-php@.service", QStringList{
        "[Unit]",
        "Description=Loft PHP %i (FPM)",
        "",
        "[Service]",
        mkdirs,
        "ExecStart=" + paths.data + "/php/%i/php-fpm -F -y " + paths.data + "/php/%i/php-fpm.conf -c "
            + paths.config + "/php/%i/php.ini",
        "ExecReload=/bin/kill -USR2 $MAINPID",
        "Restart=on-failure",
        "",
        "[Install]",
        "WantedBy=default.target",
        ""}.join('\n'));

    units.insert("loft-dns.service", QStringList{
        "[Unit]",
        "Description=Loft DNS (*." + config.tld + " to this machine)",
        "",
        "[Service]",
        "ExecStart=" + loft + " dns-server",
        "Restart=on-failure",
        "",
        "[Install]",
        "WantedBy=default.target",
        ""}.join('\n'));

    units.insert("loft-updates.service", QStringList{
        "[Unit]",
        "Description=Loft: check for new PHP and Node releases",
        "",
        "[Service]",
        "Type=oneshot",
        "ExecStart=" + loft + " check-updates --notify",
        ""}.join('\n'));

    units.insert("loft-updates.timer", QStringList{
        "[Unit]",
        "Description=Loft: check for new PHP and Node releases daily",
        "",
        "[Timer]",
        "OnCalendar=daily",
        "Persistent=true",
        "RandomizedDelaySec=1h",
        "",
        "[Install]",
        "WantedBy=timers.target",
        ""}.join('\n'));

    units.insert("loft-refresh.service", QStringList{
        "[Unit]",
        "Description=Loft: pick up new parked folders and edited nginx configs",
        "",
        "[Service]",
        "Type=oneshot",
        "ExecStart=" + loft + " refresh --notify",
        ""}.join('\n'));

    // Folders appearing in a parked directory, and your own nginx configs
    // being saved, both regenerate and reload.
    QStringList lines{"[Unit]", "Description=Loft: watch parked folders and your nginx configs", "", "[Path]"};
    for (const QString &parked : config.parked)
        lines << "PathChanged=" + parked;
    lines << "PathChanged=" + paths.nginxOverrides();
    lines << "Unit=loft-refresh.service" << "" << "[Install]" << "WantedBy=default.target" << "";
    units.insert("loft-watch.path", lines.join('\n'));
    return units;
}

bool installUnits(const Paths &paths, const Config &config, QString *error) {
    const auto units = unitFiles(paths, config, loftExecutable());
    bool anyChanged = false;
    for (auto it = units.begin(); it != units.end(); ++it) {
        bool changed = false;
        if (!writeIfChanged(paths.units + "/" + it.key(), it.value().toUtf8(), false, &changed)) {
            if (error)
                *error = tr("Can't write %1").arg(paths.units + "/" + it.key());
            return false;
        }
        anyChanged = anyChanged || changed;
    }

    // 0.1.0 watched parked folders only, under another name.
    if (QFileInfo::exists(paths.units + "/loft-parked.path")) {
        systemctl({"disable", "--now", "loft-parked.path"});
        QFile::remove(paths.units + "/loft-parked.path");
        anyChanged = true;
    }
    if (anyChanged)
        systemctl({"daemon-reload"});
    QDir().mkpath(paths.nginxOverrides());
    // A changed list of folders only takes effect when the path unit restarts.
    if (anyChanged || !unitActive("loft-watch.path")) {
        systemctl({"enable", "loft-watch.path"});
        systemctl({"restart", "loft-watch.path"});
    }
    return true;
}

void removeUnits(const Paths &paths) {
    QStringList units{"loft-nginx.service", "loft-dns.service", "loft-updates.timer", "loft-watch.path"};
    for (const QString &version : enabledPhpVersions(paths))
        units << "loft-php@" + version + ".service";
    systemctl(QStringList{"disable", "--now"} + units);
    for (const QString &file : unitFiles(paths, Config(), QString()).keys())
        QFile::remove(paths.units + "/" + file);
    systemctl({"daemon-reload"});
}

QMap<QString, QString> shimFiles(const Paths &paths, const QStringList &phpMinors) {
    const QString data = paths.data, table = paths.versionsFile();
    QMap<QString, QString> shims;

    auto php = [&](const QString &fixed) {
        QStringList lines{"#!/usr/bin/env bash",
                          "# Loft's php: " + QString(fixed.isEmpty()
                              ? "the version this folder's site is isolated to, or the default."
                              : "always PHP " + fixed + "."),
                          "loft_data='" + data + "'"};
        if (fixed.isEmpty())
            lines << "v=$(awk -F'\\t' -v d=\"$PWD/\" '$1 == \"php\" && index(d, $3) == 1 { print $2; exit }' \""
                         + table + "\" 2>/dev/null)";
        else
            lines << "v=" + fixed;
        lines << "if [[ -z \"$v\" || ! -x \"$loft_data/php/$v/php\" ]]; then"
              << "  echo \"loft: " + tr("PHP %1 is not installed. Install it with: loft php:install %1")
                                         .arg("${v:-8.5}") + "\" >&2"
              << "  exit 127"
              << "fi"
              << "export PHPRC='" + paths.config + "/php/'\"$v\""
              << "exec \"$loft_data/php/$v/php\" \"$@\""
              << "";
        return lines.join('\n');
    };
    shims.insert("php", php(QString()));
    for (const QString &minor : phpMinors)
        shims.insert("php" + minor, php(minor));

    shims.insert("composer", QStringList{
        "#!/usr/bin/env bash",
        "# Loft's composer, run by the php this folder uses.",
        "if [[ ! -r '" + paths.composer() + "' ]]; then",
        "  echo \"loft: " + tr("Composer is not installed yet. Run: loft setup") + "\" >&2",
        "  exit 127",
        "fi",
        "exec '" + paths.bin() + "/php' '" + paths.composer() + "' \"$@\"",
        ""}.join('\n'));

    const QString node = QStringList{
        "#!/usr/bin/env bash",
        "# Loft's Node: the version this folder's site is isolated to, else the major",
        "# a project's .nvmrc or .node-version asks for (when installed), else the default.",
        "loft_data='" + data + "'",
        "cmd=${0##*/}",
        "row=$(awk -F'\\t' -v d=\"$PWD/\" '$1 == \"node\" && index(d, $3) == 1 { print $2 \"\\t\" $3; exit }' \""
            + table + "\" 2>/dev/null)",
        "v=${row%%$'\\t'*}",
        "if [[ -z \"$row\" || \"${row#*$'\\t'}\" == / ]]; then",
        "  dir=$PWD",
        "  while :; do",
        "    for file in .nvmrc .node-version; do",
        "      if [[ -r \"$dir/$file\" ]]; then",
        "        want=$(head -n1 \"$dir/$file\" | tr -d ' \\tvV\\r'); want=${want%%.*}",
        "        [[ -n \"$want\" && -x \"$loft_data/node/$want/bin/node\" ]] && v=$want",
        "        break 2",
        "      fi",
        "    done",
        "    [[ \"$dir\" == / || -z \"$dir\" ]] && break",
        "    dir=${dir%/*}; [[ -z \"$dir\" ]] && dir=/",
        "  done",
        "fi",
        "if [[ -z \"$v\" || ! -x \"$loft_data/node/$v/bin/$cmd\" ]]; then",
        "  echo \"loft: " + tr("Node %1 is not installed. Install it with: loft node:install %1").arg("${v:-24}")
            + "\" >&2",
        "  exit 127",
        "fi",
        "export PATH=\"$loft_data/node/$v/bin:$PATH\"",
        "export NPM_CONFIG_PREFIX=\"${NPM_CONFIG_PREFIX:-$loft_data/npm-global}\"",
        "exec \"$loft_data/node/$v/bin/$cmd\" \"$@\"",
        ""}.join('\n');
    for (const char *command : {"node", "npm", "npx", "corepack"})
        shims.insert(command, node);
    return shims;
}

bool installShims(const Paths &paths, const QStringList &phpMinors) {
    const auto shims = shimFiles(paths, phpMinors);
    bool ok = true;
    for (auto it = shims.begin(); it != shims.end(); ++it)
        ok = writeIfChanged(paths.bin() + "/" + it.key(), it.value().toUtf8(), true) && ok;
    // Versioned shims for PHP versions that are gone.
    const auto existing = QDir(paths.bin()).entryList({"php*"}, QDir::Files);
    for (const QString &file : existing)
        if (!shims.contains(file))
            QFile::remove(paths.bin() + "/" + file);
    return ok;
}

bool dnsConfigured(const QString &tld) {
    QString output;
    if (run("nmcli", {"-t", "-g", "ipv4.dns,ipv4.dns-search,GENERAL.STATE", "connection", "show", Connection},
            &output) != 0)
        return false;
    output.remove('\\'); // terse mode escapes the colons
    return output.contains(QString("127.0.0.1:%1").arg(DnsPort)) && output.contains("~" + tld)
        && output.contains("activated");
}

bool configureDns(const QString &tld, QString *error) {
    if (which("nmcli").isEmpty()) {
        if (error)
            *error = tr("NetworkManager (nmcli) is missing");
        return false;
    }
    const QStringList settings{
        "ipv4.method", "manual",
        "ipv4.addresses", DummyAddress,
        "ipv4.never-default", "yes",
        "ipv4.dns", QString("dns+udp://127.0.0.1:%1").arg(DnsPort),
        "ipv4.dns-search", "~" + tld,
        "ipv6.method", "disabled",
        "connection.autoconnect", "yes",
    };
    QString output;
    int code;
    if (run("nmcli", {"-t", "-g", "connection.id", "connection", "show", Connection}) == 0)
        code = run("nmcli", QStringList{"connection", "modify", Connection} + settings, &output);
    else
        code = run("nmcli", QStringList{"connection", "add", "type", "dummy", "con-name", Connection,
                                        "ifname", Interface} + settings, &output);
    if (code == 0)
        code = run("nmcli", {"connection", "up", Connection}, &output);
    if (code != 0 && error)
        *error = output.trimmed();
    return code == 0;
}

void removeDns() { run("nmcli", {"connection", "delete", Connection}); }

QString shellSnippet(const Paths &paths) {
    return QStringList{
        "# Loft puts its php, composer and node first on PATH. mise rebuilds PATH",
        "# before every prompt, so this runs after it each time as well.",
        "_loft_path() {",
        "  local dir",
        "  for dir in \"$HOME/.config/composer/vendor/bin\" '" + paths.npmGlobal() + "/bin' '" + paths.bin() + "'; do",
        "    PATH=\":$PATH:\"; PATH=\"${PATH//:$dir:/:}\"; PATH=\"${PATH#:}\"; PATH=\"${PATH%:}\"",
        "    PATH=\"$dir${PATH:+:$PATH}\"",
        "  done",
        "}",
        "_loft_path",
        "if [[ \"$(declare -p PROMPT_COMMAND 2>/dev/null)\" == \"declare -a\"* ]]; then",
        "  [[ \" ${PROMPT_COMMAND[*]} \" == *\" _loft_path \"* ]] || PROMPT_COMMAND+=(_loft_path)",
        "else",
        "  [[ \";${PROMPT_COMMAND};\" == *\";_loft_path;\"* ]] || PROMPT_COMMAND=\"${PROMPT_COMMAND:+$PROMPT_COMMAND;}_loft_path\"",
        "fi",
        ""}.join('\n');
}

static QString snippetFile(const Paths &paths) { return paths.data + "/shell/loft.bash"; }

bool shellConfigured(const Paths &paths) {
    return readText(paths.home + "/.bashrc").contains(snippetFile(paths))
        && QFileInfo::exists(snippetFile(paths));
}

bool configureShell(const Paths &paths, QString *error) {
    if (!writeIfChanged(snippetFile(paths), shellSnippet(paths).toUtf8())) {
        if (error)
            *error = tr("Can't write %1").arg(snippetFile(paths));
        return false;
    }
    const QString bashrc = paths.home + "/.bashrc";
    QString text = readText(bashrc);
    if (!text.contains(snippetFile(paths))) {
        if (!text.isEmpty() && !text.endsWith('\n'))
            text += '\n';
        text += QString("\n%1 (loft uninstall removes this)\n[[ -r '%2' ]] && source '%2'\n")
                    .arg(ShellMarker, snippetFile(paths));
        if (!writeIfChanged(bashrc, text.toUtf8())) {
            if (error)
                *error = tr("Can't write %1").arg(bashrc);
            return false;
        }
    }
    // Apps started from the launcher (editors, their terminals and language
    // servers) get the same commands from the next login on.
    if (QFileInfo(paths.home + "/.config/uwsm").isDir())
        writeIfChanged(uwsmEnvFile(paths),
                       QString("# Loft: PHP, Composer and Node for apps started from the desktop.\n"
                               "export PATH=\"%1:%2/bin:$PATH\"\n")
                           .arg(paths.bin(), paths.npmGlobal()).toUtf8());
    return true;
}

void unconfigureShell(const Paths &paths) {
    const QString bashrc = paths.home + "/.bashrc";
    QStringList lines = readText(bashrc).split('\n');
    for (int i = lines.size() - 1; i >= 0; --i)
        if (lines[i].startsWith(ShellMarker) || lines[i].contains(snippetFile(paths)))
            lines.removeAt(i);
    while (lines.size() >= 2 && lines.last().isEmpty() && lines[lines.size() - 2].isEmpty())
        lines.removeLast();
    writeIfChanged(bashrc, lines.join('\n').toUtf8());
    QFile::remove(snippetFile(paths));
    QFile::remove(uwsmEnvFile(paths));
}

QString barPluginSource() {
    const QString installed = "/usr/share/loft/omarchy-plugin";
    if (QFileInfo::exists(installed + "/manifest.json"))
        return installed;
    // A development build runs from build/ beside the source.
    const QString source = QDir(QCoreApplication::applicationDirPath() + "/../omarchy-plugin").canonicalPath();
    return QFileInfo::exists(source + "/manifest.json") ? source : QString();
}

static QString barPluginLink(const Paths &paths) { return paths.home + "/.config/omarchy/plugins/" + BarWidget; }
static QString shellConfig(const Paths &paths) { return paths.home + "/.config/omarchy/shell.json"; }

bool barWidgetInstalled(const Paths &paths) {
    return QFileInfo::exists(barPluginLink(paths) + "/manifest.json")
        && readText(shellConfig(paths)).contains(QString("\"%1\"").arg(BarWidget));
}

bool installBarWidget(const Paths &paths, QString *error) {
    const QString source = barPluginSource();
    if (source.isEmpty()) {
        if (error)
            *error = tr("The bar widget isn't installed with Loft");
        return false;
    }
    const QString link = barPluginLink(paths);
    QDir().mkpath(QFileInfo(link).absolutePath());
    // The plugin stays in the package, linked from here, so updates reach
    // the bar with the next shell restart.
    if (QFileInfo(link).symLinkTarget() != source) {
        QFile::remove(link);
        if (!QFile::link(source, link)) {
            if (error)
                *error = tr("Can't write %1").arg(link);
            return false;
        }
    }
    if (readText(shellConfig(paths)).contains(QString("\"%1\"").arg(BarWidget)))
        return true;
    // The shell only knows a plugin after scanning for it; until then
    // omarchy refuses to place it.
    run("omarchy-shell", {"shell", "rescanPlugins"}, nullptr, 10000);
    QString output;
    int code = -1;
    for (int attempt = 0; attempt < 20 && code != 0; ++attempt) {
        if (attempt > 0)
            QThread::msleep(250);
        code = run("omarchy", {"plugin", "enable", BarWidget, "--before", "omarchy.tailscale"}, &output);
        if (code != 0 && !output.contains("not a known"))
            code = run("omarchy", {"plugin", "enable", BarWidget, "--section", "right", "--index", "0"}, &output);
    }
    if (code != 0 && error)
        *error = output.trimmed();
    return code == 0;
}

void removeBarWidget(const Paths &paths) {
    if (readText(shellConfig(paths)).contains(QString("\"%1\"").arg(BarWidget)))
        run("omarchy", {"plugin", "disable", BarWidget});
    QFile::remove(barPluginLink(paths));
}

void openInEditor(const QString &file) {
    if (!which("omarchy-launch-editor").isEmpty())
        QProcess::startDetached("omarchy-launch-editor", {file});
    else
        QProcess::startDetached("xdg-open", {file});
}

int unprivilegedPortStart() {
    return readText("/proc/sys/net/ipv4/ip_unprivileged_port_start").trimmed().toInt();
}

bool ipv6Loopback() {
    const QStringList lines = readText("/proc/net/if_inet6").split('\n');
    for (const QString &line : lines)
        if (line.startsWith("00000000000000000000000000000001") && line.trimmed().endsWith(" lo"))
            return true;
    return false;
}

bool rootSetupNeeded(const Paths &paths) {
    return !Certs::trustedBySystem(paths) || unprivilegedPortStart() > 80 || !QFileInfo::exists(SysctlFile);
}

bool runRootSetup(const Paths &paths, bool remove, QString *error) {
    QStringList arguments{loftExecutable(), "root-setup"};
    if (remove)
        arguments << "--remove";
    else
        arguments << "--ca" << Certs::caCert(paths);
    QString output;
    const int code = run("pkexec", arguments, &output, 10 * 60 * 1000);
    if (code != 0 && error)
        *error = code == 126 || code == 127 ? tr("The password prompt was dismissed") : output.trimmed();
    return code == 0;
}

int rootSetup(const QStringList &arguments) {
    if (geteuid() != 0) {
        fprintf(stderr, "root-setup must run as root (through pkexec)\n");
        return 1;
    }
    const QString uid = qEnvironmentVariable("PKEXEC_UID", "0");
    const QString anchor = "/etc/ca-certificates/trust-source/anchors/loft-" + uid + ".crt";

    if (arguments.contains("--remove")) {
        QFile::remove(anchor);
        QFile::remove(SysctlFile);
        run("update-ca-trust", {});
        return 0;
    }

    const int index = arguments.indexOf("--ca");
    const QString ca = index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : QString();
    QString output;
    // Only ever a certificate authority: refuse anything else handed in.
    if (ca.isEmpty() || run("openssl", {"x509", "-in", ca, "-noout", "-ext", "basicConstraints"}, &output) != 0
        || !output.contains("CA:TRUE")) {
        fprintf(stderr, "not a CA certificate: %s\n", qPrintable(ca));
        return 1;
    }
    QFile::remove(anchor);
    if (!QFile::copy(ca, anchor)) {
        fprintf(stderr, "can't write %s\n", qPrintable(anchor));
        return 1;
    }
    QFile::setPermissions(anchor, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (run("update-ca-trust", {}, &output) != 0) {
        fprintf(stderr, "update-ca-trust: %s\n", qPrintable(output));
        return 1;
    }

    writeIfChanged(SysctlFile, "# Loft: nginx runs as your user and serves ports 80 and 443.\n"
                               "net.ipv4.ip_unprivileged_port_start=80\n");
    if (unprivilegedPortStart() > 80)
        run("sysctl", {"-q", "-w", "net.ipv4.ip_unprivileged_port_start=80"});
    return 0;
}

void notify(const QString &title, const QString &body) {
    run("notify-send", {"-a", "Loft", "-i", "loft", title, body}, nullptr, 10000);
}

} // namespace System
