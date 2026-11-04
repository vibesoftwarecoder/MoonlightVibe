#pragma once

#include <QString>

#include <Limelight.h>
#include "SDL_compat.h"
#include "video/decoder.h"

class Session;

/**
 * The second video stream of a session: a host window shown in a client window of its own.
 *
 * It owns the SDL window and the decoder for that window. The session creates one when the user asked for a
 * second window, lets the connection deliver frames to it through the static callbacks below, and hands it the
 * window and pointer events that belong to its window. Everything except the callbacks runs on the session's
 * main thread, like the first window.
 *
 * Absolute pointer input only: the first window can capture the mouse, this one cannot.
 */
class SecondaryStreamWindow
{
public:
    SecondaryStreamWindow(Session* session, const QString& title);
    ~SecondaryStreamWindow();

    // Decoder and renderer callbacks for the connection, for the second stream. The decoder pulls its frames itself,
    // the way the first window's does, so there is no submit callback.
    static int drSetup(int videoFormat, int width, int height, int frameRate, void* context, int drFlags);
    static void drCleanup();

    // Main thread: show the window. The decoder is created when the window reports that it was shown.
    bool open();

    // Main thread: destroy the decoder and the window. Safe to call twice.
    void close();

    bool isOpen() const { return m_Window != nullptr; }
    bool ownsWindow(Uint32 windowId) const;

    void handleWindowEvent(const SDL_WindowEvent& event);
    void renderFrameOnMainThread();
    void handleMouseMotion(const SDL_MouseMotionEvent* event);
    void handleMouseButton(const SDL_MouseButtonEvent* event);

private:
    // Close the window and tell the host to stop sending the stream it showed
    void stopAndClose();
    bool recreateDecoder();
    bool toVideoPosition(int windowX, int windowY, short& x, short& y, short& referenceWidth, short& referenceHeight);

    Session* m_Session;
    QString m_Title;

    SDL_Window* m_Window;
    IVideoDecoder* m_Decoder;
    SDL_mutex* m_DecoderLock;

    // Set by drSetup() on the connection thread before the window exists
    int m_VideoFormat;
    int m_VideoWidth;
    int m_VideoHeight;
    int m_FrameRate;
};
