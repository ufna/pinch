#include "overlaycontroller.h"

#include "keys.h"
#include "output.h"
#include "renderer.h"
#include "screenview.h"
#include "toolbar.h"

#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPainter>
#include <QRegion>
#include <QTextStream>

#include <utility>

namespace {
constexpr int kHandleTolerance = 6;
constexpr int kHandleSize = 8;
constexpr int kClickThreshold = 3;
constexpr int kMinThickness = 1;
constexpr int kMaxThickness = 40;

QColor accentColor()
{
    return QColor(0x3D, 0x8B, 0xFD);
}

QString sizeLabelText(const QRect& selection)
{
    return QStringLiteral("%1×%2").arg(selection.width()).arg(selection.height());
}

// Эту плашку рисует paintSizeLabel и перерисовывает updateViews — расходиться они не должны.
QRect sizeLabelRect(const QRect& selection, const QString& text, const QFont& font, const QVector<QRect>& screens)
{
    const QFontMetrics metrics(font);
    const QSize size(metrics.horizontalAdvance(text) + 12, metrics.height() + 6);
    // Над выделением, если там есть видимая часть монитора; иначе внутри, на видимом мониторе.
    return QRect(sizeLabelPosition(selection, size, screens), size);
}
}

OverlayController::OverlayController(Capture capture, Settings settings, QString saveDir, QObject* parent)
    : QObject(parent)
    , m_capture(std::move(capture))
    , m_saveDir(std::move(saveDir))
    , m_style{settings.color, settings.thickness}
{
    m_dimmed = m_capture.image.copy();
    {
        QPainter painter(&m_dimmed);
        painter.fillRect(m_dimmed.rect(), QColor(0, 0, 0, 120));
    }
    m_hintScreen = screenAt(QCursor::pos() - m_capture.origin);

    // Без родителя, пока не прикреплено окно: updateToolbar подвешивает панель к окну, где она лежит.
    auto* toolbar = new Toolbar;
    m_toolbar = toolbar;
    toolbar->hide();
    toolbar->setTool(m_tool);
    toolbar->setColor(m_style.color);
    toolbar->setThickness(m_style.thickness);
    connect(toolbar, &Toolbar::toolChosen, this, &OverlayController::setTool);
    connect(toolbar, &Toolbar::colorChosen, this, &OverlayController::setColor);
    connect(toolbar, &Toolbar::thicknessChosen, this, &OverlayController::setThickness);
    connect(toolbar, &Toolbar::undoRequested, this, &OverlayController::undo);
    connect(toolbar, &Toolbar::redoRequested, this, &OverlayController::redo);
    connect(toolbar, &Toolbar::copyRequested, this, &OverlayController::copyResult);
    connect(toolbar, &Toolbar::quickSaveRequested, this, &OverlayController::saveQuick);
    connect(toolbar, &Toolbar::saveAsRequested, this, &OverlayController::saveAs);
    connect(toolbar, &Toolbar::closeRequested, this, &OverlayController::cancel);
}

OverlayController::~OverlayController()
{
    delete m_toolbar.data();
}

Toolbar* OverlayController::toolbar() const
{
    return m_toolbar.data();
}

QImage OverlayController::result() const
{
    return ::render(m_capture.image, m_selection, m_document.annotations());
}

void OverlayController::attachView(ScreenView* view)
{
    m_views.append(view);
}

// ---- Состояние ----

void OverlayController::setSelection(const QRect& selection)
{
    m_selection = selection;
    m_cacheValid = false;
    updateToolbar();
    updateViews();
}

void OverlayController::setTool(Tool tool)
{
    commitText();
    m_defaultToolApplied = true; // любой выбор (клавиша, панель, умолчание) отменяет дальнейший умолчательный выбор
    m_tool = tool;
    m_toolbar->setTool(tool);
    updateCursor(QCursor::pos() - m_capture.origin);
}

void OverlayController::applyDefaultTool()
{
    if (m_defaultToolApplied || m_selection.isEmpty())
        return;
    setTool(Tool::Pen);
}

