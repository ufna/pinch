#include <QtTest>

#include "i18n.h"
#include "overlaycontroller.h"
#include "overlaysession.h"
#include "screenview.h"
#include "toolbar.h"

#include <QLocale>
#include <QPainter>
#include <QPaintEvent>
#include <QToolButton>

namespace {
// Два «монитора»: 400×300 и 400×200 справа; зона (400..799, 200..299) — мёртвая (окна там нет).
// Левая половина снимка серая 100, правая — 150: по пикселю окна видно, какую часть кадра оно рисует.
Capture makeCapture()
{
    Capture c;
    c.image = QImage(800, 300, QImage::Format_RGB32);
    c.image.fill(QColor(100, 100, 100));
    {
        QPainter painter(&c.image);
        painter.fillRect(QRect(400, 0, 400, 300), QColor(150, 150, 150));
    }
    c.origin = QPoint(0, 0);
    c.screens = {QRect(0, 0, 400, 300), QRect(400, 0, 400, 200)};
    return c;
}

struct Env {
    QTemporaryDir dir;
    OverlayController ctl{makeCapture(), Settings{}, dir.path()};
    OverlaySession session{&ctl};
    bool finished = false;
    int finishCount = 0; // finished должен приходить ровно один раз
    QImage copied;
    bool wasCopied = false;
    ScreenView* grabber = nullptr; // окно, получившее нажатие: ему идут движения и отпускание до отпускания кнопки

    Env()
    {
        QObject::connect(&ctl, &OverlayController::finished, [this] {
            finished = true;
            ++finishCount;
        });
        QObject::connect(&ctl, &OverlayController::copyRequested, [this](const QImage& image) {
            copied = image;
            wasCopied = true;
        });
        session.show();
    }

    ScreenView* view(int i = 0) const { return session.views().at(i); }

    ScreenView* viewAt(QPoint pos) const
    {
        for (ScreenView* v : session.views()) {
            if (v->imageRect().contains(pos))
                return v;
        }
        return nullptr;
    }

    bool allVisible() const
    {
        for (ScreenView* v : session.views()) {
            if (!v->isVisible())
                return false;
        }
        return true;
    }

    bool allHidden() const
    {
        for (ScreenView* v : session.views()) {
            if (v->isVisible())
                return false;
        }
        return true;
    }
};

void sendMouse(QWidget* w, QEvent::Type type, QPoint local, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent e(type, QPointF(local), w->mapToGlobal(QPointF(local)), button, buttons, mods);
    QApplication::sendEvent(w, &e);
}

// Событие мыши в координатах изображения. Пока кнопка зажата (от нажатия до отпускания), события получает окно
// нажатия (неявный захват указателя) в своих локальных координатах, даже за своими пределами; иначе — окно под точкой.
void mouse(Env& e, QEvent::Type type, QPoint pos, Qt::MouseButton button, Qt::MouseButtons buttons,
           Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const bool held = type == QEvent::MouseButtonRelease || (type == QEvent::MouseMove && buttons != Qt::NoButton);
    ScreenView* view = held && e.grabber ? e.grabber : e.viewAt(pos);
    QVERIFY2(view, "no view under the point");
    if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick)
        e.grabber = view;
    else if (type == QEvent::MouseButtonRelease)
        e.grabber = nullptr;
    sendMouse(view, type, pos - view->imageRect().topLeft(), button, buttons, mods);
}

void drag(Env& e, QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    mouse(e, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, mods);
    mouse(e, QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton, mods);
    mouse(e, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, mods);
    mouse(e, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, mods);
}

