#include "safety_keys.hpp"
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>

namespace drivelink::gui {
SafetyKeys::SafetyKeys(QObject *parent):QObject(parent) {qApp->installEventFilter(this);}
void SafetyKeys::remember(QObject *object) {
    // QQuickTextInput/TextEdit (including Controls TextField) expose these
    // properties. Other focusable controls must not acquire an edit snapshot.
    if(object && object->property("text").isValid() && object->property("cursorPosition").isValid() &&
       !object->property("readOnly").toBool()) {
        m_editor=object;m_before=object->property("text");
    } else {m_editor.clear();m_before.clear();}
}
void SafetyKeys::cancelEdit() {
    // Only the editor that still has focus: a field that already committed on
    // focus loss keeps its value.
    if(m_editor && m_editor->property("activeFocus").toBool()) {m_editor->setProperty("text",m_before);}
}
void SafetyKeys::editCommitted() {
    if(m_editor && m_editor->property("activeFocus").toBool()) {m_before=m_editor->property("text");}
}
bool SafetyKeys::eventFilter(QObject *object,QEvent *event) {
    if(event->type()==QEvent::FocusIn) {remember(object);}
    if(event->type()==QEvent::KeyRelease && static_cast<QKeyEvent *>(event)->key()==Qt::Key_Escape) {m_escapeDown=false;}
    if(event->type()!=QEvent::ShortcutOverride && event->type()!=QEvent::KeyPress) {return false;}
    auto *key=static_cast<QKeyEvent *>(event);
    if(key->key()==Qt::Key_Escape) {
        key->accept();
        if(event->type()==QEvent::ShortcutOverride) {
            // A release can be lost when focus leaves the app. Each new
            // physical press still gets its own shortcut-override event.
            if(!key->isAutoRepeat()) {m_escapeDown=false;}
            return true;
        }
        if(!key->isAutoRepeat() && !m_escapeDown) {
            m_escapeDown=true;
            // Restore before anything closes a popup or changes focus. Keep
            // the native Esc event for editor/popup cancellation as well.
            cancelEdit();
            emit escapeRequested();
        }
        return false;
    }
    if(key->modifiers()==Qt::ControlModifier && key->key()>=Qt::Key_1 && key->key()<=Qt::Key_4) {
        key->accept();
        if(event->type()==QEvent::KeyPress && !key->isAutoRepeat()) {emit pageRequested(key->key()-Qt::Key_1);}
        return true;
    }
    return false;
}
}
