#include "secondarystream.h"

#include "session.h"
#include "streamutils.h"

#include <QtGlobal>

#include <algorithm>

SecondaryStreamWindow::SecondaryStreamWindow(Session* session, const QString& title)
    : m_Session(session),
      m_Title(title),
      m_Window(nullptr),
      m_Decoder(nullptr),
      m_DecoderLock(SDL_CreateMutex()),
      m_VideoFormat(0),
      m_VideoWidth(0),
      m_VideoHeight(0),
      m_FrameRate(0)
{
}

SecondaryStreamWindow::~SecondaryStreamWindow()
{
    // The session closes the window on the main thread before it gets here
    SDL_assert(m_Window == nullptr);
    SDL_DestroyMutex(m_DecoderLock);
}

int SecondaryStreamWindow::drSetup(int videoFormat, int width, int height, int frameRate, void*, int)
{
    SecondaryStreamWindow* self = Session::get()->m_SecondWindow;

    self->m_VideoFormat = videoFormat;
    self->m_VideoWidth = width;
    self->m_VideoHeight = height;
    self->m_FrameRate = frameRate;

    // Like the first stream, the decoder is created later on the main thread, once the window is shown.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Second video stream is %dx%dx%d (format 0x%x)",
                width, height, frameRate, videoFormat);

    return 0;
}

void SecondaryStreamWindow::drCleanup()
{
    // The decoder and the window belong to the main thread and are destroyed there.
}

int SecondaryStreamWindow::drSubmitDecodeUnit(PDECODE_UNIT du)
{
    SecondaryStreamWindow* self = Session::get()->m_SecondWindow;

    // The main thread takes this lock while it replaces or destroys the decoder. If we cannot get it, the decoder
    // is going away; drop the frame, and the new decoder asks for a keyframe when it is ready.
    if (SDL_TryLockMutex(self->m_DecoderLock) == 0) {
        int ret = DR_OK;
        if (self->m_Decoder != nullptr) {
            ret = self->m_Decoder->submitDecodeUnit(du);
        }
        SDL_UnlockMutex(self->m_DecoderLock);
        return ret;
    }

    return DR_OK;
}

bool SecondaryStreamWindow::open()
{
    if (m_Window != nullptr) {
        return true;
    }

    // Start at the stream's shape, at most 960 pixels wide. The user can resize it.
    int width = std::min(m_VideoWidth > 0 ? m_VideoWidth : 960, 960);
    int height = m_VideoWidth > 0 ? width * m_VideoHeight / m_VideoWidth : 540;

    std::string title = (m_Session->m_Computer->name + " - " + m_Title).toStdString();
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;

    m_Window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height,
                                flags | StreamUtils::getPlatformWindowFlags());
    if (m_Window == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateWindow() for the second window failed with platform flags: %s",
                    SDL_GetError());
        m_Window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, flags);
    }
    if (m_Window == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_CreateWindow() for the second window failed: %s",
                     SDL_GetError());
        return false;
    }

    return true;
}

void SecondaryStreamWindow::close()
{
    // The decoder must go before the window it draws into
    SDL_LockMutex(m_DecoderLock);
    delete m_Decoder;
    m_Decoder = nullptr;
    SDL_UnlockMutex(m_DecoderLock);

    if (m_Window != nullptr) {
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
    }
}

bool SecondaryStreamWindow::ownsWindow(Uint32 windowId) const
{
    return m_Window != nullptr && SDL_GetWindowID(m_Window) == windowId;
}

bool SecondaryStreamWindow::recreateDecoder()
{
    StreamingPreferences* prefs = m_Session->m_Preferences;

    // Like the first window: a stream faster than the display cannot be V-synced.
    bool enableVsync = prefs->enableVsync;
    if (StreamUtils::getDisplayRefreshRate(m_Window) + 5 < m_FrameRate) {
        enableVsync = false;
    }

    SDL_LockMutex(m_DecoderLock);
    delete m_Decoder;
    m_Decoder = nullptr;

    bool ok = Session::chooseDecoder(prefs->videoDecoderSelection,
                                     prefs->rendererSelection,
                                     m_Window, m_VideoFormat, m_VideoWidth, m_VideoHeight, m_FrameRate,
                                     enableVsync,
                                     enableVsync && prefs->framePacing,
                                     false,
                                     m_Decoder);
    SDL_UnlockMutex(m_DecoderLock);

    if (!ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to initialize the decoder of the second window");
        return false;
    }

    // Frames that arrived before the decoder existed were dropped, so ask for a keyframe to start from.
    LiRequestIdrFrameForStream(1);
    return true;
}