void OverlayController::setColor(const QColor& color)
{
    m_style.color = color;
    m_toolbar->setColor(color);
    if (m_textEditing)
        m_current->style.color = color;
    updateViews();
}

void OverlayController::setThickness(int thickness)
{
    m_style.thickness = qBound(kMinThickness, thickness, kMaxThickness);
    m_toolbar->setThickness(m_style.thickness);
    if (m_textEditing)
        m_current->style.thickness = m_style.thickness;
    updateViews();
}

void OverlayController::undo()
{
    commitText();
    if (m_document.undo())
        documentChanged();
}

void OverlayController::redo()
{
    commitText();
    if (m_document.redo())
        documentChanged();
}

void OverlayController::documentChanged()
{
    m_cacheValid = false;
    m_toolbar->setUndoRedoEnabled(m_document.canUndo(), m_document.canRedo());
    updateViews();
}

void OverlayController::updateToolbar()
{
    if (m_selection.isEmpty() || m_drag == Drag::Selecting) {
        m_toolbar->hide();
        return;
    }
    const QSize size = m_toolbar->sizeHint();
    m_toolbar->resize(size);
    const QPoint pos = placeToolbar(m_selection, size, m_capture.screens);
    ScreenView* view = viewForToolbar(pos);
    if (!view) {
        m_toolbar->hide(); // окон нет — показывать панель негде
        return;
    }
    // Панель — дочерний виджет окна, на чьём мониторе она лежит; выделение ушло на другой монитор — переподвешиваем.
    if (m_toolbar->parentWidget() != view)
        m_toolbar->setParent(view);
    m_toolbar->move(pos - view->imageRect().topLeft());
    m_toolbar->show();
    m_toolbar->raise();
}

ScreenView* OverlayController::viewForToolbar(QPoint topLeft) const
{
    // Окно, содержащее левый верхний угол панели; иначе — с наибольшим пересечением с выделением.
    ScreenView* best = nullptr;
    qint64 bestArea = -1;
    for (const QPointer<ScreenView>& view : m_views) {
        if (!view)
            continue;
        if (view->imageRect().contains(topLeft))
            return view;
        const QRect i = view->imageRect().intersected(m_selection);
        const qint64 area = i.isEmpty() ? 0 : qint64(i.width()) * i.height();
        if (area > bestArea) {
            bestArea = area;
            best = view;
        }
    }
    return best;
}

void OverlayController::updateCursor(QPoint pos)
{
    if (m_selection.isEmpty()) {
        setViewsCursor(Qt::CrossCursor);
        return;
    }
    switch (hitTestHandle(m_selection, pos, kHandleTolerance)) {
    case Handle::TopLeft:
    case Handle::BottomRight:
        setViewsCursor(Qt::SizeFDiagCursor);
        return;
    case Handle::TopRight:
    case Handle::BottomLeft:
        setViewsCursor(Qt::SizeBDiagCursor);
        return;
    case Handle::Top:
    case Handle::Bottom:
        setViewsCursor(Qt::SizeVerCursor);
        return;
    case Handle::Left:
    case Handle::Right:
        setViewsCursor(Qt::SizeHorCursor);
        return;
    case Handle::Move:
        if (m_tool == Tool::None)
            setViewsCursor(Qt::SizeAllCursor);
        else if (m_tool == Tool::Text)
            setViewsCursor(Qt::IBeamCursor);
        else
            setViewsCursor(Qt::CrossCursor);
        return;
    case Handle::None:
        setViewsCursor(m_tool == Tool::None ? Qt::CrossCursor : Qt::ArrowCursor);
        return;
    }
}

void OverlayController::setViewsCursor(Qt::CursorShape shape)
{
    for (const QPointer<ScreenView>& view : std::as_const(m_views)) {
        if (view && view->cursor().shape() != shape)
            view->setCursor(shape);
    }
}

