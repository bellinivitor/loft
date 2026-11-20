#include "mcp.h"

#include "loft.h"
#include "runtimes.h"
#include "system.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

const char *Instructions =
    "Loft serves local PHP and Node projects on this machine at http(s)://<name>.test through nginx, which runs "
    "as the user, with a local certificate authority the system and browsers trust. It keeps several PHP "
    "versions (static builds, each with its own PHP-FPM) and Node versions side by side, and any site can be "
    "isolated to its own PHP or Node version.\n"
    "The php, php8.x, composer, node, npm, npx and corepack commands on the user's PATH are Loft's: they pick "
    "the version by folder (the site's isolated version, then for Node the project's .nvmrc or .node-version "
    "when that major is installed, then the default). Call which_runtime with a project folder to see which "
    "PHP and Node apply there before running commands in it.\n"
    "Start with get_status or list_sites. link_site serves a folder (Laravel projects from public/); "
    "secure_site turns on HTTPS. Ask the user before unlink_site, remove_proxy, uninstall_version, "
    "control_services with stop, set_default_version (it changes the PHP or Node their terminal runs), and "
    "before replacing a site's nginx config they didn't ask to change. install_version downloads 15 to 45 MB "
    "and can take a minute. run_setup may show the user a password dialog.";

QJsonObject schema(const QJsonObject &properties, const QStringList &required = {}) {
    QJsonObject result{{"type", "object"}, {"properties", properties}, {"additionalProperties", false}};
    if (!required.isEmpty())
        result["required"] = QJsonArray::fromStringList(required);
    return result;
}

QJsonObject text(const QString &description) { return {{"type", "string"}, {"description", description}}; }
QJsonObject flag(const QString &description) { return {{"type", "boolean"}, {"description", description}}; }

QJsonObject choice(const QStringList &values, const QString &description) {
    return {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}, {"description", description}};
}

const QJsonObject SiteName = text("The site's name, as list_sites shows it (\"blog\" or \"blog.test\").");
const QJsonObject Tool = choice({"php", "node"}, "Which runtime.");

QJsonObject hints(const QString &title, bool readOnly, bool destructive, bool idempotent, bool openWorld = false) {
    return {{"title", title}, {"readOnlyHint", readOnly}, {"destructiveHint", destructive},
            {"idempotentHint", idempotent}, {"openWorldHint", openWorld}};
}

QJsonObject tool(const QString &name, const QString &title, const QString &description, const QJsonObject &input,
                 const QJsonObject &annotations) {
    return {{"name", name}, {"title", title}, {"description", description}, {"inputSchema", input},
            {"annotations", annotations}};
}

// A tool's answer: the data as JSON text for every client, and as
// structured content for clients that read it.
QJsonObject result(const QJsonObject &data) {
    return {{"content", QJsonArray{QJsonObject{
                {"type", "text"}, {"text", QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented))}}}},
            {"structuredContent", data},
            {"isError", false}};
}

// A failure the agent can act on goes back as a tool result, not a protocol
// error, so the model reads the sentence.
QJsonObject failure(const QString &message) {
    return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", message.trimmed()}}}}, {"isError", true}};
}

QJsonObject outcome(const Result &done, const QJsonObject &extra = {}) {
    if (!done.ok)
        return failure(done.message);
    QJsonObject data = extra;
    data["ok"] = true;
    data["message"] = done.message.trimmed();
    return result(data);
}

QJsonObject describeSite(const Loft &loft, const Site &site) {
    const Config &config = loft.config();
    const Paths &paths = loft.paths();
    return {
        {"name", site.name},
        {"host", site.host},
        {"url", site.url()},
        {"folder", site.path},
        {"document_root", site.root},
        {"kind", site.kind},
        {"source", site.source},
        {"proxy_to", site.proxy},
        {"secure", site.secure},
        {"php", site.usesPhp() ? site.phpVersion : QString()},
        {"php_isolated", !site.php.isEmpty()},
        {"node", site.node.isEmpty() ? config.node : site.node},
        {"node_isolated", !site.node.isEmpty()},
        {"custom_nginx_config", site.customNginx},
        {"nginx_config_file", site.customNginx ? paths.nginxOverride(site.host)
                                               : paths.nginx() + "/sites/" + site.host + ".conf"},
        {"folder_missing", site.missing},
    };
}

