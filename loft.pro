QT += core gui qml quick quickcontrols2 network dbus

CONFIG += c++17 release
TARGET = loft
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/certs.h \
    src/cli.h \
    src/core.h \
    src/dnsserver.h \
    src/loft.h \
    src/mcp.h \
    src/portal.h \
    src/runtimes.h \
    src/system.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/certs.cpp \
    src/cli.cpp \
    src/core.cpp \
    src/dnsserver.cpp \
    src/loft.cpp \
    src/mcp.cpp \
    src/portal.cpp \
    src/runtimes.cpp \
    src/system.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