void OverlayController::updateViews()
{
    QRegion dirty(bounds());
    if (m_drag == Drag::Selecting && !m_selection.isEmpty() && !m_previousSelection.isEmpty()) {
        // При выделении общая часть старого и нового прямоугольника не меняется: перерисовываются только
        // изменившиеся полосы, старые и новые рамки с маркерами и подписи размера.
        dirty = QRegion(m_selection).xored(QRegion(m_previousSelection));
        for (const QRect& selection : {m_previousSelection, m_selection}) {
            dirty += QRegion(selection.adjusted(-kHandleSize, -kHandleSize, kHandleSize, kHandleSize))
                .subtracted(QRegion(selection.adjusted(kHandleSize, kHandleSize, -kHandleSize, -kHandleSize)));
            const QString text = sizeLabelText(selection);
            for (const QPointer<ScreenView>& view : std::as_const(m_views)) {
                if (!view)
                    continue;
                dirty += sizeLabelRect(selection, text, view->font(), m_capture.screens).adjusted(-2, -2, 2, 2);
            }
        }
    }
    for (const QPointer<ScreenView>& view : std::as_const(m_views)) {
        if (!view)
            continue;
        if (m_selection.isEmpty() && m_previousSelection.isEmpty()) {
            view->update();
        } else {
            const QRegion local = dirty.intersected(view->imageRect()).translated(-view->imageRect().topLeft());
            if (!local.isEmpty())
                view->update(local);
        }
    }
    m_previousSelection = m_selection;
}

QRect OverlayController::screenAt(QPoint pos) const
{
    for (const QRect& s : m_capture.screens) {
        if (s.contains(pos))
            return s;
    }
    return {};
}

QRect OverlayController::bounds() const
{
    return m_capture.image.rect();
}

// ---- Рисование ----

void OverlayController::beginAnnotation(QPoint pos)
{
    Annotation a;
    a.tool = m_tool;
    a.style = m_style;
    a.points = {pos};
    switch (m_tool) {
    case Tool::Counter:
        m_document.add(a);
        documentChanged();
        return;
    case Tool::Text:
        m_current = a;
        m_textEditing = true;
        updateViews();
        return;
    case Tool::Line:
    case Tool::Arrow:
    case Tool::Rect:
    case Tool::Ellipse:
    case Tool::Pixelate:
        a.points.append(pos); // [начало, конец]
        break;
    case Tool::Pen:
    case Tool::Marker:
    case Tool::None:
        break;
    }
    m_current = a;
    m_drag = Drag::Drawing;
    updateViews();
}

void OverlayController::finishAnnotation()
{
    if (!m_current)
        return;
    const Annotation a = *m_current;
    m_current.reset();

    bool valid = false;
    switch (a.tool) {
    case Tool::Pen:
        valid = !a.points.isEmpty();
        break;
    case Tool::Marker:
        valid = a.points.size() >= 2;
        break;
    case Tool::Line:
    case Tool::Arrow:
        valid = a.points.at(0) != a.points.at(1);
        break;
    case Tool::Rect:
    case Tool::Ellipse:
    case Tool::Pixelate: {
        const QRect r = rectFromPoints(a.points.at(0), a.points.at(1));
        valid = r.width() >= 2 && r.height() >= 2;
        break;
    }
    case Tool::None:
    case Tool::Text:
    case Tool::Counter:
        break;
    }
    if (valid) {
        m_document.add(a);
        documentChanged();
    } else {
        updateViews();
    }
}

void OverlayController::commitText()
{
    if (!m_textEditing)
        return;
    m_textEditing = false;
    const Annotation a = *m_current;
    m_current.reset();
    if (!a.text.isEmpty()) {
        m_document.add(a);
        documentChanged();
    } else {
        updateViews();
    }
}

bool OverlayController::handleTextKey(QKeyEvent* event)
{
    if (!m_textEditing)
        return false;
    if (event->key() == Qt::Key_Escape) {
        commitText();
        return true;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        commitText();
        return false; // сочетание обработает keyPress
    }
    switch (event->key()) {
    case Qt::Key_Backspace:
        m_current->text.chop(1);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        m_current->text += QLatin1Char('\n');
        break;
    default: {
        const QString text = event->text();
        if (!text.isEmpty() && text.at(0).isPrint())
            m_current->text += text;
        break;
    }
    }
    updateViews();
    return true;
}

