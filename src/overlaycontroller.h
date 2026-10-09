#pragma once

#include "annotation.h"
#include "capture.h"
#include "document.h"
#include "geometry.h"
#include "settings.h"

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <optional>

class QKeyEvent;
class QPainter;
class ScreenView;
class Toolbar;

// Вся логика оверлея: выделение области, рисование и вывод результата; координаты — координаты изображения.
// Не QWidget: окна мониторов (ScreenView) переводят свои события в координаты изображения и отдают их сюда,
// а рисуют свою часть кадра через paint(). Контроллер должен жить дольше окон (сессию удаляют первой).
class OverlayController : public QObject {
    Q_OBJECT
public:
    OverlayController(Capture capture, Settings settings, QString saveDir, QObject* parent = nullptr);
    ~OverlayController() override; // удаляет панель инструментов

    const Capture& capture() const { return m_capture; }
    QRect selection() const { return m_selection; }
    Tool tool() const { return m_tool; }
    Style style() const { return m_style; }
    const Document& document() const { return m_document; }
    Toolbar* toolbar() const;
    bool isEditingText() const { return m_textEditing; }
    QImage result() const; // выделенная область со всеми аннотациями

    void attachView(ScreenView* view); // окна для перерисовки, курсора и размещения панели

    // События от окон, уже переведённые в координаты изображения:
    void mousePress(QPoint pos, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    void mouseMove(QPoint pos, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods);
    void mouseRelease(QPoint pos, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    void mouseDoubleClick(QPoint pos);
    void wheel(int angleDeltaY);
    void keyPress(QKeyEvent* event);
    // Рисует кадр в координатах изображения; imageRect — видимая окну часть.
    void paint(QPainter& painter, const QRect& imageRect) const;
    // Отмена: скрыть окна и завершить (Esc, кнопка панели, закрытие окна композитором).
    void cancel();

signals:
    void copyRequested(const QImage& result); // испускается ДО hideRequested
    void finished();
    void hideRequested(); // скрыть все окна (перед диалогами, после копирования/сохранения/отмены)
    void showRequested(); // снова показать окна (отмена диалога, ошибка записи)

private:
    enum class Drag { None, Selecting, Moving, Resizing, Drawing };

    void setSelection(const QRect& selection);
    void setTool(Tool tool);
    void applyDefaultTool(); // первое непустое выделение: включить карандаш, если инструмент не выбирали
    void setColor(const QColor& color);
    void setThickness(int thickness);
    void undo();
    void redo();
    void copyResult();
    void saveQuick();
    void saveAs();
    void showError(const QString& text);
    void documentChanged();
    void updateToolbar();
    ScreenView* viewForToolbar(QPoint topLeft) const;
    void updateCursor(QPoint pos);
    void setViewsCursor(Qt::CursorShape shape);
    void updateViews(); // перерисовать все окна
    QRect screenAt(QPoint pos) const;
    QRect bounds() const;
    void beginAnnotation(QPoint pos);
    void finishAnnotation();
    void commitText();                    // завершить ввод текста; пустой текст отбрасывается
    bool handleTextKey(QKeyEvent* event); // true — клавиша поглощена вводом текста
    void paintCurrent(QPainter& painter) const;

    void paintSelectionFrame(QPainter& painter) const;
    void paintSizeLabel(QPainter& painter) const;
    void paintHint(QPainter& painter) const;

    Capture m_capture;
    QImage m_dimmed; // снимок с затемнением — фон вне выделения
    QString m_saveDir;
    Style m_style;
    Tool m_tool = Tool::None;
    bool m_defaultToolApplied = false; // умолчательный инструмент уже применён или пользователь выбрал свой
    Document m_document;
    QRect m_selection;
    QRect m_previousSelection; // выделение на момент прошлого updateViews: при выделении перерисовывается только разница
    QRect m_hintScreen; // монитор под курсором, пока выделения нет
    // Панель — дочерний виджет одного из окон, но принадлежит контроллеру (сессия отцепляет её до удаления окон).
    QPointer<Toolbar> m_toolbar;
    QVector<QPointer<ScreenView>> m_views;

    Drag m_drag = Drag::None;
    Handle m_handle = Handle::None;
    QPoint m_pressPos;
    QRect m_selectionAtPress;
    std::optional<Annotation> m_current; // рисуемая аннотация или вводимый текст
    bool m_textEditing = false;
    int m_wheelAccumulator = 0; // накопленные единицы колеса (120 = один шаг)

    mutable QImage m_cache; // render() для текущего выделения и документа
    mutable bool m_cacheValid = false;
};
