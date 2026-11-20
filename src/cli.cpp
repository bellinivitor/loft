// Commands print plain text, or JSON with --json, and exit 0 or 1 with
// errors on stderr, like Hype's and Depot's.

#include "cli.h"

#include "certs.h"
#include "dnsserver.h"
#include "loft.h"
#include "runtimes.h"
#include "system.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QTextStream>
#include <cstring>
#include <unistd.h>

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

QString tr(const char *text) { return QCoreApplication::translate("loft", text); }

int fail(const QString &message) {
    err << "loft: " << message.trimmed() << Qt::endl;
    return 1;
}

int report(const Result &result) {
    if (!result.ok)
        return fail(result.message);
    if (!result.message.isEmpty())
        out << result.message << Qt::endl;
    return 0;
}

void printJson(const QJsonValue &value) {
    out << (value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject()))
               .toJson(QJsonDocument::Indented);
    out.flush();
}

QString tilde(const QString &path) {
    const QString home = QDir::homePath();
    return path == home || path.startsWith(home + "/") ? "~" + path.mid(home.length()) : path;
}

// One change at a time across every loft process (the window runs commands
// too, and parked folders trigger refreshes), each starting from the config
// as the last one left it.
bool lockAndReload(Loft &loft, std::unique_ptr<QLockFile> &lock) {
    QDir().mkpath(loft.paths().state);
    lock = std::make_unique<QLockFile>(loft.paths().state + "/lock");
    lock->setStaleLockTime(0);
    if (!lock->tryLock(120000))
        return false;
    loft.reload();
    return true;
}

// Arguments split into positionals and --flags (with optional =values).
struct Arguments {
    QStringList positional;
    QMap<QString, QString> flags;
    bool has(const QString &flag) const { return flags.contains(flag); }
    QString value(const QString &flag) const { return flags.value(flag); }
    QString at(int index) const { return positional.value(index); }
};

Arguments parse(const QStringList &raw) {
    Arguments arguments;
    for (const QString &argument : raw) {
        if (argument.startsWith("--")) {
            const int equals = argument.indexOf('=');
            if (equals > 0)
                arguments.flags.insert(argument.mid(2, equals - 2), argument.mid(equals + 1));
            else
                arguments.flags.insert(argument.mid(2), QString());
        } else {
            arguments.positional << argument;
        }
    }
    return arguments;
}

void usage() {
    out << tr("Usage: loft [command]") << "\n\n"
        << tr("Sites") << "\n"
        << "  link [name] [--secure] [--isolate=8.4]  " << tr("Serve this folder at name.test") << "\n"
        << "  unlink [name]                          " << tr("Stop serving a linked folder") << "\n"
        << "  links [--json]                         " << tr("List every site") << "\n"
        << "  park [folder]                          " << tr("Serve every folder inside this one") << "\n"
        << "  forget [folder]                        " << tr("Stop parking a folder") << "\n"
        << "  parked                                 " << tr("List parked folders") << "\n"
        << "  secure [name]                          " << tr("Serve a site over HTTPS") << "\n"
        << "  unsecure [name]                        " << tr("Back to plain HTTP") << "\n"
        << "  isolate <8.4|node@24> [--site=name]    " << tr("Pin a site to a PHP or Node version") << "\n"
        << "  unisolate [--node] [--site=name]       " << tr("Follow the default version again") << "\n"
        << "  isolated                               " << tr("List pinned sites") << "\n"
        << "  proxy <name> <url> [--secure]          " << tr("Serve name.test from another server") << "\n"
        << "  unproxy <name>                         " << tr("Remove a proxy") << "\n"
        << "  open [name]                            " << tr("Open a site in the browser") << "\n"
        << "\n" << tr("PHP and Node") << "\n"
        << "  use <8.4|node@24>                      " << tr("Set the default version (the terminal's too)") << "\n"
        << "  php:list, node:list [--json]           " << tr("Installed and available versions") << "\n"
        << "  php:install <8.4>, node:install <24>   " << tr("Install a version") << "\n"
        << "  php:update [8.4], node:update [24]     " << tr("Update to the newest patch release") << "\n"
        << "  php:uninstall <8.4>, node:uninstall    " << tr("Remove a version") << "\n"
        << "  php:restart [8.4]                      " << tr("Restart PHP-FPM after editing php.ini") << "\n"
        << "  which-php, which-node                  " << tr("The binary this folder uses") << "\n"
        << "  check-updates [--notify] [--json]      " << tr("Look for new PHP and Node releases") << "\n"
        << "  update                                 " << tr("Install every update") << "\n"
        << "\n" << tr("Loft itself") << "\n"
        << "  (" << tr("no command") << ")                           " << tr("Open the window") << "\n"
        << "  setup                                  " << tr("One-time setup (asks for your password once)") << "\n"
        << "  doctor [--json]                        " << tr("Check that everything works") << "\n"
        << "  start, stop, restart                   " << tr("Control nginx, DNS and PHP") << "\n"
        << "  set check-updates on|off               " << tr("Check for new releases daily") << "\n"
        << "  set php-build bulk|common              " << tr("Which PHP extension set to download") << "\n"
        << "  uninstall [--purge]                    " << tr("Undo setup (--purge deletes everything)") << "\n";
    out.flush();
}

