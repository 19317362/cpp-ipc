TEMPLATE = app

QT += core testlib
CONFIG += console c++14
CONFIG -= app_bundle

DESTDIR = ../output

INCLUDEPATH += \
    ../include \
    ../src \
    ../src/platform

HEADERS += \
    ../test/test.h

SOURCES += \
    ../test/main.cpp \
    ../test/test_shm.cpp

LIBS += -L$${DESTDIR} -lipc