QJsonArray versions(const Loft &loft, const QString &which, const Catalog &catalog) {
    const bool php = which == "php";
    const auto installed = php ? loft.installedPhp() : loft.installedNode();
    const QString fallback = php ? loft.config().php : loft.config().node;
    QMap<QString, QString> latest, lts;
    if (php) {
        for (const PhpRelease &release : catalog.php)
            latest.insert(release.minor, release.version);
    } else {
        for (const NodeRelease &release : catalog.node) {
            latest.insert(release.major, release.version);
            lts.insert(release.major, release.lts);
        }
    }
    QStringList lines = installed.keys();
    for (const QString &line : latest.keys())
        if (!lines.contains(line))
            lines << line;
    std::sort(lines.begin(), lines.end(), [](const QString &a, const QString &b) { return compareVersions(a, b) > 0; });

    QJsonArray array;
    for (const QString &line : lines) {
        const QString version = installed.value(line);
        QJsonObject entry{
            {"line", line},
            {"installed", version},
            {"latest", latest.value(line)},
            {"default", line == fallback},
            {"update_available", !version.isEmpty() && latest.contains(line)
                                     && compareVersions(latest.value(line), version) > 0},
        };
        if (!php)
            entry["lts"] = lts.value(line);
        if (!version.isEmpty()) {
            entry["binary"] = php ? loft.paths().phpDir(line) + "/php" : loft.paths().nodeDir(line) + "/bin/node";
            if (php)
                entry["php_ini"] = loft.paths().phpConfigDir(line) + "/php.ini";
        }
        array.append(entry);
    }
    return array;
}