QJsonObject siteJson(const Site &site, const Config &config) {
    return QJsonObject{
        {"name", site.name}, {"host", site.host}, {"url", site.url()}, {"path", site.path},
        {"root", site.root}, {"kind", site.kind}, {"source", site.source}, {"proxy", site.proxy},
        {"secure", site.secure}, {"php", site.php}, {"node", site.node},
        {"phpVersion", site.phpVersion}, {"nodeVersion", site.node.isEmpty() ? config.node : site.node},
        {"missing", site.missing},
    };
}

// A left-aligned table with a header row.
void table(const QStringList &header, const QList<QStringList> &rows) {
    QList<int> widths;
    for (const QString &cell : header)
        widths << cell.length();
    for (const QStringList &row : rows)
        for (int i = 0; i < row.size() && i < widths.size(); ++i)
            widths[i] = std::max<int>(widths[i], int(row[i].length()));
    auto line = [&](const QStringList &cells) {
        QString text;
        for (int i = 0; i < cells.size(); ++i)
            text += i + 1 < cells.size() ? cells[i].leftJustified(widths[i] + 2) : cells[i];
        while (text.endsWith(' '))
            text.chop(1);
        out << text << "\n";
    };
    line(header);
    for (const QStringList &row : rows)
        line(row);
    out.flush();
}

// The window reads machine lines; a terminal gets a percentage that redraws.
Runtimes::Progress progressPrinter(bool machine, const QString &label) {
    const bool terminal = isatty(STDERR_FILENO);
    return [machine, label, terminal, last = -1](qint64 received, qint64 total) mutable {
        if (machine) {
            out << "progress " << received << " " << total << Qt::endl;
            return;
        }
        if (!terminal || total <= 0)
            return;
        const int percent = int(received * 100 / total);
        if (percent == last)
            return;
        last = percent;
        err << "\r" << label << " " << percent << "%   ";
        err.flush();
        if (percent >= 100)
            err << "\r" << QString(label.length() + 8, ' ') << "\r";
    };
}

Catalog catalog(const Loft &loft, bool refresh, QString *error) {
    Catalog cached = Runtimes::cachedCatalog(loft.paths());
    if (!refresh && cached.fetched.isValid() && cached.fetched.secsTo(QDateTime::currentDateTimeUtc()) < 6 * 3600)
        return cached;
    Catalog fresh = Runtimes::fetchCatalog(loft.config().phpBuild, error);
    if (fresh.php.isEmpty())
        return cached;
    Runtimes::saveCatalog(loft.paths(), fresh);
    return fresh;
}

