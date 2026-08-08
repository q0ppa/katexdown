// Kate destroys a document when its editor tab closes, and it does so in two shapes:
// closing one tab runs closeUrl() first (which empties the url and then the buffer),
// while closing several at once deletes the document with its text still intact.
// Either way the preview widget outlives the document, so it has to keep rendering
// from its own copy of the source.

#include "previewwidget.h"

#include <QDeadlineTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <memory>

#include <KTextEditor/Document>
#include <KTextEditor/Editor>

namespace
{
const QLatin1String Body("the cached body line");
const QLatin1String Closed("(closed)");
const QLatin1String ImageName("red.png");
constexpr int ImageWidth = 2;
// A 2x1 red PNG, inline so the test carries no fixture file. Two pixels wide because a
// failed load reports naturalWidth 0 and a placeholder reports 1.
const QByteArray RedPng =
    QByteArray::fromHex("89504e470d0a1a0a0000000d494844520000000200000001080200000"
                        "07b40e8dd0000000d4944415478da63f8cfc000440008fe01ff19c06be"
                        "70000000049454e44ae426082");

// Evaluate one expression in the page and hand back its value as a string.
QString evalJs(PreviewWidget *preview, const QString &code)
{
    auto *view = preview->findChild<QWebEngineView *>();
    if (!view) {
        return QString();
    }
    // The callback outlives this call if the deadline expires first, so the result
    // cannot live on the stack.
    auto slot = std::make_shared<std::pair<QString, bool>>(QString(), false);
    view->page()->runJavaScript(code, [slot](const QVariant &result) {
        slot->first = result.toString();
        slot->second = true;
    });
    QDeadlineTimer deadline(5000);
    while (!slot->second && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return slot->first;
}

// Read the rendered article back out of the page.
QString pageText(PreviewWidget *preview)
{
    return evalJs(preview, QStringLiteral("document.getElementById('content').innerText"));
}

// naturalWidth stays 0 for an image the page never fetched, which is what separates a
// blocked request from a rendered <img> tag.
int waitForImageWidth(PreviewWidget *preview)
{
    QDeadlineTimer deadline(20000);
    while (!deadline.hasExpired()) {
        const int width = evalJs(preview, QStringLiteral("document.images.length ? document.images[0].naturalWidth : 0")).toInt();
        if (width > 0) {
            return width;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return 0;
}

bool waitForPageText(PreviewWidget *preview, QLatin1String needle)
{
    QDeadlineTimer deadline(20000);
    while (!deadline.hasExpired()) {
        if (pageText(preview).contains(needle)) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
}
} // namespace

class PreviewLifecycleTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void survivesDocumentClose();
    void survivesDeletionWithoutCloseUrl();
    void reattachesToReopenedDocument();
    void loadsImageBesideTheDocument();

private:
    KTextEditor::Document *openDocument();

    QTemporaryDir m_dir;
    QString m_path;
};

void PreviewLifecycleTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_path = m_dir.filePath(QStringLiteral("sample.md"));
    QFile f(m_path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(QByteArrayLiteral("# katdown\n\nthe cached body line.\n")) > 0);
    f.close();

    QFile png(m_dir.filePath(ImageName));
    QVERIFY(png.open(QIODevice::WriteOnly));
    QCOMPARE(png.write(RedPng), RedPng.size());
    png.close();
}

KTextEditor::Document *PreviewLifecycleTest::openDocument()
{
    KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
    doc->openUrl(QUrl::fromLocalFile(m_path));
    return doc;
}

// One editor tab closing: Kate calls closeUrl(), which clears the url before anything
// announces the close and empties the buffer straight after, then deletes the document.
void PreviewLifecycleTest::survivesDocumentClose()
{
    KTextEditor::Document *doc = openDocument();
    QTRY_VERIFY(doc->text().contains(Body));

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    QVERIFY(waitForPageText(preview.get(), Body));

    QVERIFY(doc->closeUrl());
    delete doc;

    // Well past the render debounce and any reload the close could have kicked off.
    QTest::qWait(2000);
    const QString text = pageText(preview.get());
    QVERIFY2(text.contains(Body), qPrintable(QStringLiteral("preview went blank, article reads: '%1'").arg(text)));
}

// Closing several tabs at once takes the other path: no closeUrl, so the document is
// deleted with its url and text still intact and detachDocument() is the only warning.
void PreviewLifecycleTest::survivesDeletionWithoutCloseUrl()
{
    KTextEditor::Document *doc = openDocument();
    QTRY_VERIFY(doc->text().contains(Body));

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    QVERIFY(waitForPageText(preview.get(), Body));

    preview->detachDocument();
    delete doc;

    QTest::qWait(2000);
    const QString text = pageText(preview.get());
    QVERIFY2(text.contains(Body), qPrintable(QStringLiteral("preview went blank, article reads: '%1'").arg(text)));
    QVERIFY(preview->windowTitle().contains(Closed));
}

// Reopening the file hands the frozen tab a fresh document and it goes live again.
void PreviewLifecycleTest::reattachesToReopenedDocument()
{
    KTextEditor::Document *doc = openDocument();
    QTRY_VERIFY(doc->text().contains(Body));

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    QVERIFY(waitForPageText(preview.get(), Body));
    const QUrl url = preview->documentUrl();
    QCOMPARE(url, QUrl::fromLocalFile(m_path));

    QVERIFY(doc->closeUrl());
    preview->detachDocument();
    delete doc;

    QTest::qWait(500);
    QVERIFY(preview->windowTitle().contains(Closed));
    // The url survives the document: it is what the reopened file is matched against.
    QCOMPARE(preview->documentUrl(), url);

    KTextEditor::Document *reopened = openDocument();
    QTRY_VERIFY(reopened->text().contains(Body));
    preview->attachDocument(reopened, nullptr);
    QVERIFY(!preview->windowTitle().contains(Closed));

    reopened->setText(QStringLiteral("# live again\n\nthe reattached body line.\n"));
    QVERIFY(waitForPageText(preview.get(), QLatin1String("the reattached body line")));
    delete reopened;
}

// An image referenced by a relative path has to survive LocalFileGuard, which compares the
// request's canonical path against the document folder's canonical path. Those two strings
// are produced on different code paths, and a filesystem that matches names
// case-insensitively or hands out 8.3 short names can make them differ while naming the same
// file. The guard blocks on any mismatch, so the failure is a silently missing image rather
// than an error anyone sees.
void PreviewLifecycleTest::loadsImageBesideTheDocument()
{
    KTextEditor::Document *doc = openDocument();
    QTRY_VERIFY(doc->text().contains(Body));

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    QVERIFY(waitForPageText(preview.get(), Body));

    doc->setText(QStringLiteral("# katdown\n\n![red](%1)\n").arg(ImageName));
    const int width = waitForImageWidth(preview.get());
    QVERIFY2(width == ImageWidth, qPrintable(QStringLiteral("image beside the document did not load, naturalWidth is %1").arg(width)));

    delete doc;
}

QTEST_MAIN(PreviewLifecycleTest)

#include "previewlifecycletest.moc"