QJsonObject callTool(Loft &loft, const QString &name, const QJsonObject &args) {
    const QString site = args.value("name").toString();
    const QString which = args.value("tool").toString();

    // Read-only tools see the config as it is now; changes lock it first.
    auto locked = [&]() { return loft.lock(); };

    if (name == "get_status") {
        loft.reload();
        const Config &config = loft.config();
        QStringList units{"loft-nginx.service", "loft-dns.service"};
        if (!config.php.isEmpty())
            units << "loft-php@" + config.php + ".service";
        const auto states = System::unitStates(units);
        const auto php = loft.installedPhp();
        const auto node = loft.installedNode();
        return result({
            {"nginx", states.value("loft-nginx.service")},
            {"dns", states.value("loft-dns.service")},
            {"php_fpm", config.php.isEmpty() ? QString() : states.value("loft-php@" + config.php + ".service")},
            {"tld", config.tld},
            {"default_php", config.php.isEmpty() ? QString() : php.value(config.php, config.php)},
            {"default_node", config.node.isEmpty() ? QString() : node.value(config.node, config.node)},
            {"installed_php", QJsonArray::fromStringList(php.values())},
            {"installed_node", QJsonArray::fromStringList(node.values())},
            {"sites", int(loft.sites().size())},
            {"parked_folders", QJsonArray::fromStringList(config.parked)},
            {"php_build", config.phpBuild},
            {"daily_update_check", config.checkUpdates},
        });
    }

    if (name == "list_sites") {
        loft.reload();
        QJsonArray list;
        for (const Site &each : loft.sites())
            list.append(describeSite(loft, each));
        return result({{"sites", list}});
    }

    if (name == "which_runtime") {
        loft.reload();
        const QString directory = args.value("directory").toString();
        if (directory.isEmpty() || !QFileInfo(directory).isDir())
            return failure("Give an existing folder as directory.");
        const RuntimeChoice runtime = loft.runtimeFor(directory);
        return result({
            {"directory", QDir(directory).absolutePath()},
            {"site", runtime.site},
            {"php", runtime.phpVersion}, {"php_line", runtime.php}, {"php_binary", runtime.phpBinary},
            {"php_from", runtime.phpFrom},
            {"node", runtime.nodeVersion}, {"node_line", runtime.node}, {"node_binary", runtime.nodeBinary},
            {"node_from", runtime.nodeFrom},
            {"note", "Inside this folder, php, composer, node, npm and npx already run these versions."},
        });
    }

    if (name == "list_versions") {
        loft.reload();
        if (which != "php" && which != "node")
            return failure("Say which: tool=\"php\" or tool=\"node\".");
        QString error;
        const Catalog catalog = loft.catalog(args.value("refresh").toBool(false), &error);
        QJsonObject data{{"tool", which}, {"versions", versions(loft, which, catalog)}};
        if (which == "php")
            data["build"] = loft.config().phpBuild;
        if (!error.isEmpty())
            data["warning"] = "Couldn't reach the version lists (" + error + "); available versions may be stale.";
        return result(data);
    }

    if (name == "check_updates") {
        loft.reload();
        QString error;
        const Catalog catalog = loft.catalog(true, &error);
        if (!catalog.fetched.isValid())
            return failure(error);
        QJsonArray list;
        for (const Update &update : Runtimes::updates(loft.paths(), catalog))
            list.append(QJsonObject{{"tool", update.tool}, {"line", update.line}, {"installed", update.installed},
                                    {"latest", update.latest}});
        return result({{"updates", list}});
    }

    if (name == "doctor") {
        loft.reload();
        QJsonArray list;
        bool healthy = true;
        for (const Check &check : loft.doctor()) {
            healthy = healthy && (check.ok || check.id == "node");
            list.append(QJsonObject{{"id", check.id}, {"label", check.label}, {"ok", check.ok},
                                    {"detail", check.detail}, {"fixed_by_setup", check.needsSetup}});
        }
        return result({{"healthy", healthy}, {"checks", list}});
    }

    if (name == "get_nginx_config") {
        loft.reload();
        QString error;
        const auto found = loft.target(site, &error);
        if (site.isEmpty() || !found)
            return failure(site.isEmpty() ? "Give the site's name." : error);
        const QString file = found->customNginx ? loft.paths().nginxOverride(found->host)
                                                : loft.paths().nginx() + "/sites/" + found->host + ".conf";
        QFile config(file);
        if (!config.open(QIODevice::ReadOnly))
            return failure("No nginx config for " + found->host + " yet.");
        return result({{"name", found->name}, {"custom", found->customNginx}, {"file", file},
                       {"config", QString::fromUtf8(config.readAll())}});
    }

    if (name == "install_version") {
        if (which != "php" && which != "node")
            return failure("Say which: tool=\"php\" or tool=\"node\".");
        const QString version = args.value("version").toString();
        if (version.isEmpty())
            return failure("Give the version line, like \"8.4\" for PHP or \"24\" for Node.");
        loft.reload();
        return outcome(loft.install(which, version, args.value("force").toBool(false)));
    }

    if (name == "update_versions") {
        loft.reload();
        QString error;
        const Catalog catalog = loft.catalog(true, &error);
        if (!catalog.fetched.isValid())
            return failure(error);
        QJsonArray done, failed;
        for (const Update &update : Runtimes::updates(loft.paths(), catalog)) {
            if ((!which.isEmpty() && update.tool != which)
                || (args.contains("version") && update.line != args.value("version").toString()))
                continue;
            const Result installed = loft.install(update.tool, update.line, false);
            (installed.ok ? done : failed)
                .append(QJsonObject{{"tool", update.tool}, {"from", update.installed}, {"to", update.latest},
                                    {"message", installed.message.trimmed()}});
        }
        if (!failed.isEmpty())
            return failure(QString::fromUtf8(QJsonDocument(QJsonObject{{"updated", done}, {"failed", failed}})
                                                 .toJson(QJsonDocument::Indented)));
        return result({{"updated", done}, {"note", done.isEmpty() ? "Everything was up to date." : ""}});
    }

    if (name == "open_site") {
        loft.reload();
        QString error;
        const auto found = loft.target(site, &error);
        if (site.isEmpty() || !found)
            return failure(site.isEmpty() ? "Give the site's name." : error);
        QProcess::startDetached("xdg-open", {found->url()});
        return result({{"opened", found->url()}});
    }

    // Everything below changes something: one loft process at a time.
    const auto held = locked();
    if (!held)
        return failure("Another loft command is still running; try again in a moment.");

    auto siteResult = [&](const Result &done, const QString &lookup) {
        if (!done.ok)
            return failure(done.message);
        QJsonObject data{{"ok", true}, {"message", done.message.trimmed()}};
        if (const auto found = loft.site(lookup))
            data["site"] = describeSite(loft, *found);
        return result(data);
    };

    if (name == "link_site") {
        const QString directory = args.value("directory").toString();
        if (directory.isEmpty())
            return failure("Give the project folder as directory.");
        const Result done = loft.link(directory, site, args.value("secure").toBool(false),
                                      args.value("php").toString());
        QString linked = site;
        if (linked.isEmpty())
            linked = siteName(QFileInfo(QDir::cleanPath(directory)).fileName());
        return siteResult(done, linked);
    }
    if (site.isEmpty() && (name == "unlink_site" || name == "secure_site" || name == "isolate_site"
                           || name == "add_proxy" || name == "remove_proxy" || name == "set_nginx_config"
                           || name == "reset_nginx_config"))
        return failure("Give the site's name.");

    if (name == "unlink_site")
        return outcome(loft.unlink(site));
    if (name == "secure_site") {
        if (!args.contains("secure"))
            return failure("Say secure=true for HTTPS or secure=false for plain HTTP.");
        return siteResult(loft.secure(site, args.value("secure").toBool()), site);
    }
    if (name == "isolate_site") {
        if (which != "php" && which != "node")
            return failure("Say which: tool=\"php\" or tool=\"node\".");
        return siteResult(loft.isolate(site, which, args.value("version").toString()), site);
    }
    if (name == "park_folder" || name == "forget_folder") {
        const QString directory = args.value("directory").toString();
        if (directory.isEmpty())
            return failure("Give the folder as directory.");
        return outcome(name == "park_folder" ? loft.park(directory) : loft.forget(directory));
    }
    if (name == "add_proxy")
        return siteResult(loft.proxy(site, args.value("url").toString(), args.value("secure").toBool(false)), site);
    if (name == "remove_proxy")
        return outcome(loft.unproxy(site));
    if (name == "set_default_version") {
        if (which != "php" && which != "node")
            return failure("Say which: tool=\"php\" or tool=\"node\".");
        return outcome(loft.useDefault(which, args.value("version").toString()));
    }
    if (name == "uninstall_version") {
        if (which != "php" && which != "node")
            return failure("Say which: tool=\"php\" or tool=\"node\".");
        QString line = args.value("version").toString();
        line = which == "php" ? line.section('.', 0, 1) : line.section('.', 0, 0);
        return outcome(loft.uninstallRuntime(which, line));
    }
    if (name == "restart_php") {
        const QString line = args.value("version").toString().section('.', 0, 1);
        return outcome(loft.restartPhp(line.isEmpty() ? loft.config().php : line));
    }
    if (name == "control_services") {
        const QString action = args.value("action").toString();
        if (action != "start" && action != "stop" && action != "restart")
            return failure("action is start, stop or restart.");
        QStringList units{"loft-nginx.service", "loft-dns.service"};
        for (const QString &minor : System::enabledPhpVersions(loft.paths()))
            units << "loft-php@" + minor + ".service";
        QString output;
        if (System::systemctl(QStringList{action} + units, &output) != 0)
            return failure(output);
        const auto states = System::unitStates(units);
        QJsonObject now;
        for (auto it = states.begin(); it != states.end(); ++it)
            now[QString(it.key()).remove(".service")] = it.value();
        return result({{"ok", true}, {"services", now}});
    }
    if (name == "set_nginx_config") {
        const QString config = args.value("config").toString();
        if (config.trimmed().isEmpty())
            return failure("Give the whole nginx config for the site as config.");
        QString error;
        const auto found = loft.target(site, &error);
        if (!found)
            return failure(error);
        // Validated before it stays: a config nginx rejects is put back the
        // way it was, so the site never ends up on a broken file.
        const QString file = loft.paths().nginxOverride(found->host);
        QFile previous(file);
        const bool had = previous.open(QIODevice::ReadOnly);
        const QByteArray before = had ? previous.readAll() : QByteArray();
        previous.close();
        if (!System::writeIfChanged(file, config.toUtf8()))
            return failure("Can't write " + file);
        const Result applied = loft.apply();
        if (!applied.ok && applied.message.contains(file)) {
            if (had)
                System::writeIfChanged(file, before);
            else
                QFile::remove(file);
            loft.apply();
            return failure("nginx rejected that config, so the previous one is back. " + applied.message);
        }
        return siteResult(applied.ok ? Result::done("Saved " + file + " and reloaded nginx.") : applied, site);
    }
    if (name == "reset_nginx_config")
        return siteResult(loft.nginxReset(site), site);
    if (name == "change_settings") {
        if (!args.contains("check_updates") && !args.contains("php_build"))
            return failure("Give check_updates, php_build, or both.");
        QStringList messages;
        if (args.contains("check_updates")) {
            const Result done = loft.setCheckUpdates(args.value("check_updates").toBool());
            if (!done.ok)
                return failure(done.message);
            messages << done.message;
        }
        if (args.contains("php_build")) {
            const Result done = loft.setPhpBuild(args.value("php_build").toString());
            if (!done.ok)
                return failure(done.message);
            messages << done.message;
        }
        return outcome(Result::done(messages.join('\n')));
    }
    if (name == "run_setup") {
        QJsonArray steps;
        const Result done = loft.setup(true, [&](const QString &label, bool ok, const QString &detail) {
            steps.append(QJsonObject{{"step", label}, {"ok", ok}, {"detail", detail.trimmed()}});
        });
        if (!done.ok)
            return failure(done.message + "\n"
                           + QString::fromUtf8(QJsonDocument(steps).toJson(QJsonDocument::Indented)));
        return result({{"ok", true}, {"message", done.message}, {"steps", steps}});
    }

    return failure(QString("Unknown tool %1.").arg(name));
}