int installRuntime(Loft &loft, const QString &tool, QString line, const Arguments &arguments) {
    if (line.startsWith(tool + "@"))
        line = line.mid(tool.length() + 1);
    line = tool == "php" ? line.section('.', 0, 1) : line.section('.', 0, 0);
    QString error;
    const Catalog list = catalog(loft, true, &error);
    const bool machine = arguments.has("progress");
    if (tool == "php") {
        if (!list.php.contains(line))
            return fail(error.isEmpty() ? tr("There's no PHP %1 build. Available: %2")
                                              .arg(line, QStringList(list.php.keys()).join(", "))
                                        : error);
        const PhpRelease release = list.php.value(line);
        if (loft.installedPhp().value(line) == release.version && !arguments.has("force"))
            return report(Result::done(tr("PHP %1 is already the newest").arg(release.version)));
        const QString label = tr("Downloading PHP %1…").arg(release.version);
        if (!machine)
            err << label << Qt::endl;
        if (!Runtimes::installPhp(loft.paths(), release, loft.config().phpBuild, progressPrinter(machine, label),
                                  &error))
            return fail(error);
    } else {
        const NodeRelease *release = list.nodeRelease(line);
        if (!release) {
            QStringList majors;
            for (const NodeRelease &node : list.node)
                majors << node.major;
            return fail(error.isEmpty() ? tr("There's no Node %1 release. Available: %2").arg(line, majors.join(", "))
                                        : error);
        }
        if (loft.installedNode().value(line) == release->version && !arguments.has("force"))
            return report(Result::done(tr("Node %1 is already the newest").arg(release->version)));
        const QString label = tr("Downloading Node %1…").arg(release->version);
        if (!machine)
            err << label << Qt::endl;
        if (!Runtimes::installNode(loft.paths(), *release, progressPrinter(machine, label), &error))
            return fail(error);
    }
    std::unique_ptr<QLockFile> lock;
    if (!lockAndReload(loft, lock))
        return fail(tr("Another loft command is still running"));
    return report(loft.installedRuntime(tool, line));
}

int listRuntimes(const Loft &loft, const QString &tool, const Arguments &arguments) {
    QString error;
    const Catalog list = catalog(loft, arguments.has("refresh"), &error);
    const auto installed = tool == "php" ? loft.installedPhp() : loft.installedNode();
    const QString fallback = tool == "php" ? loft.config().php : loft.config().node;

    QStringList lines;
    QMap<QString, QString> latest, lts;
    if (tool == "php") {
        for (const PhpRelease &release : list.php)
            latest.insert(release.minor, release.version);
    } else {
        for (const NodeRelease &release : list.node) {
            latest.insert(release.major, release.version);
            lts.insert(release.major, release.lts);
        }
    }
    for (const QString &line : installed.keys() + latest.keys())
        if (!lines.contains(line))
            lines << line;
    std::sort(lines.begin(), lines.end(), [](const QString &a, const QString &b) { return compareVersions(a, b) > 0; });

    QMap<QString, int> uses;
    for (const Site &site : loft.sites()) {
        const QString used = tool == "php" ? (site.usesPhp() ? site.phpVersion : QString())
                                           : (site.node.isEmpty() ? fallback : site.node);
        if (!used.isEmpty() && site.kind != "proxy")
            uses[used] += 1;
    }

    if (arguments.has("json")) {
        QJsonArray array;
        for (const QString &line : lines)
            array.append(QJsonObject{{"line", line}, {"installed", installed.value(line)},
                                     {"latest", latest.value(line)}, {"lts", lts.value(line)},
                                     {"default", line == fallback}, {"sites", uses.value(line)}});
        printJson(array);
        return 0;
    }
    QList<QStringList> rows;
    for (const QString &line : lines) {
        QString state = installed.contains(line) ? installed.value(line) : QString("-");
        if (installed.contains(line) && latest.contains(line) && compareVersions(latest[line], installed[line]) > 0)
            state += "  → " + latest[line];
        rows << QStringList{(line == fallback ? "* " : "  ") + line, state,
                            installed.contains(line) ? QString() : latest.value(line),
                            lts.value(line)};
    }
    table({"  " + tr("VERSION"), tr("INSTALLED"), tr("AVAILABLE"), tool == "node" ? "LTS" : ""}, rows);
    if (!error.isEmpty())
        err << tr("(offline: %1)").arg(error) << Qt::endl;
    return 0;
}