void OverlayController::paintCurrent(QPainter& painter) const
{
    if (!m_current)
        return;
    painter.save();
    painter.setClipRect(m_selection);
    drawAnnotation(painter, *m_current, m_document.nextCounterNumber());
    if (m_textEditing) {
        // Курсор в конце последней строки, по тем же метрикам, что и drawAnnotation.
        const QFontMetrics metrics(textFont(m_current->style.thickness));
        const QStringList lines = m_current->text.split(QLatin1Char('\n'));
        const QPoint origin = m_current->points.constFirst();
        const int x = origin.x() + metrics.horizontalAdvance(lines.constLast());
        const int y = origin.y() + int(lines.size() - 1) * metrics.lineSpacing();
        painter.setPen(QPen(m_current->style.color, 2));
        painter.drawLine(x, y, x, y + metrics.height());
    }
    painter.restore();
}

// ---- Вывод ----

void OverlayController::copyResult()
{
    commitText();
    if (m_selection.isEmpty())
        return;
    const QImage image = result();
    // Сначала буфер, потом скрытие: под Wayland буфер назначается, только пока окно видимо и в фокусе.
    emit copyRequested(image);
    emit hideRequested();
}

void OverlayController::saveQuick()
{
    commitText();
    if (m_selection.isEmpty())
        return;
    QString path;
    const WriteResult r = quickSave(result(), m_saveDir, QDateTime::currentDateTime(), &path);
    if (r.status != WriteResult::Ok) {
        showError(r.error);
        return;
    }
    QTextStream(stdout) << path << Qt::endl;
    emit hideRequested();
    emit finished();
}

void OverlayController::saveAs()
{
    commitText();
    if (m_selection.isEmpty())
        return;
    const QImage image = result();
    emit hideRequested(); // окна поверх всех: иначе диалог окажется под ними

    // Собственный диалог Qt: суффикс .png добавляется до вопроса о перезаписи.
    QFileDialog dialog(nullptr, qtTrId("dialog.save.title"));
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(qtTrId("dialog.save.filter"));
    dialog.setDefaultSuffix(QStringLiteral("png"));
    dialog.setDirectory(m_saveDir);
    dialog.selectFile(quickSaveFileName(QDateTime::currentDateTime(), 0));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        emit showRequested();
        return;
    }

    const QString path = dialog.selectedFiles().constFirst();
    const QByteArray png = encodePng(image);
    const WriteResult r = png.isEmpty()
        ? WriteResult{WriteResult::Error, qtTrId("error.save.encode")}
        : writeFileAtomic(path, png, WriteMode::Replace);
    if (r.status != WriteResult::Ok) {
        showError(r.error);
        return;
    }
    QTextStream(stdout) << path << Qt::endl;
    emit finished();
}

void OverlayController::cancel()
{
    emit hideRequested();
    emit finished();
}

void OverlayController::showError(const QString& text)
{
    emit hideRequested();
    QMessageBox::critical(nullptr, qtTrId("dialog.error.title"), text);
    emit showRequested();
}

// ---- Мышь и клавиатура ----

void OverlayController::mousePress(QPoint pos, Qt::MouseButton button, Qt::KeyboardModifiers /*mods*/)
{
    if (button != Qt::LeftButton)
        return;
    commitText(); // клик в любом месте завершает вводимый текст
    m_pressPos = pos;
    m_selectionAtPress = m_selection;

    if (m_selection.isEmpty()) {
        m_drag = Drag::Selecting;
        return;
    }
    const Handle handle = hitTestHandle(m_selection, pos, kHandleTolerance);
    if (handle == Handle::None) {
        // Вне выделения: без инструмента — новое выделение (аннотации остаются), с инструментом — ничего.
        if (m_tool == Tool::None) {
            m_drag = Drag::Selecting;
            setSelection(QRect());
        }
        return;
    }
    if (handle != Handle::Move) {
        m_drag = Drag::Resizing;
        m_handle = handle;
        return;
    }
    if (m_tool == Tool::None) {
        m_drag = Drag::Moving;
        m_handle = Handle::Move;
        return;
    }
    beginAnnotation(pos);
}

