QT += core network testlib
QT -= gui
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = usb_forwarding_session_glue_test
SOURCES += main.cpp \
    ../../app/backend/usbforwardingsessionglue.cpp \
    ../../app/backend/usbforwardingmanager.cpp \
    ../../app/backend/usbforwardingtunnel.cpp
HEADERS += \
    ../../app/backend/usbforwardingsessionglue.h \
    ../../app/backend/usbforwardingmanager.h \
    ../../app/backend/usbforwardingtunnel.h \
    ../../app/backend/usbforwardingcapability.h \
    ../../app/backend/pairedcertificate.h \
    ../usb_forwarding_common/faketlsserver.h
