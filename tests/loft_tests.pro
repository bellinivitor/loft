QT += core network testlib
QT -= gui
CONFIG += c++17 testcase
TARGET = loft_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    ../src/certs.h \
    ../src/core.h \
    ../src/dnsserver.h \
    ../src/loft.h \
    ../src/mcp.h \
    ../src/runtimes.h \
    ../src/system.h

SOURCES += \
    loft_tests.cpp \
    ../src/certs.cpp \
    ../src/core.cpp \
    ../src/dnsserver.cpp \
    ../src/loft.cpp \
    ../src/mcp.cpp \
    ../src/runtimes.cpp \
    ../src/system.cpp
