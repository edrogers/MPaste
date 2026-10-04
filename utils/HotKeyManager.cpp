// input: 依赖对应头文件及其所需 Qt/标准库/同层组件实现。
// output: 提供 HotKeyManager 的实现逻辑。
// pos: utils 层中的 HotKeyManager 实现文件。
// update: 修改本文件时，同步更新文件头注释与所属目录 README.md。
// HotKeyManager.cpp
#include "HotKeyManager.h"
#include <QApplication>
#include <QAbstractNativeEventFilter>

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <QGuiApplication>
#include <array>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#endif

class HotkeyManager::Private {
public:
    explicit Private(HotkeyManager *q) : q(q) {
        eventFilter = new EventFilter(this);
        qApp->installNativeEventFilter(eventFilter);

#ifdef Q_OS_WIN
        hotkeyId = 0;
#elif defined(Q_OS_LINUX)
        // Share Qt's own X connection. Grabbed key events are delivered to the
        // connection that issued the grab, and the native event filter below
        // only sees events from Qt's connection, so a private xcb_connect()
        // would grab the key but never report a key press.
        connection = nullptr;
        keySymbols = nullptr;
        keycode = 0;
        modifiers = 0;
        if (auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
            connection = x11->connection();
        }
        if (!connection || xcb_connection_has_error(connection)) {
            qWarning("Failed to get Qt's X server connection");
            connection = nullptr;
            return;
        }

        // Get root window
        const xcb_setup_t *setup = xcb_get_setup(connection);
        xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
        root = iter.data->root;

        // Initialize key symbols
        keySymbols = xcb_key_symbols_alloc(connection);
        keycode = 0;
        modifiers = 0;
#endif
    }

    ~Private() {
        unregisterHotkey();
#ifdef Q_OS_LINUX
        if (keySymbols) {
            xcb_key_symbols_free(keySymbols);
        }
        // connection belongs to Qt; do not disconnect it.
#endif
        qApp->removeNativeEventFilter(eventFilter);
        delete eventFilter;
    }

    bool registerHotkey(const QKeySequence &keySequence) {
#ifdef Q_OS_WIN
        if (keySequence.isEmpty())
            return false;

        // Parse modifiers
        int modifiers = 0;
        Qt::KeyboardModifiers qtMods = Qt::KeyboardModifiers(keySequence[0] & Qt::KeyboardModifierMask);

        if (qtMods & Qt::AltModifier)
            modifiers |= MOD_ALT;
        if (qtMods & Qt::ControlModifier)
            modifiers |= MOD_CONTROL;
        if (qtMods & Qt::ShiftModifier)
            modifiers |= MOD_SHIFT;

        // Get the key
        int key = keySequence[0] & ~Qt::KeyboardModifierMask;

        // Register hotkey
        hotkeyId = 1;  // You might want to generate unique IDs
        return RegisterHotKey(nullptr, hotkeyId, modifiers, key);

#elif defined(Q_OS_LINUX)
        if (keySequence.isEmpty() || !connection || !keySymbols)
            return false;

        // Parse modifiers
        modifiers = 0;
        Qt::KeyboardModifiers qtMods = Qt::KeyboardModifiers(keySequence[0] & Qt::KeyboardModifierMask);

        if (qtMods & Qt::ShiftModifier)
            modifiers |= XCB_MOD_MASK_SHIFT;
        if (qtMods & Qt::ControlModifier)
            modifiers |= XCB_MOD_MASK_CONTROL;
        if (qtMods & Qt::AltModifier)
            modifiers |= XCB_MOD_MASK_1;

        // Get keycode
        int key = keySequence[0] & ~Qt::KeyboardModifierMask;
        xcb_keycode_t *keycodes = xcb_key_symbols_get_keycode(keySymbols, key);
        if (!keycodes) {
            return false;
        }

        keycode = keycodes[0];
        free(keycodes);

        // Register global hotkey. A grab only matches the exact modifier
        // state, so also grab with CapsLock/NumLock set.
        for (uint32_t lock : lockCombinations()) {
            xcb_grab_key(connection, 1, root,
                         modifiers | lock, keycode,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        }

        xcb_flush(connection);
        return true;
#else
        return false;
#endif
    }

    void unregisterHotkey() {
#ifdef Q_OS_WIN
        if (hotkeyId != 0) {
            UnregisterHotKey(nullptr, hotkeyId);
            hotkeyId = 0;
        }
#elif defined(Q_OS_LINUX)
        if (connection && keycode != 0) {
            for (uint32_t lock : lockCombinations()) {
                xcb_ungrab_key(connection, keycode, root, modifiers | lock);
            }
            xcb_flush(connection);
            keycode = 0;
            modifiers = 0;
        }
#endif
    }

#ifdef Q_OS_LINUX
    // CapsLock and NumLock (Mod2) change the modifier state of a key press
    // but should not stop the hotkey from matching.
    static constexpr uint32_t lockMask() {
        return XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2;
    }

    static std::array<uint32_t, 4> lockCombinations() {
        return {0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2, XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2};
    }
#endif

private:
    class EventFilter : public QAbstractNativeEventFilter {
    public:
        explicit EventFilter(Private *d) : d(d) {}

        bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override {
#ifdef Q_OS_WIN
            if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
                MSG *msg = static_cast<MSG *>(message);
                if (msg->message == WM_HOTKEY) {
                    emit d->q->hotkeyPressed();
                    return true;
                }
            }
#elif defined(Q_OS_LINUX)
            if (eventType == "xcb_generic_event_t") {
                xcb_generic_event_t *event = static_cast<xcb_generic_event_t *>(message);
                if ((event->response_type & ~0x80) == XCB_KEY_PRESS) {
                    xcb_key_press_event_t *kp = (xcb_key_press_event_t *)event;
                    if (kp->detail == d->keycode &&
                        (kp->state & ~lockMask()) == d->modifiers) {
                        emit d->q->hotkeyPressed();
                        return true;
                    }
                }
            }
#endif
            return false;
        }

    private:
        Private *d;
    };

    HotkeyManager *q;
    EventFilter *eventFilter;

#ifdef Q_OS_WIN
    int hotkeyId;
#elif defined(Q_OS_LINUX)
    xcb_connection_t *connection;
    xcb_window_t root;
    xcb_key_symbols_t *keySymbols;
    uint32_t keycode;
    uint32_t modifiers;
#endif

    friend class HotkeyManager;
};

HotkeyManager::HotkeyManager(QObject *parent)
    : QObject(parent)
    , d(new Private(this))
{
}

HotkeyManager::~HotkeyManager()
{
    delete d;
}

bool HotkeyManager::registerHotkey(const QKeySequence &keySequence)
{
    return d->registerHotkey(keySequence);
}

void HotkeyManager::unregisterHotkey()
{
    d->unregisterHotkey();
}