QJsonObject reply(const QJsonValue &id, const QJsonObject &resultObject) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", resultObject}};
}

QJsonObject error(const QJsonValue &id, int code, const QString &message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

void write(const QJsonValue &value) {
    const QByteArray line = value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                            : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace

namespace mcp {

QStringList supportedVersions() { return {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"}; }

QJsonArray tools() {
    const QJsonObject version = text("A version line: \"8.4\" for PHP, \"24\" for Node. A full version "
                                     "(\"8.4.27\") means its line.");
    const QJsonObject directory = text("An absolute folder path.");
    return {
        tool("get_status", "Status",
             "What's running (nginx, DNS for .test, the default PHP-FPM), the default PHP and Node, the installed "
             "versions, how many sites there are, the parked folders and the settings. Quick, no network.",
             schema({}), hints("Status", true, false, true)),
        tool("list_sites", "List sites",
             "Every site: URL, folder, document root, kind (laravel, php, static or proxy), where it comes from "
             "(linked, parked or proxy), HTTPS, the PHP and Node it runs and whether they're isolated, and its "
             "nginx config file.",
             schema({}), hints("List sites", true, false, true)),
        tool("which_runtime", "PHP and Node for a folder",
             "The PHP and Node a folder runs and why (site isolation, .nvmrc/.node-version, or the default), with "
             "the binaries' paths. php, composer, node and npm already pick these inside the folder.",
             schema({{"directory", directory}}, {"directory"}), hints("PHP and Node for a folder", true, false, true)),
        tool("list_versions", "List PHP or Node versions",
             "Installed and available versions of PHP or Node: which is the default, which have a newer patch, "
             "the binary and (PHP) php.ini paths. Reads cached release lists unless refresh is true.",
             schema({{"tool", Tool}, {"refresh", flag("Fetch the release lists again first.")}}, {"tool"}),
             hints("List versions", true, false, true, true)),
        tool("check_updates", "Check for updates",
             "Fetches the PHP and Node release lists and returns installed versions with a newer patch release.",
             schema({}), hints("Check for updates", true, false, true, true)),
        tool("doctor", "Health check",
             "Checks every piece: nginx, ports 80/443, .test DNS, the CA trusted by the system and browsers, the "
             "shell PATH, the default PHP and Node, Composer and the bar widget. fixed_by_setup marks what "
             "run_setup fixes.",
             schema({}), hints("Health check", true, false, true)),
        tool("get_nginx_config", "Read a site's nginx config",
             "The nginx server config a site is served with, and whether it's the user's own (custom) or Loft's "
             "generated one.",
             schema({{"name", SiteName}}, {"name"}), hints("Read a site's nginx config", true, false, true)),
        tool("link_site", "Link a folder",
             "Serves a project folder at http://<name>.test (Laravel and other public/ apps from public/). The "
             "name defaults to the folder's, lowercased with dashes.",
             schema({{"directory", directory},
                     {"name", text("Site name: letters, digits and dashes. Defaults to the folder's name.")},
                     {"secure", flag("Serve it over HTTPS right away.")},
                     {"php", text("Isolate it to this PHP line (\"8.4\"), which must be installed.")}},
                    {"directory"}),
             hints("Link a folder", false, false, true)),
        tool("unlink_site", "Unlink a site",
             "Stops serving a linked folder and drops its certificate and custom nginx config. The folder itself "
             "isn't touched. Parked sites go by moving their folder out instead. Ask the user first.",
             schema({{"name", SiteName}}, {"name"}), hints("Unlink a site", false, true, true)),
        tool("secure_site", "HTTPS on or off",
             "secure=true serves the site at https:// with a certificate from Loft's CA (http redirects there); "
             "secure=false goes back to plain HTTP.",
             schema({{"name", SiteName}, {"secure", flag("true for HTTPS, false for plain HTTP.")}},
                    {"name", "secure"}),
             hints("HTTPS on or off", false, false, true)),
        tool("isolate_site", "Pin a site to a version",
             "Makes a site, and php/node commands inside its folder, run a specific PHP or Node version. An empty "
             "version goes back to the default. The version must be installed (install_version).",
             schema({{"name", SiteName}, {"tool", Tool},
                     {"version", text("The line to pin, like \"8.3\" or \"22\"; empty to follow the default.")}},
                    {"name", "tool", "version"}),
             hints("Pin a site to a version", false, false, true)),
        tool("park_folder", "Park a folder",
             "Every folder inside this one becomes a site named after it, including folders created later.",
             schema({{"directory", directory}}, {"directory"}), hints("Park a folder", false, false, true)),
        tool("forget_folder", "Stop parking a folder", "Its subfolders stop being sites; nothing is deleted.",
             schema({{"directory", directory}}, {"directory"}), hints("Stop parking a folder", false, false, true)),
        tool("add_proxy", "Add a proxy",
             "Serves <name>.test from a server the user runs (Vite, a Node app), WebSockets included.",
             schema({{"name", text("The site name: letters, digits and dashes.")},
                     {"url", text("Where to forward, like http://127.0.0.1:5173.")},
                     {"secure", flag("Serve it over HTTPS.")}},
                    {"name", "url"}),
             hints("Add a proxy", false, false, true)),
        tool("remove_proxy", "Remove a proxy", "Stops forwarding <name>.test. The server itself keeps running.",
             schema({{"name", SiteName}}, {"name"}), hints("Remove a proxy", false, true, true)),
        tool("set_default_version", "Set the default PHP or Node",
             "Sets the version the user's terminal and every non-isolated site run. Changes their environment: "
             "ask first.",
             schema({{"tool", Tool}, {"version", version}}, {"tool", "version"}),
             hints("Set the default version", false, false, true)),
        tool("install_version", "Install PHP or Node",
             "Downloads the newest release of a line, checks its published SHA-256 and installs it (PHP gets its "
             "own PHP-FPM). The first PHP or Node installed becomes the default. Takes up to a minute.",
             schema({{"tool", Tool}, {"version", version},
                     {"force", flag("Reinstall even when the newest release is already there.")}},
                    {"tool", "version"}),
             hints("Install PHP or Node", false, false, true, true)),
        tool("update_versions", "Install updates",
             "Installs the newest patch release of every installed version that has one, or only the tool or "
             "line given.",
             schema({{"tool", Tool}, {"version", version}}), hints("Install updates", false, false, true, true)),
        tool("uninstall_version", "Remove a PHP or Node version",
             "Removes a version's binaries (PHP keeps its php.ini). Refused while it's the default with others "
             "installed, or while a site is isolated to it. Ask the user first.",
             schema({{"tool", Tool}, {"version", version}}, {"tool", "version"}),
             hints("Remove a version", false, true, true)),
        tool("restart_php", "Restart PHP-FPM",
             "Restarts one PHP version's FPM, for example after editing its php.ini. Defaults to the default PHP.",
             schema({{"version", version}}), hints("Restart PHP-FPM", false, false, true)),
        tool("control_services", "Start, stop or restart",
             "Starts, stops or restarts nginx, the .test DNS and PHP-FPM together. stop takes every site down: ask "
             "first.",
             schema({{"action", choice({"start", "stop", "restart"}, "What to do.")}}, {"action"}),
             hints("Start, stop or restart", false, false, true)),
        tool("set_nginx_config", "Replace a site's nginx config",
             "Makes the given text the site's nginx config (the user's own, kept in ~/.config/loft/nginx) and "
             "reloads nginx. Rejected configs are rolled back. Read the current one with get_nginx_config first; "
             "while it's custom, Loft's HTTPS and PHP changes for the site don't apply until reset_nginx_config.",
             schema({{"name", SiteName}, {"config", text("The whole nginx config for the site (server blocks).")}},
                    {"name", "config"}),
             hints("Replace a site's nginx config", false, true, false)),
        tool("reset_nginx_config", "Back to Loft's nginx config",
             "Drops the site's custom nginx config so Loft generates it again.",
             schema({{"name", SiteName}}, {"name"}), hints("Back to Loft's nginx config", false, true, true)),
        tool("change_settings", "Change settings",
             "check_updates turns the daily check for new PHP and Node releases (with a desktop notification) on "
             "or off; php_build picks the extension set for new PHP installs: bulk (intl, opcache, sodium, "
             "imagick, redis, swoole...) or common (lean, native pdo_sqlite and pdo_pgsql).",
             schema({{"check_updates", flag("Check for new releases daily.")},
                     {"php_build", choice({"bulk", "common"}, "Extension set for new PHP installs.")}}),
             hints("Change settings", false, false, true)),
        tool("open_site", "Open in the browser", "Opens a site in the user's browser.",
             schema({{"name", SiteName}}, {"name"}), hints("Open in the browser", false, false, true)),
        tool("run_setup", "Run setup",
             "Does whatever one-time setup is missing: the CA, browser and system trust, .test DNS, the shell PATH, "
             "the services, Composer and the bar widget. Root steps show the user a password dialog.",
             schema({}), hints("Run setup", false, false, true, true)),
    };
}

QJsonValue handle(Loft &loft, const QJsonObject &message) {
    const QJsonValue id = message.value("id");
    const bool notification = !message.contains("id");
    const QString method = message.value("method").toString();

    if (message.value("jsonrpc").toString() != "2.0" || method.isEmpty())
        return notification ? QJsonValue() : QJsonValue(error(id, -32600, "Invalid request"));
    if (notification)
        return {}; // notifications/initialized, notifications/cancelled, ...

    const QJsonObject params = message.value("params").toObject();
    if (method == "initialize") {
        const QString asked = params.value("protocolVersion").toString();
        const QString version = supportedVersions().contains(asked) ? asked : supportedVersions().first();
        return reply(id, {{"protocolVersion", version},
                          {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
                          {"serverInfo", QJsonObject{{"name", "loft"}, {"title", "Loft"},
                                                     {"version", QCoreApplication::applicationVersion()}}},
                          {"instructions", Instructions}});
    }
    if (method == "ping")
        return reply(id, {});
    if (method == "tools/list")
        return reply(id, {{"tools", tools()}});
    if (method == "tools/call") {
        const QString name = params.value("name").toString();
        bool known = false;
        for (const auto &definition : tools())
            known = known || definition.toObject().value("name").toString() == name;
        if (!known)
            return error(id, -32602, QString("Unknown tool: %1").arg(name));
        return reply(id, callTool(loft, name, params.value("arguments").toObject()));
    }
    return error(id, -32601, QString("Method not found: %1").arg(method));
}

int serve() {
    Loft loft;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            write(error(QJsonValue::Null, -32700, "Parse error"));
            continue;
        }
        // Older clients may batch several messages in one array.
        if (document.isArray()) {
            QJsonArray replies;
            for (const auto &item : document.array())
                if (const auto answer = handle(loft, item.toObject()); !answer.isUndefined() && !answer.isNull())
                    replies.append(answer);
            if (!replies.isEmpty())
                write(replies);
            continue;
        }
        const auto answer = handle(loft, document.object());
        if (!answer.isUndefined() && !answer.isNull())
            write(answer);
    }
    return 0;
}

} // namespace mcp
