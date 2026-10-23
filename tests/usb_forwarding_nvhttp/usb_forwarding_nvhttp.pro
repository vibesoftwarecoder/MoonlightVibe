# Compiles the real app/backend/nvhttp.cpp. It includes sibling headers
# (identitymanager.h, nvcomputer.h, Limelight.h) that drag in the whole
# application, so the build copies nvhttp and its small neighbours next to
# lightweight stubs in a scratch directory and compiles that copy.
QT += core network gui testlib
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = usb_forwarding_nvhttp_test
SHADOW = $$OUT_PWD/shadow
mkpath($$SHADOW)
BACKEND = $$PWD/../../app/backend
BACKEND_FILES = nvhttp.h nvhttp.cpp nvaddress.h nvaddress.cpp nvapp.h pairedcertificate.h usbforwardingcapability.h
STUB_FILES = Limelight.h identitymanager.h nvcomputer.h
for(f, BACKEND_FILES) {
    system($$QMAKE_COPY_FILE $$shell_quote($$shell_path($$BACKEND/$$f)) $$shell_quote($$shell_path($$SHADOW/$$f)))
}
for(f, STUB_FILES) {
    system($$QMAKE_COPY_FILE $$shell_quote($$shell_path($$PWD/stubs/$$f)) $$shell_quote($$shell_path($$SHADOW/$$f)))
}


INCLUDEPATH += $$SHADOW
SOURCES += main.cpp $$SHADOW/nvhttp.cpp $$SHADOW/nvaddress.cpp
HEADERS += $$SHADOW/nvhttp.h ../usb_forwarding_common/faketlsserver.h