void click(Env& e, QPoint pos)
{
    mouse(e, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    mouse(e, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

// Клик по виджету (панели) в его локальных координатах.
void clickWidget(QWidget* w, QPoint local)
{
    sendMouse(w, QEvent::MouseButtonPress, local, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, local, Qt::LeftButton, Qt::NoButton);
}

void wheel(QWidget* w, QPointF local, int dy)
{
    QWheelEvent ev(local, w->mapToGlobal(local), QPoint(), QPoint(0, dy), Qt::NoButton, Qt::NoModifier,
                   Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &ev);
}

// Нажатие клавиши так, как его присылает X11 в кириллической раскладке.
void rawKey(QWidget* w, int key, Qt::KeyboardModifiers mods, quint32 scanCode, const QString& text)
{
    QKeyEvent e(QEvent::KeyPress, key, mods, scanCode, 0, 0, text);
    QApplication::sendEvent(w, &e);
}

// Платформа offscreen не умеет поднимать окна и захватывать клавиатуру, а сессия делает это при показе (как на X11):
// эти предупреждения платформы — шум. Остальные сообщения идут дальше, в обработчик QTest.
QtMessageHandler g_previousHandler = nullptr;
void filterPlatformNoise(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (message == QLatin1String("This plugin does not support raise()")
        || message == QLatin1String("This plugin does not support grabbing the keyboard"))
        return;
    g_previousHandler(type, context, message);
}

// Цвет фона вне выделения: снимок под тем же затемнением, что и в контроллере.
QRgb dimmed(QRgb rgb)
{
    QImage image(1, 1, QImage::Format_RGB32);
    image.fill(rgb);
    QPainter painter(&image);
    painter.fillRect(image.rect(), QColor(0, 0, 0, 120));
    painter.end();
    return image.pixel(0, 0);
}
}

class TestOverlay : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        g_previousHandler = qInstallMessageHandler(filterPlatformNoise);
        // Без таблиц qtTrId вернул бы голый ключ, и .arg() в подсказках панели предупреждал бы о лишних аргументах.
        installTranslations(*QCoreApplication::instance(), QLocale(QStringLiteral("en_US")));
    }
    void cleanupTestCase() { qInstallMessageHandler(g_previousHandler); }

    void dragSelects()
    {
        Env e;
        QVERIFY(!e.ctl.toolbar()->isVisible());
        drag(e, {10, 10}, {110, 60});
        QCOMPARE(e.ctl.selection(), QRect(10, 10, 101, 51));
        QVERIFY(e.ctl.toolbar()->isVisible());
    }

    void selectionRepaintsOnlyChangedStrips()
    {
        Env e;
        struct Recorder : QObject {
            OverlayController* controller;
            QImage frame{400, 300, QImage::Format_RGB32};
            QRegion dirty;
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::Paint) {
                    const QRegion region = static_cast<QPaintEvent*>(event)->region();
                    dirty += region;
                    QPainter painter(&frame);
                    for (const QRect& rect : region) {
                        painter.setClipRect(rect);
                        controller->paint(painter, rect);
                    }
                }
                return false;
            }
        } recorder;
        recorder.controller = &e.ctl;
        e.view()->installEventFilter(&recorder);
        e.view()->update();
        QApplication::processEvents();
        mouse(e, QEvent::MouseButtonPress, {20, 40}, Qt::LeftButton, Qt::LeftButton);
        mouse(e, QEvent::MouseMove, {350, 260}, Qt::NoButton, Qt::LeftButton);
        QApplication::processEvents();
        for (const QPoint end : {QPoint(355, 265), QPoint(340, 250)}) {
            recorder.dirty = {};
            mouse(e, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
            QApplication::processEvents();
            QVERIFY(!recorder.dirty.isEmpty());
            QVERIFY(!recorder.dirty.contains(QPoint(180, 150)));
            QImage expected(400, 300, QImage::Format_RGB32);
            QPainter painter(&expected);
            e.ctl.paint(painter, expected.rect());
            painter.end();
            QCOMPARE(recorder.frame, expected);
        }
    }

    void clickSelectsScreen()
    {
        Env e;
        click(e, {500, 100});
        QCOMPARE(e.ctl.selection(), QRect(400, 0, 400, 200));
    }

    void noViewCoversDeadZone()
    {
        Env e;
        QCOMPARE(e.session.views().size(), 2);
        QVERIFY(!e.viewAt({500, 250}));
    }

    void dragBeyondEdgeClamps()
    {
        Env e;
        QCOMPARE(e.viewAt({700, 150}), e.view(1));
        drag(e, {700, 150}, {900, 400});
        QCOMPARE(e.ctl.selection(), QRect(700, 150, 100, 150));
    }

    void selectionAcrossViews()
    {
        Env e;
        QCOMPARE(e.viewAt({350, 100}), e.view(0));
        drag(e, {350, 100}, {450, 150}); // все события — первому окну, конец — за его правым краем
        QCOMPARE(e.ctl.selection(), QRect(350, 100, 101, 51));
    }

    void toolbarMovesToOtherView()
    {
        Env e;
        click(e, {500, 100});
        QCOMPARE(e.ctl.selection(), QRect(400, 0, 400, 200));
        QCOMPARE(e.ctl.toolbar()->parentWidget(), e.view(1));
        QVERIFY(e.ctl.toolbar()->isVisible());
        QTest::keyClick(e.view(), Qt::Key_V);
        drag(e, {10, 10}, {300, 250}); // вне выделения без инструмента — новое выделение на первом мониторе
        QCOMPARE(e.ctl.selection(), QRect(10, 10, 291, 241));
        QCOMPARE(e.ctl.toolbar()->parentWidget(), e.view(0));
        QVERIFY(e.ctl.toolbar()->isVisible());
    }

    void keysFromSecondView()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        QCOMPARE(e.ctl.tool(), Tool::Pen);
        QTest::keyClick(e.view(1), Qt::Key_V);
        QCOMPARE(e.ctl.tool(), Tool::None);
        QTest::keyClick(e.view(1), Qt::Key_P);
        QCOMPARE(e.ctl.tool(), Tool::Pen);
    }

    void copyEmittedBeforeHide()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        bool visibleAtCopy = false;
        QObject::connect(&e.ctl, &OverlayController::copyRequested, [&] { visibleAtCopy = e.allVisible(); });
        QTest::keyClick(e.view(), Qt::Key_C, Qt::ControlModifier);
        QVERIFY(e.wasCopied);
        QVERIFY(visibleAtCopy);
        QVERIFY(e.allHidden());
    }

    void closingViewCancels_data()
    {
        QTest::addColumn<int>("index");
        QTest::newRow("first") << 0;
        QTest::newRow("second") << 1;
    }

    // Композитор закрыл одно окно (Alt+F4, Super+Q): отменяется весь оверлей, а не прячется одно окно.
    void closingViewCancels()
    {
        QFETCH(int, index);
        Env e;
        drag(e, {10, 10}, {110, 60});
        e.view(index)->close();
        QVERIFY(e.finished);
        QCOMPARE(e.finishCount, 1); // своё скрытие окон (hide) не присылает closeEvent и не отменяет повторно
        QVERIFY(e.allHidden());
        QVERIFY(!e.wasCopied);
    }

    void mirroredScreensShareOneView()
    {
        Capture c;
        c.image = QImage(400, 300, QImage::Format_RGB32);
        c.image.fill(QColor(100, 100, 100));
        c.screens = {QRect(0, 0, 400, 300), QRect(0, 0, 400, 300)}; // два монитора в режиме зеркала
        OverlayController ctl{c, Settings{}, QString()};
        OverlaySession session{&ctl};
        QCOMPARE(session.views().size(), 1);
        QCOMPARE(session.views().at(0)->imageRect(), QRect(0, 0, 400, 300));
    }

    void viewPaintsItsRegion()
    {
        Env e;
        QCOMPARE(e.view(1)->grab().toImage().pixel(10, 10), dimmed(qRgb(150, 150, 150)));
        QCOMPARE(e.view(0)->grab().toImage().pixel(10, 10), dimmed(qRgb(100, 100, 100)));
    }

    void ctrlASelectsAll()
    {
        Env e;
        QTest::keyClick(e.view(), Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(e.ctl.selection(), QRect(0, 0, 800, 300));
    }

    void moveSelection()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        QTest::keyClick(e.view(), Qt::Key_V); // после выделения активен карандаш: для перемещения нужен V
        drag(e, {50, 30}, {70, 40});
        QCOMPARE(e.ctl.selection(), QRect(30, 20, 101, 51));
    }

    void resizeByCorner()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        drag(e, {110, 60}, {150, 80});
        QCOMPARE(e.ctl.selection(), QRect(10, 10, 141, 71));
    }

    void dragOutsideStartsNewSelection()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        QTest::keyClick(e.view(), Qt::Key_V); // с инструментом клик вне выделения ничего не делает
        drag(e, {200, 100}, {300, 200});
        QCOMPARE(e.ctl.selection(), QRect(200, 100, 101, 101));
    }

    void toolKeys()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        const QList<QPair<Qt::Key, Tool>> keys = {
            {Qt::Key_P, Tool::Pen}, {Qt::Key_M, Tool::Marker}, {Qt::Key_L, Tool::Line},
            {Qt::Key_A, Tool::Arrow}, {Qt::Key_R, Tool::Rect}, {Qt::Key_E, Tool::Ellipse},
            {Qt::Key_T, Tool::Text}, {Qt::Key_N, Tool::Counter}, {Qt::Key_B, Tool::Pixelate},
            {Qt::Key_V, Tool::None},
        };
        for (const auto& [key, tool] : keys) {
            QTest::keyClick(e.view(), key);
            QCOMPARE(e.ctl.tool(), tool);
        }
    }

    void penActiveAfterSelection()
    {
        Env e;
        QCOMPARE(e.ctl.tool(), Tool::None);
        drag(e, {10, 10}, {110, 60});
        QCOMPARE(e.ctl.tool(), Tool::Pen);
        QVERIFY(e.ctl.toolbar()->findChild<QToolButton*>(QStringLiteral("tool-pen"))->isChecked());
    }

    void dragInsideDrawsAfterSelection()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        const QRect sel = e.ctl.selection();
        drag(e, {50, 50}, {100, 80});
        QCOMPARE(e.ctl.document().annotations().size(), 1);
        QCOMPARE(e.ctl.document().annotations().at(0).tool, Tool::Pen);
        QCOMPARE(e.ctl.selection(), sel);
    }

    void penNotForcedAfterUserChoseNone()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        QTest::keyClick(e.view(), Qt::Key_V);
        drag(e, {200, 100}, {300, 200});
        QCOMPARE(e.ctl.selection(), QRect(200, 100, 101, 101));
        QCOMPARE(e.ctl.tool(), Tool::None);
    }

    void ctrlAAlsoSelectsPen()
    {
        Env e;
        QTest::keyClick(e.view(), Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(e.ctl.tool(), Tool::Pen);
    }

    void clickScreenAlsoSelectsPen()
    {
        Env e;
        click(e, {500, 100});
        QCOMPARE(e.ctl.tool(), Tool::Pen);
    }

    void toolKeysIgnoredWithoutSelection()
    {
        Env e;
        QTest::keyClick(e.view(), Qt::Key_P);
        QCOMPARE(e.ctl.tool(), Tool::None);
    }

    void toolbarButtonChangesTool()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        e.ctl.toolbar()->findChild<QToolButton*>(QStringLiteral("tool-rect"))->click();
        QCOMPARE(e.ctl.tool(), Tool::Rect);
    }

    void cyrillicLayoutHotkeys()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        QTest::keyClick(e.view(), Qt::Key_V); // сбросить умолчательный карандаш, чтобы проверка ловила отображение «З» → P
        QCOMPARE(e.ctl.tool(), Tool::None);
        rawKey(e.view(), 0x0417, Qt::NoModifier, 33, QStringLiteral("з"));
        QCOMPARE(e.ctl.tool(), Tool::Pen);
        rawKey(e.view(), 0x0421, Qt::ControlModifier, 54, QString());
        QVERIFY(e.wasCopied);
    }

    void ctrlCCopiesSelection()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        QTest::keyClick(e.view(), Qt::Key_C, Qt::ControlModifier);
        QVERIFY(e.wasCopied);
        QCOMPARE(e.copied.size(), QSize(101, 51));
        QCOMPARE(e.copied.pixel(5, 5), qRgb(100, 100, 100));
        QVERIFY(e.allHidden());
        QVERIFY(!e.finished);
    }

    void enterCopies()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        QTest::keyClick(e.view(), Qt::Key_Return);
        QVERIFY(e.wasCopied);
    }

    void outputIgnoredWithoutSelection()
    {
        Env e;
        QTest::keyClick(e.view(), Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(e.view(), Qt::Key_S, Qt::ControlModifier);
        QVERIFY(!e.wasCopied);
        QVERIFY(!e.finished);
        QVERIFY(e.allVisible());
    }

    void quickSaveWritesFile()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        QTest::keyClick(e.view(), Qt::Key_S, Qt::ControlModifier);
        QVERIFY(e.finished);
        const QStringList files = QDir(e.dir.path()).entryList({QStringLiteral("*.png")}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QCOMPARE(QImage(e.dir.filePath(files.first())).size(), QSize(101, 51));
    }

    void escapeFinishes()
    {
        Env e;
        QTest::keyClick(e.view(), Qt::Key_Escape);
        QVERIFY(e.finished);
        QCOMPARE(e.finishCount, 1);
        QVERIFY(e.allHidden());
    }

    void wheelChangesThickness()
    {
        Env e;
        wheel(e.view(), {50, 50}, 120);
        QCOMPARE(e.ctl.style().thickness, 5);
        for (int i = 0; i < 50; ++i)
            wheel(e.view(), {50, 50}, 120);
        QCOMPARE(e.ctl.style().thickness, 40);
        for (int i = 0; i < 50; ++i)
            wheel(e.view(), {50, 50}, -120);
        QCOMPARE(e.ctl.style().thickness, 1);
    }

    void toolbarBackgroundClickDoesNotReachOverlay()
    {
        Env e;
        drag(e, {10, 10}, {110, 60});
        const QRect before = e.ctl.selection();
        clickWidget(e.ctl.toolbar(), {1, 1});
        QCOMPARE(e.ctl.selection(), before);
    }

    void toolbarClickWithCounterAddsNothing()
    {
        Env e;
        click(e, {100, 100}); // выделен весь левый монитор: панель внутри выделения
        QCOMPARE(e.ctl.selection(), QRect(0, 0, 400, 300));
        const auto* view = qobject_cast<ScreenView*>(e.ctl.toolbar()->parentWidget());
        QVERIFY(view);
        // Геометрия панели — в координатах её окна; переводим в координаты изображения.
        QVERIFY(e.ctl.toolbar()->geometry().translated(view->imageRect().topLeft()).intersects(e.ctl.selection()));
        QTest::keyClick(e.view(), Qt::Key_N);
        QCOMPARE(e.ctl.tool(), Tool::Counter);
        clickWidget(e.ctl.toolbar(), {1, 1});
        QVERIFY(e.ctl.document().annotations().isEmpty());
    }

    void digitKeysSelectPresets()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_3);
        QCOMPARE(e.ctl.style().thickness, 8);
        QTest::keyClick(e.view(), Qt::Key_5);
        QCOMPARE(e.ctl.style().thickness, 24);
    }

    void digitsGoIntoText()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_T);
        click(e, {60, 60});
        const int before = e.ctl.style().thickness;
        QTest::keyClicks(e.view(), QStringLiteral("12"));
        QCOMPARE(e.ctl.style().thickness, before);
        QTest::keyClick(e.view(), Qt::Key_Escape);
        QCOMPARE(e.ctl.document().annotations().at(0).text, QStringLiteral("12"));
    }

    void toolbarPresetChangesThickness()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        e.ctl.toolbar()->findChild<QToolButton*>(QStringLiteral("thickness-4"))->click();
        QCOMPARE(e.ctl.style().thickness, 24);
    }

    void wheelDirectionChangeResetsAccumulator()
    {
        Env e;
        QCOMPARE(e.ctl.style().thickness, 4);
        wheel(e.view(), {50, 50}, 100);
        QCOMPARE(e.ctl.style().thickness, 4);
        wheel(e.view(), {50, 50}, -120);
        QCOMPARE(e.ctl.style().thickness, 3);
    }

    void wheelOverToolbarStillChangesThickness()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        const int before = e.ctl.style().thickness;
        wheel(e.ctl.toolbar(), {1, 1}, 120);
        QCOMPARE(e.ctl.style().thickness, before + 1);
    }

    void smallWheelStepsAccumulate()
    {
        Env e;
        const int before = e.ctl.style().thickness;
        for (int i = 0; i < 2; ++i) {
            wheel(e.view(), {50, 50}, 60);
            if (i == 0)
                QCOMPARE(e.ctl.style().thickness, before);
        }
        QCOMPARE(e.ctl.style().thickness, before + 1);
    }

    void doubleClickDoesNotActAsSecondPress()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_N);
        click(e, {100, 100});
        mouse(e, QEvent::MouseButtonDblClick, {100, 100}, Qt::LeftButton, Qt::LeftButton);
        mouse(e, QEvent::MouseButtonRelease, {100, 100}, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(e.ctl.document().annotations().size(), 1);
    }

    void drawRectAddsAnnotation()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_R);
        drag(e, {50, 50}, {150, 120});
        QCOMPARE(e.ctl.document().annotations().size(), 1);
        const Annotation& a = e.ctl.document().annotations().at(0);
        QCOMPARE(a.tool, Tool::Rect);
        QCOMPARE(a.points, (QVector<QPoint>{{50, 50}, {150, 120}}));
        QCOMPARE(a.style.color, Settings().color);
        QCOMPARE(e.ctl.result().pixel(40, 75), Settings().color.rgb()); // левая сторона x=50
        QVERIFY(e.ctl.toolbar()->findChild<QToolButton*>(QStringLiteral("undo"))->isEnabled());
    }

    void shiftMakesSquare()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_R);
        drag(e, {50, 50}, {150, 120}, Qt::ShiftModifier);
        QCOMPARE(e.ctl.document().annotations().at(0).points.at(1), QPoint(150, 150));
    }

    void shiftSnapsArrow()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_A);
        drag(e, {50, 50}, {150, 58}, Qt::ShiftModifier);
        QCOMPARE(e.ctl.document().annotations().at(0).points.at(1), QPoint(150, 50));
    }

    void tinyShapesIgnored()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_R);
        click(e, {50, 50});
        QTest::keyClick(e.view(), Qt::Key_M);
        click(e, {60, 60});
        QTest::keyClick(e.view(), Qt::Key_L);
        click(e, {70, 70});
        QVERIFY(e.ctl.document().annotations().isEmpty());
    }

    void penStroke()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_P);
        drag(e, {50, 50}, {100, 80});
        QCOMPARE(e.ctl.document().annotations().size(), 1);
        QCOMPARE(e.ctl.document().annotations().at(0).tool, Tool::Pen);
        QCOMPARE(e.ctl.document().annotations().at(0).points.size(), 3);
    }

    void drawingOutsideSelectionIgnored()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_R);
        drag(e, {350, 250}, {380, 280});
        QVERIFY(e.ctl.document().annotations().isEmpty());
        QCOMPARE(e.ctl.selection(), QRect(10, 10, 291, 191));
    }

    void undoRedoKeys()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_R);
        drag(e, {50, 50}, {150, 120});
        QTest::keyClick(e.view(), Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(e.ctl.document().annotations().isEmpty());
        QTest::keyClick(e.view(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(e.ctl.document().annotations().size(), 1);
        QTest::keyClick(e.view(), Qt::Key_Z, Qt::ControlModifier);
        QTest::keyClick(e.view(), Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(e.ctl.document().annotations().size(), 1);
    }

    void counterClicks()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_N);
        click(e, {60, 60});
        click(e, {80, 80});
        QCOMPARE(e.ctl.document().annotations().size(), 2);
        QCOMPARE(e.ctl.document().annotations().at(1).tool, Tool::Counter);
        QCOMPARE(e.ctl.document().nextCounterNumber(), 3);
    }

    void textEscapeKeepsOverlay()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_T);
        click(e, {60, 60});
        QVERIFY(e.ctl.isEditingText());
        QTest::keyClicks(e.view(), QStringLiteral("ab"));
        QTest::keyClick(e.view(), Qt::Key_Backspace);
        QTest::keyClick(e.view(), Qt::Key_Escape);
        QVERIFY(!e.ctl.isEditingText());
        QVERIFY(!e.finished);
        QVERIFY(e.allVisible());
        QCOMPARE(e.ctl.document().annotations().size(), 1);
        QCOMPARE(e.ctl.document().annotations().at(0).text, QStringLiteral("a"));
        QTest::keyClick(e.view(), Qt::Key_Escape);
        QVERIFY(e.finished);
    }

    void textEnterIsNewline()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_T);
        click(e, {60, 60});
        QTest::keyClicks(e.view(), QStringLiteral("a"));
        QTest::keyClick(e.view(), Qt::Key_Return);
        QTest::keyClicks(e.view(), QStringLiteral("b"));
        QTest::keyClick(e.view(), Qt::Key_Escape);
        QVERIFY(!e.wasCopied);
        QCOMPARE(e.ctl.document().annotations().at(0).text, QStringLiteral("a\nb"));
    }

    void textLettersDontSwitchTool()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_T);
        click(e, {60, 60});
        QTest::keyClicks(e.view(), QStringLiteral("pmv"));
        QCOMPARE(e.ctl.tool(), Tool::Text);
        QTest::keyClick(e.view(), Qt::Key_Escape);
        QCOMPARE(e.ctl.document().annotations().at(0).text, QStringLiteral("pmv"));
    }

    void textCyrillic()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_T);
        click(e, {60, 60});
        rawKey(e.view(), 0x0416, Qt::NoModifier, 47, QStringLiteral("ж"));
        rawKey(e.view(), 0x0417, Qt::NoModifier, 33, QStringLiteral("з")); // физическая P — не инструмент
        QCOMPARE(e.ctl.tool(), Tool::Text);
        QTest::keyClick(e.view(), Qt::Key_Escape);
        QCOMPARE(e.ctl.document().annotations().at(0).text, QStringLiteral("жз"));
    }

    void emptyTextDiscarded()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_T);
        click(e, {60, 60});
        QTest::keyClick(e.view(), Qt::Key_Escape);
        QVERIFY(e.ctl.document().annotations().isEmpty());
        QVERIFY(!e.finished);
    }

    void clickCommitsTextAndStartsNew()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_T);
        click(e, {60, 60});
        QTest::keyClicks(e.view(), QStringLiteral("a"));
        click(e, {60, 120});
        QVERIFY(e.ctl.isEditingText());
        QTest::keyClicks(e.view(), QStringLiteral("b"));
        QTest::keyClick(e.view(), Qt::Key_Escape);
        QCOMPARE(e.ctl.document().annotations().size(), 2);
        QCOMPARE(e.ctl.document().annotations().at(1).points.at(0), QPoint(60, 120));
    }

    void ctrlCCommitsText()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_T);
        click(e, {60, 60});
        QTest::keyClicks(e.view(), QStringLiteral("hi"));
        QTest::keyClick(e.view(), Qt::Key_C, Qt::ControlModifier);
        QVERIFY(e.wasCopied);
        QCOMPARE(e.ctl.document().annotations().at(0).text, QStringLiteral("hi"));
    }

    void toolSwitchCommitsText()
    {
        Env e;
        drag(e, {10, 10}, {300, 200});
        QTest::keyClick(e.view(), Qt::Key_T);
        click(e, {60, 60});
        QTest::keyClicks(e.view(), QStringLiteral("x"));
        e.ctl.toolbar()->findChild<QToolButton*>(QStringLiteral("tool-rect"))->click();
        QVERIFY(!e.ctl.isEditingText());
        QCOMPARE(e.ctl.tool(), Tool::Rect);
        QCOMPARE(e.ctl.document().annotations().size(), 1);
    }
};

QTEST_MAIN(TestOverlay)
#include "test_overlay.moc"
