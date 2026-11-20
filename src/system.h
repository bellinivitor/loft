#pragma once

#include "core.h"

#include <QByteArray>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

// Everything Loft touches outside its own files: processes, systemd user
// units, NetworkManager, the shell, and the one-time root setup.
namespace System {

constexpr int DnsPort = 5354;

// Runs a program to completion and returns its exit code (-1 when it can't
// start or times out), with stdout and stderr merged into output.
int run(const QString &program, const QStringList &arguments, QString *output = nullptr,
        int timeoutMs = 60000, bool mergeErrors = true);
QString which(const QString &program);
// The installed /usr/bin/loft when there is one, else this binary, so units
// keep working from a development build.
QString loftExecutable();
QString nginxExecutable();

// Writes only when the content differs, so callers know whether to reload.
bool writeIfChanged(const QString &path, const QByteArray &content, bool executable = false,
                    bool *changed = nullptr);

// systemd --user.
int systemctl(const QStringList &arguments, QString *output = nullptr);
QMap<QString, QString> unitStates(const QStringList &units);
bool unitActive(const QString &unit);
QSet<QString> enabledPhpVersions(const Paths &paths);

// The unit files Loft owns, by file name.
QMap<QString, QString> unitFiles(const Paths &paths, const Config &config, const QString &loft);
// Writes them, removes ones no longer wanted, and reloads systemd on change.
bool installUnits(const Paths &paths, const Config &config, QString *error);
void removeUnits(const Paths &paths);

// The php, composer, node, npm, npx and corepack commands, by file name.
QMap<QString, QString> shimFiles(const Paths &paths, const QStringList &phpMinors);
bool installShims(const Paths &paths, const QStringList &phpMinors);

// NetworkManager: a dummy connection that sends *.<tld> to Loft's DNS and
// nothing else, so the rest of the system's lookups are untouched.
bool dnsConfigured(const QString &tld);
bool configureDns(const QString &tld, QString *error);
void removeDns();

// ~/.bashrc and the desktop session put Loft's commands first on PATH.
QString shellSnippet(const Paths &paths);
bool shellConfigured(const Paths &paths);
bool configureShell(const Paths &paths, QString *error);
void unconfigureShell(const Paths &paths);

int unprivilegedPortStart();
bool ipv6Loopback();

// The parts only root can do: trust the CA system-wide and let users bind
// ports 80 and 443. Asked for once, through pkexec.
bool rootSetupNeeded(const Paths &paths);
bool runRootSetup(const Paths &paths, bool remove, QString *error);
int rootSetup(const QStringList &arguments); // runs as root

void notify(const QString &title, const QString &body);

} // namespace System
