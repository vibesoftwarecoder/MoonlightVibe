#pragma once

// The logger is initialized in main.cpp before the QML UI is created.
bool diagnosticLoggingEnabled();
void setDiagnosticLoggingEnabled(bool enabled);
