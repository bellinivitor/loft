#pragma once

#include <QStringList>

// Every command but the window. Returns the exit code.
int runCli(const QStringList &arguments);

// Whether these arguments ask for the window rather than a command.
bool wantsWindow(int argc, char *argv[]);