void SecondaryStreamWindow::handleWindowEvent(const SDL_WindowEvent& event)
{
    if (m_Window == nullptr) {
        return;
    }

    switch (event.event) {
    case SDL_WINDOWEVENT_CLOSE:
        // The user closed the second window. The session carries on with the first.
        close();
        break;

    case SDL_WINDOWEVENT_SHOWN:
        if (m_Decoder == nullptr && !recreateDecoder()) {
            close();
        }
        break;

    case SDL_WINDOWEVENT_SIZE_CHANGED:
        if (m_Decoder != nullptr && !(SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED)) {
            WINDOW_STATE_CHANGE_INFO info = {};
            info.window = m_Window;
            info.stateChangeFlags = WINDOW_STATE_CHANGE_SIZE;
            info.width = event.data1;
            info.height = event.data2;
            if (!m_Decoder->notifyWindowChanged(&info) && !recreateDecoder()) {
                close();
            }
        }
        break;

    case SDL_WINDOWEVENT_DISPLAY_CHANGED:
        // Another display can have another refresh rate, so start the renderer over.
        if (m_Decoder != nullptr && !recreateDecoder()) {
            close();
        }
        break;

    default:
        break;
    }
}

void SecondaryStreamWindow::renderFrameOnMainThread()
{
    if (m_Decoder != nullptr) {
        m_Decoder->renderFrameOnMainThread();
    }
}

bool SecondaryStreamWindow::toVideoPosition(int windowX, int windowY, short& x, short& y, short& referenceWidth, short& referenceHeight)
{
    if (m_Window == nullptr || m_VideoWidth <= 0 || m_VideoHeight <= 0) {
        return false;
    }

    int windowWidth, windowHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);

    SDL_Rect src, dst;
    src.x = src.y = 0;
    src.w = m_VideoWidth;
    src.h = m_VideoHeight;
    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;

    // The picture is letterboxed inside the window, so map the position into the picture and clamp it there.
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    x = (short)qBound(0, windowX - dst.x, dst.w);
    y = (short)qBound(0, windowY - dst.y, dst.h);
    referenceWidth = (short)dst.w;
    referenceHeight = (short)dst.h;
    return referenceWidth > 0 && referenceHeight > 0;
}

void SecondaryStreamWindow::handleMouseMotion(const SDL_MouseMotionEvent* event)
{
    // Ignore the synthetic mouse events that touch input produces
    if (event->which == SDL_TOUCH_MOUSEID) {
        return;
    }

    short x, y, referenceWidth, referenceHeight;
    if (toVideoPosition(event->x, event->y, x, y, referenceWidth, referenceHeight)) {
        // Positions are mapped onto this window's picture, so tell the host which picture that is first.
        LiSetInputTargetStream(1);
        LiSendMousePositionEvent(x, y, referenceWidth, referenceHeight);
    }
}

void SecondaryStreamWindow::handleMouseButton(const SDL_MouseButtonEvent* event)
{
    if (event->which == SDL_TOUCH_MOUSEID) {
        return;
    }

    int button;
    switch (event->button) {
    case SDL_BUTTON_LEFT:
        button = m_Session->m_Preferences->swapMouseButtons ? BUTTON_RIGHT : BUTTON_LEFT;
        break;
    case SDL_BUTTON_MIDDLE:
        button = BUTTON_MIDDLE;
        break;
    case SDL_BUTTON_RIGHT:
        button = m_Session->m_Preferences->swapMouseButtons ? BUTTON_LEFT : BUTTON_RIGHT;
        break;
    case SDL_BUTTON_X1:
        button = BUTTON_X1;
        break;
    case SDL_BUTTON_X2:
        button = BUTTON_X2;
        break;
    default:
        return;
    }

    // Put the pointer where the click happened first, so a click does not depend on a motion event before it.
    short x, y, referenceWidth, referenceHeight;
    if (toVideoPosition(event->x, event->y, x, y, referenceWidth, referenceHeight)) {
        LiSetInputTargetStream(1);
        LiSendMousePositionEvent(x, y, referenceWidth, referenceHeight);
    }

    LiSendMouseButtonEvent(event->type == SDL_MOUSEBUTTONDOWN ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, button);
}