void OverlayController::mouseMove(QPoint pos, Qt::MouseButtons /*buttons*/, Qt::KeyboardModifiers mods)
{
    switch (m_drag) {
    case Drag::None:
        updateCursor(pos);
        if (m_selection.isEmpty()) {
            const QRect screen = screenAt(pos);
            if (screen != m_hintScreen) {
                m_hintScreen = screen;
                updateViews();
            }
        }
        return;
    case Drag::Selecting:
        setSelection(rectFromPoints(m_pressPos, pos).intersected(bounds()));
        return;
    case Drag::Moving:
    case Drag::Resizing:
        setSelection(applyHandleDrag(m_selectionAtPress, m_handle, pos - m_pressPos, bounds()));
        return;
    case Drag::Drawing: {
        if (!m_current)
            return;
        Annotation& a = *m_current;
        if (a.tool == Tool::Pen || a.tool == Tool::Marker) {
            if (pos != a.points.constLast())
                a.points.append(pos);
        } else {
            QPoint end = pos;
            if (mods.testFlag(Qt::ShiftModifier)) {
                const bool line = a.tool == Tool::Line || a.tool == Tool::Arrow;
                end = line ? snapLine45(a.points.at(0), pos) : snapSquare(a.points.at(0), pos);
            }
            a.points[1] = end;
        }
        updateViews();
        return;
    }
    }
}

void OverlayController::mouseRelease(QPoint pos, Qt::MouseButton button, Qt::KeyboardModifiers /*mods*/)
{
    if (button != Qt::LeftButton)
        return;
    const Drag drag = m_drag;
    m_drag = Drag::None;
    m_handle = Handle::None;

    if (drag == Drag::Selecting) {
        // Клик без перетаскивания выделяет монитор под курсором (в мёртвой зоне — ничего).
        if ((pos - m_pressPos).manhattanLength() < kClickThreshold)
            setSelection(screenAt(pos));
        else
            setSelection(rectFromPoints(m_pressPos, pos).intersected(bounds()));
        applyDefaultTool(); // выделение зафиксировано отпусканием кнопки
    }
    else if (drag == Drag::Drawing)
        finishAnnotation();
    updateToolbar();
    updateCursor(pos);
}

// Двойной клик не должен работать как второе нажатие: окно не передаёт его как mousePress, здесь — ничего.
void OverlayController::mouseDoubleClick(QPoint /*pos*/)
{
}

void OverlayController::wheel(int angleDeltaY)
{
    // Тачпады и hi-res колёса шлют дробные шаги: копим, меняем толщину на 1 за каждые 120 единиц.
    const int delta = angleDeltaY;
    // Смена направления сбрасывает остаток: иначе он гасил бы первые единицы обратного движения.
    if ((delta > 0 && m_wheelAccumulator < 0) || (delta < 0 && m_wheelAccumulator > 0))
        m_wheelAccumulator = 0;
    m_wheelAccumulator += delta;
    const int steps = m_wheelAccumulator / 120;
    if (steps != 0) {
        m_wheelAccumulator -= steps * 120;
        setThickness(m_style.thickness + steps);
    }
}

