TEMPLATE = app

QT += core testlib
QT -= gui
CONFIG += console c++1z
CONFIG -= app_bundle

DESTDIR = ../output

msvc:QMAKE_CXXFLAGS += /std:c++17
else:QMAKE_CXXFLAGS += -std=gnu++1z -Wno-unused-function -Wno-attributes

INCLUDEPATH += \
    ../test \
    ../test/capo \
    ../include \
    ../src \
    ../src/platform

HEADERS += \
    ../test/test.h

SOURCES += \
    ../test/main.cpp \
    ../test/test_shm.cpp \
    ../test/test_circ.cpp \
    ../test/test_ipc.cpp

LIBS += \
    -L$${DESTDIR} -lipc
