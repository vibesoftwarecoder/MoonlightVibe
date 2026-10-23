QT += core testlib
QT -= gui
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = usb_forwarding_backend_list_test
SOURCES += main.cpp \
    ../../app/backend/usbforwardingbackend.cpp \
    ../../app/backend/usbforwardingenvironment.cpp \
    ../../app/backend/usbforwardingelevation.cpp
HEADERS += \
    ../../app/backend/usbforwardingbackend.h \
    ../../app/backend/usbforwardingcapability.h \
    ../../app/backend/usbforwardingenvironment.h \
    ../../app/backend/usbforwardingelevation.h
win32:LIBS += -ladvapi32 -lshell32 -lole32
