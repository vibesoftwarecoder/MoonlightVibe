#pragma once
// Test stub: the few NvComputer fields NvHTTP reads.
#include "nvaddress.h"
#include "nvhttp.h" // the real nvcomputer.h pulls it in, and nvhttp.cpp relies on that
#include <QSslCertificate>

class NvComputer
{
public:
    NvAddress activeAddress;
    uint16_t activeHttpsPort = 0;
    QSslCertificate serverCert;
    bool isNvidiaServerSoftware = false;
};
