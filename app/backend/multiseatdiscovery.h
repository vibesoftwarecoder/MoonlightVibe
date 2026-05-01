#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QTimer>

// Polls the local MultiSeat service API (http://localhost:9550/api/seats)
// and emits seatFound() for each active seat so ComputerManager can add it.
class MultiSeatDiscovery : public QObject
{
    Q_OBJECT

public:
    static constexpr int POLL_INTERVAL_MS = 15000;
    static constexpr int MULTISEAT_API_PORT = 9550;

    explicit MultiSeatDiscovery(QObject* parent = nullptr);
    ~MultiSeatDiscovery();

    void start();
    void stop();

signals:
    // Emitted for each Ready/Streaming seat found in the MultiSeat API.
    // port is the Apollo HTTP discovery port (seat.portBase).
    void seatFound(QString host, uint16_t port, QString displayName);

private slots:
    void poll();
    void handleReply(QNetworkReply* reply);

private:
    QNetworkAccessManager* m_Nam;
    QTimer* m_Timer;
    bool m_RequestPending;
};
