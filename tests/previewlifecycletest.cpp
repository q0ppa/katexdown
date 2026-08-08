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

// Read the rendered article back out of the page.
QString pageText(PreviewWidget *preview)
{
    auto *view = preview->findChild<QWebEngineView *>();
    if (!view) {
        return QString();
    }
    // The callback outlives this call if the deadline expires first, so the result
    // cannot live on the stack.
    auto slot = std::make_shared<std::pair<QString, bool>>(QString(), false);
    view->page()->runJavaScript(QStringLiteral("document.getElementById('content').innerText"), [slot](const QVariant &result) {
        slot->first = result.toString();
        slot->second = true;
    });
    QDeadlineTimer deadline(5000);
    while (!slot->second && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return slot->first;
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

QTEST_MAIN(PreviewLifecycleTest)

#include "previewlifecycletest.moc"
