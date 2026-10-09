#include "screenview.h"

#include "overlaycontroller.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QWheelEvent>

ScreenView::ScreenView(OverlayController* controller, QRect imageRect, QWidget* parent)
    : QWidget(parent)
    , m_controller(controller)
    , m_imageRect(imageRect)
{
    resize(m_imageRect.size());
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
}

QPoint ScreenView::toImage(const QPointF& local) const
{
    return local.toPoint() + m_imageRect.topLeft();
}

void ScreenView::paintEvent(QPaintEvent* event)
{
    if (!m_controller)
        return;
    QPainter painter(this);
    painter.translate(-m_imageRect.topLeft());
    for (const QRect& rect : event->region()) {
        painter.save();
        const QRect imageArea = rect.translated(m_imageRect.topLeft());
        painter.setClipRect(imageArea);
        m_controller->paint(painter, imageArea);
        painter.restore();
    }
}

// Окно, получившее нажатие, получает и все движения до отпускания (неявный захват указателя), даже за своими
// пределами: тот же сдвиг переводит их в координаты изображения, и выделение тянется на соседний монитор.
void ScreenView::mousePressEvent(QMouseEvent* event)
{
    if (m_controller)
        m_controller->mousePress(toImage(event->position()), event->button(), event->modifiers());
}

void ScreenView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_controller)
        m_controller->mouseMove(toImage(event->position()), event->buttons(), event->modifiers());
}

void ScreenView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_controller)
        m_controller->mouseRelease(toImage(event->position()), event->button(), event->modifiers());
}

// Без переопределения Qt вызвал бы mousePressEvent: двойной клик сработал бы как второе нажатие.
void ScreenView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (m_controller)
        m_controller->mouseDoubleClick(toImage(event->position()));
    event->accept();
}

// Сюда же приходит колесо, пересланное панелью инструментов (она — дочерний виджет окна).
void ScreenView::wheelEvent(QWheelEvent* event)
{
    if (m_controller)
        m_controller->wheel(event->angleDelta().y());
    event->accept();
}

void ScreenView::keyPressEvent(QKeyEvent* event)
{
    if (m_controller)
        m_controller->keyPress(event);
}

// Окно закрыл композитор или оконный менеджер (Alt+F4, Super+Q): это отмена всего оверлея. Иначе спряталось бы
// одно окно, а процесс без окон держал бы блокировку экземпляра. Свои окна сессия прячет через hide() —
// он closeEvent не присылает.
void ScreenView::closeEvent(QCloseEvent* event)
{
    if (!m_controller)
        return;
    event->ignore(); // окна прячет сессия по hideRequested
    m_controller->cancel();
}