void OverlayController::keyPress(QKeyEvent* event)
{
    if (handleTextKey(event))
        return;
    const Qt::KeyboardModifiers mods = event->modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const int key = layoutIndependentKey(event->key(), event->nativeScanCode());

    if (key == Qt::Key_Escape) {
        cancel();
        return;
    }
    if (ctrl) {
        switch (key) {
        case Qt::Key_C:
            copyResult();
            return;
        case Qt::Key_S:
            if (shift)
                saveAs();
            else
                saveQuick();
            return;
        case Qt::Key_Z:
            if (shift)
                redo();
            else
                undo();
            return;
        case Qt::Key_Y:
            redo();
            return;
        case Qt::Key_A:
            setSelection(bounds());
            applyDefaultTool();
            return;
        default:
            return;
        }
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        copyResult();
        return;
    }
    if (mods != Qt::NoModifier || m_selection.isEmpty())
        return;
    switch (key) {
    case Qt::Key_P: setTool(Tool::Pen); return;
    case Qt::Key_M: setTool(Tool::Marker); return;
    case Qt::Key_L: setTool(Tool::Line); return;
    case Qt::Key_A: setTool(Tool::Arrow); return;
    case Qt::Key_R: setTool(Tool::Rect); return;
    case Qt::Key_E: setTool(Tool::Ellipse); return;
    case Qt::Key_T: setTool(Tool::Text); return;
    case Qt::Key_N: setTool(Tool::Counter); return;
    case Qt::Key_B: setTool(Tool::Pixelate); return;
    case Qt::Key_V: setTool(Tool::None); return;
    case Qt::Key_1: case Qt::Key_2: case Qt::Key_3: case Qt::Key_4: case Qt::Key_5:
        setThickness(Toolbar::thicknessPresets().at(key - Qt::Key_1));
        return;
    default: return;
    }
}

// ---- Отрисовка ----

void OverlayController::paint(QPainter& painter, const QRect& imageRect) const
{
    painter.drawImage(imageRect.topLeft(), m_dimmed, imageRect); // только видимая окну часть фона
    if (m_selection.isEmpty()) {
        if (m_drag == Drag::None)
            paintHint(painter);
        return;
    }
    if (m_document.annotations().isEmpty()) {
        // Без аннотаций выделение рисуется прямо из снимка, без промежуточной копии.
        const QRect visible = m_selection.intersected(imageRect);
        if (!visible.isEmpty())
            painter.drawImage(visible.topLeft(), m_capture.image, visible);
    } else {
        if (!m_cacheValid) {
            m_cache = ::render(m_capture.image, m_selection, m_document.annotations());
            m_cacheValid = true;
        }
        const QRect visible = m_selection.intersected(imageRect);
        if (!visible.isEmpty())
            painter.drawImage(visible.topLeft(), m_cache, visible.translated(-m_selection.topLeft()));
    }
    paintCurrent(painter);
    paintSelectionFrame(painter);
    paintSizeLabel(painter);
}

void OverlayController::paintSelectionFrame(QPainter& painter) const
{
    const QRect& s = m_selection;
    painter.setPen(QPen(accentColor(), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(s.adjusted(0, 0, -1, -1));

    const QPoint handles[] = {
        s.topLeft(), QPoint(s.center().x(), s.top()), s.topRight(), QPoint(s.right(), s.center().y()),
        s.bottomRight(), QPoint(s.center().x(), s.bottom()), s.bottomLeft(), QPoint(s.left(), s.center().y()),
    };
    painter.setPen(Qt::NoPen);
    painter.setBrush(accentColor());
    for (const QPoint& p : handles)
        painter.drawRect(QRect(p.x() - kHandleSize / 2, p.y() - kHandleSize / 2, kHandleSize, kHandleSize));
}

void OverlayController::paintSizeLabel(QPainter& painter) const
{
    const QString text = sizeLabelText(m_selection);
    const QRect box = sizeLabelRect(m_selection, text, painter.font(), m_capture.screens);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 160));
    painter.drawRoundedRect(box, 4, 4);
    painter.setPen(Qt::white);
    painter.drawText(box, Qt::AlignCenter, text);
}

void OverlayController::paintHint(QPainter& painter) const
{
    QRect screen = m_hintScreen;
    if (screen.isEmpty())
        screen = m_capture.screens.isEmpty() ? bounds() : m_capture.screens.constFirst();
    const QString text = qtTrId("overlay.hint");
    QFont font = painter.font();
    font.setPixelSize(18);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const QSize size(metrics.horizontalAdvance(text) + 40, metrics.height() + 24);
    const QRect box(screen.center() - QPoint(size.width() / 2, size.height() / 2), size);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 180));
    painter.drawRoundedRect(box, 8, 8);
    painter.setPen(Qt::white);
    painter.drawText(box, Qt::AlignCenter, text);
}