int checkUpdates(Loft &loft, const Arguments &arguments) {
    QString error;
    const Catalog list = catalog(loft, true, &error);
    if (!list.fetched.isValid())
        return fail(error);
    const QList<Update> updates = Runtimes::updates(loft.paths(), list);

    QFile file(loft.paths().updatesFile());
    QJsonObject previous;
    if (file.open(QIODevice::ReadOnly))
        previous = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QStringList notified;
    for (const auto &value : previous.value("notified").toArray())
        notified << value.toString();

    QJsonArray array;
    QStringList fresh, names;
    for (const Update &update : updates) {
        const QString key = update.tool + " " + update.latest;
        const QString label = (update.tool == "php" ? "PHP " : "Node ") + update.latest;
        array.append(QJsonObject{{"tool", update.tool}, {"line", update.line}, {"installed", update.installed},
                                 {"latest", update.latest}});
        names << label;
        if (!notified.contains(key)) {
            fresh << label;
            notified << key;
        }
    }
    const QJsonObject state{{"checked", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                            {"updates", array}, {"notified", QJsonArray::fromStringList(notified)}};
    System::writeIfChanged(loft.paths().updatesFile(), QJsonDocument(state).toJson());

    if (arguments.has("notify") && !fresh.isEmpty())
        System::notify(tr("Updates for Loft"), tr("%1. Update with: loft update").arg(fresh.join(", ")));
    if (arguments.has("json")) {
        printJson(state);
        return 0;
    }
    out << (names.isEmpty() ? tr("Everything is up to date") : tr("Updates: %1").arg(names.join(", ")))
        << Qt::endl;
    return 0;
}

int updateAll(Loft &loft, const QString &onlyTool, const QString &onlyLine, const Arguments &arguments) {
    QString error;
    const Catalog list = catalog(loft, true, &error);
    if (!list.fetched.isValid())
        return fail(error);
    int code = 0, count = 0;
    for (const Update &update : Runtimes::updates(loft.paths(), list)) {
        if ((!onlyTool.isEmpty() && update.tool != onlyTool) || (!onlyLine.isEmpty() && update.line != onlyLine))
            continue;
        code = std::max(code, installRuntime(loft, update.tool, update.line, arguments));
        ++count;
    }
    if (count == 0)
        out << tr("Everything is up to date") << Qt::endl;
    return code;
}

int doctor(Loft &loft, const Arguments &arguments) {
    const QList<Check> checks = loft.doctor();
    bool ok = true;
    QJsonArray array;
    for (const Check &check : checks) {
        ok = ok && (check.ok || check.id == "node");
        array.append(QJsonObject{{"id", check.id}, {"label", check.label}, {"ok", check.ok},
                                 {"detail", check.detail}, {"needsSetup", check.needsSetup}});
    }
    if (arguments.has("json")) {
        printJson(array);
        return 0;
    }
    for (const Check &check : checks)
        out << (check.ok ? "  ✓ " : "  ✗ ") << check.label << (check.detail.isEmpty() ? "" : "  — " + check.detail)
            << "\n";
    bool fixable = false;
    for (const Check &check : checks)
        fixable = fixable || (!check.ok && check.needsSetup);
    if (fixable)
        out << "\n" << tr("Run loft setup to fix what's marked.") << "\n";
    out.flush();
    return ok ? 0 : 1;
}

int services(const Loft &loft, const QString &action) {
    QStringList units{"loft-nginx.service", "loft-dns.service"};
    for (const QString &minor : System::enabledPhpVersions(loft.paths()))
        units << "loft-php@" + minor + ".service";
    QString output;
    if (System::systemctl(QStringList{action} + units, &output) != 0)
        return fail(output);
    out << (action == "stop" ? tr("Stopped nginx, DNS and PHP") : tr("nginx, DNS and PHP are running")) << Qt::endl;
    return 0;
}

int runDns(const Loft &loft) {
    DnsServer server(loft.config().tld, System::ipv6Loopback());
    QString error;
    if (!server.listen(System::DnsPort, &error))
        return fail(tr("DNS can't listen on 127.0.0.1:%1: %2").arg(System::DnsPort).arg(error));
    out << tr("Answering *.%1 on 127.0.0.1:%2").arg(loft.config().tld).arg(System::DnsPort) << Qt::endl;
    return QCoreApplication::exec();
}

} // namespace

bool wantsWindow(int argc, char *argv[]) {
    return argc < 2 || std::strcmp(argv[1], "app") == 0;
}

int runCli(const QStringList &all) {
    const QString command = all.value(1);
    const Arguments arguments = parse(all.mid(2));

    if (command == "root-setup")
        return System::rootSetup(all.mid(2));
    if (command == "help" || command == "--help" || command == "-h") {
        usage();
        return 0;
    }
    if (command == "--version" || command == "version") {
        out << "loft " << QCoreApplication::applicationVersion() << Qt::endl;
        return 0;
    }

    Loft loft;
    const QString first = arguments.at(0);
    const QString siteFlag = arguments.value("site");

    if (command == "dns-server")
        return runDns(loft);

    static const QStringList mutating{"setup", "uninstall", "refresh", "apply", "link", "unlink", "park",
                                      "forget", "secure", "unsecure", "isolate", "unisolate", "proxy", "unproxy",
                                      "use", "php:uninstall", "node:uninstall", "set"};
    std::unique_ptr<QLockFile> lock;
    if (mutating.contains(command) && !lockAndReload(loft, lock))
        return fail(tr("Another loft command is still running"));
    if (command == "setup") {
        const Result result = loft.setup(!arguments.has("no-root"), [](const QString &label, bool ok, const QString &detail) {
            out << (ok ? "  ✓ " : "  ✗ ") << label << (detail.isEmpty() ? "" : "  — " + detail.trimmed()) << Qt::endl;
        });
        return report(result);
    }
    if (command == "uninstall") {
        const Result result = loft.uninstall(arguments.has("purge"), [](const QString &label, bool ok, const QString &detail) {
            out << (ok ? "  ✓ " : "  ✗ ") << label << (detail.isEmpty() ? "" : "  — " + detail.trimmed()) << Qt::endl;
        });
        return report(result);
    }
    if (command == "doctor" || command == "status")
        return doctor(loft, arguments);
    if (command == "refresh" || command == "apply")
        return report(loft.apply());
    if (command == "start" || command == "stop" || command == "restart")
        return services(loft, command);

    if (command == "link")
        return report(loft.link(QDir::currentPath(), first, arguments.has("secure"), arguments.value("isolate")));
    if (command == "unlink") {
        QString name = first, error;
        if (name.isEmpty()) {
            const auto found = loft.target({}, &error);
            if (!found)
                return fail(error);
            name = found->name;
        }
        return report(loft.unlink(name));
    }
    if (command == "links" || command == "sites" || command == "secured" || command == "isolated"
        || command == "proxies") {
        QList<Site> sites = loft.sites();
        if (command == "secured" || command == "isolated" || command == "proxies") {
            QList<Site> kept;
            for (const Site &site : sites)
                if ((command == "secured" && site.secure) || (command == "proxies" && site.kind == "proxy")
                    || (command == "isolated" && (!site.php.isEmpty() || !site.node.isEmpty())))
                    kept << site;
            sites = kept;
        }
        if (arguments.has("json")) {
            QJsonArray array;
            for (const Site &site : sites)
                array.append(siteJson(site, loft.config()));
            printJson(array);
            return 0;
        }
        if (sites.isEmpty()) {
            out << tr("No sites yet. In a project folder, run: loft link") << Qt::endl;
            return 0;
        }
        QList<QStringList> rows;
        for (const Site &site : sites) {
            QString php = site.usesPhp() ? site.phpVersion + (site.php.isEmpty() ? "" : " ●") : "-";
            QString node = site.node.isEmpty() ? loft.config().node : site.node + " ●";
            rows << QStringList{site.url(), site.kind == "proxy" ? "-" : php,
                                site.kind == "proxy" ? "-" : node,
                                site.kind == "proxy" ? "→ " + site.proxy
                                                     : tilde(site.path) + (site.missing ? " " + tr("(missing)") : "")};
        }
        table({tr("SITE"), "PHP", "NODE", tr("FOLDER")}, rows);
        return 0;
    }
    if (command == "park")
        return report(loft.park(first.isEmpty() ? QDir::currentPath() : first));
    if (command == "forget")
        return report(loft.forget(first.isEmpty() ? QDir::currentPath() : first));
    if (command == "parked" || command == "paths") {
        if (arguments.has("json")) {
            printJson(QJsonArray::fromStringList(loft.config().parked));
            return 0;
        }
        for (const QString &folder : loft.config().parked)
            out << tilde(folder) << "\n";
        if (loft.config().parked.isEmpty())
            out << tr("No parked folders. Park one with: loft park ~/Sites") << "\n";
        out.flush();
        return 0;
    }
    if (command == "secure" || command == "unsecure")
        return report(loft.secure(first, command == "secure"));
    if (command == "isolate") {
        if (first.isEmpty())
            return fail(tr("Which version? For example: loft isolate 8.4, or loft isolate node@24"));
        const bool node = first.startsWith("node@") || arguments.has("node");
        return report(loft.isolate(siteFlag.isEmpty() ? arguments.at(1) : siteFlag, node ? "node" : "php", first));
    }
    if (command == "unisolate")
        return report(loft.isolate(siteFlag.isEmpty() ? first : siteFlag, arguments.has("node") ? "node" : "php", {}));
    if (command == "proxy") {
        if (arguments.positional.size() < 2)
            return fail(tr("Usage: loft proxy <name> <url>, e.g. loft proxy app http://127.0.0.1:3000"));
        return report(loft.proxy(first, arguments.at(1), arguments.has("secure")));
    }
    if (command == "unproxy")
        return report(loft.unproxy(first));
    if (command == "open") {
        QString error;
        const auto found = loft.target(first, &error);
        if (!found)
            return fail(error);
        QProcess::startDetached("xdg-open", {found->url()});
        return 0;
    }
    if (command == "use") {
        if (first.isEmpty())
            return fail(tr("Which version? For example: loft use 8.4, or loft use node@24"));
        return report(loft.useDefault(first.startsWith("node@") ? "node" : "php", first));
    }
    if (command == "which-php" || command == "which-node") {
        const bool php = command == "which-php";
        QString error;
        const auto found = loft.target(siteFlag, &error);
        QString line = php ? loft.config().php : loft.config().node;
        if (found && !(php ? found->php : found->node).isEmpty())
            line = php ? found->php : found->node;
        const QString binary = php ? loft.paths().phpDir(line) + "/php" : loft.paths().nodeDir(line) + "/bin/node";
        if (line.isEmpty() || !QFileInfo(binary).isExecutable())
            return fail(php ? tr("No PHP installed yet: loft php:install 8.5")
                            : tr("No Node installed yet: loft node:install 24"));
        out << binary << Qt::endl;
        return 0;
    }

    if (command == "set") {
        const QString value = arguments.at(1);
        if (first == "check-updates" && (value == "on" || value == "off"))
            return report(loft.setCheckUpdates(value == "on"));
        if (first == "php-build")
            return report(loft.setPhpBuild(value));
        return fail(tr("Usage: loft set check-updates on|off, or loft set php-build bulk|common"));
    }
    if (command == "php:list" || command == "node:list")
        return listRuntimes(loft, command.section(':', 0, 0), arguments);
    if (command == "php:install" || command == "node:install") {
        if (first.isEmpty())
            return fail(tr("Which version? For example: loft %1 %2")
                            .arg(command, command.startsWith("php") ? "8.5" : "24"));
        return installRuntime(loft, command.section(':', 0, 0), first, arguments);
    }
    if (command == "php:update" || command == "node:update")
        return updateAll(loft, command.section(':', 0, 0), first, arguments);
    if (command == "update")
        return updateAll(loft, {}, {}, arguments);
    if (command == "php:uninstall" || command == "node:uninstall") {
        if (first.isEmpty())
            return fail(tr("Which version?"));
        const QString tool = command.section(':', 0, 0);
        const QString line = tool == "php" ? first.section('.', 0, 1) : first.section('.', 0, 0);
        return report(loft.uninstallRuntime(tool, line));
    }
    if (command == "php:restart")
        return report(loft.restartPhp(first.isEmpty() ? loft.config().php : first));
    if (command == "check-updates")
        return checkUpdates(loft, arguments);
    if (command == "catalog") {
        QString error;
        const Catalog list = catalog(loft, arguments.has("refresh"), &error);
        QJsonObject object = list.toJson();
        object.insert("error", error);
        printJson(object);
        return list.fetched.isValid() ? 0 : 1;
    }

    err << tr("loft: unknown command %1").arg(command) << "\n\n";
    err.flush();
    usage();
    return 1;
}
