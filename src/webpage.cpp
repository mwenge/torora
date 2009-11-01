/*
 * Copyright 2009 Benjamin C. Meyer <ben@meyerhome.net>
 * Copyright 2009 Jakub Wieczorek <faw217@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "webpage.h"

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "cookiejar.h"
#include "downloadmanager.h"
#include "historymanager.h"
#include "networkaccessmanager.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "sslcert.h"
#include "sslstrings.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webpluginfactory.h"
#include "webview.h"

#include <qbuffer.h>
#include <qdesktopservices.h>
#include <qmessagebox.h>
#include <qnetworkreply.h>
#include <qnetworkrequest.h>
#include <qsettings.h>
#include <qwebhistory.h>
#include <qwebframe.h>

#include <quiloader.h>

#if QT_VERSION >= 0x040600 || defined(WEBKIT_TRUNK)
#include <qwebelement.h>
#else
#define QT_NO_UITOOLS
#endif

WebPluginFactory *WebPage::s_webPluginFactory = 0;
QString WebPage::s_userAgent;

JavaScriptExternalObject::JavaScriptExternalObject(QObject *parent)
    : QObject(parent)
{
}

void JavaScriptExternalObject::AddSearchProvider(const QString &url)
{
    ToolbarSearch::openSearchManager()->addEngine(QUrl(url));
}

Q_DECLARE_METATYPE(OpenSearchEngine*)
Q_DECLARE_METATYPE(QWebFrame*)
JavaScriptAroraObject::JavaScriptAroraObject(QObject *parent)
    : QObject(parent)
{
    static const char *translations[] = {
        QT_TR_NOOP("Welcome to Arora!"),
        QT_TR_NOOP("Arora Start"),
        QT_TR_NOOP("Search!"),
        QT_TR_NOOP("Search results provided by"),
        QT_TR_NOOP("About Arora")
    };
    Q_UNUSED(translations);

    qRegisterMetaType<OpenSearchEngine*>("OpenSearchEngine*");
}

QString JavaScriptAroraObject::translate(const QString &string)
{
    QString translatedString = trUtf8(string.toUtf8().constData());

    // If the translation is the same as the original string
    // it could not be translated.  In that case
    // try to translate using the QApplication domain
    if (translatedString != string)
        return translatedString;
    else
        return qApp->trUtf8(string.toUtf8().constData());
}

QObject *JavaScriptAroraObject::currentEngine() const
{
    return ToolbarSearch::openSearchManager()->currentEngine();
}

QString JavaScriptAroraObject::searchUrl(const QString &string) const
{
    return QString::fromUtf8(ToolbarSearch::openSearchManager()->currentEngine()->searchUrl(string).toEncoded());
}

WebPage::WebPage(QObject *parent)
    : WebPageProxy(parent)
    , m_openTargetBlankLinksIn(TabWidget::NewWindow)
    , m_javaScriptExternalObject(0)
    , m_javaScriptAroraObject(0)
{
    setPluginFactory(webPluginFactory());
    NetworkAccessManagerProxy *networkManagerProxy = new NetworkAccessManagerProxy(this);
    networkManagerProxy->setWebPage(this);
    networkManagerProxy->setPrimaryNetworkAccessManager(BrowserApplication::networkAccessManager());
    setNetworkAccessManager(networkManagerProxy);
    networkManagerProxy->setCookieJar(BrowserApplication::instance()->cookieJar());
    networkManagerProxy->cookieJar()->setParent(0); // Required for CookieJars shared by networkaccessmanagers
#if QT_VERSION >= 0x040600 || defined(WEBKIT_TRUNK)
#ifndef QT_NO_OPENSSL
    connect(networkManagerProxy, SIGNAL(sslErrors(QNetworkReply *, const QList<QSslError> &)),
            this, SLOT(handleSSLError(QNetworkReply *, const QList<QSslError> &)));
    connect(networkManagerProxy, SIGNAL(finished(QNetworkReply*)),
            SLOT(setSSLConfiguration(QNetworkReply*)));
#endif
#endif
    connect(this, SIGNAL(unsupportedContent(QNetworkReply *)),
            this, SLOT(handleUnsupportedContent(QNetworkReply *)));
    connect(this, SIGNAL(frameCreated(QWebFrame *)),
            this, SLOT(addExternalBinding(QWebFrame *)));
    connect(this, SIGNAL(networkRequestStarted(QWebFrame *, QNetworkRequest *)),
            this, SLOT(bindRequestToFrame(QWebFrame *, QNetworkRequest *)));
    addExternalBinding(mainFrame());
    loadSettings();
}

WebPage::~WebPage()
{
    setNetworkAccessManager(0);
}

WebPluginFactory *WebPage::webPluginFactory()
{
    if (!s_webPluginFactory)
        s_webPluginFactory = new WebPluginFactory(BrowserApplication::instance());
    return s_webPluginFactory;
}

QList<WebPageLinkedResource> WebPage::linkedResources(const QString &relation)
{
    QList<WebPageLinkedResource> resources;

#if QT_VERSION >= 0x040600 || defined(WEBKIT_TRUNK)
    QUrl baseUrl = mainFrame()->baseUrl();

    QWebElementCollection linkElements = mainFrame()->findAllElements(QLatin1String("html > head > link"));

    foreach (const QWebElement &linkElement, linkElements) {
        QString rel = linkElement.attribute(QLatin1String("rel"));
        QString href = linkElement.attribute(QLatin1String("href"));
        QString type = linkElement.attribute(QLatin1String("type"));
        QString title = linkElement.attribute(QLatin1String("title"));

        if (href.isEmpty() || type.isEmpty())
            continue;
        if (!relation.isEmpty() && rel != relation)
            continue;

        WebPageLinkedResource resource;
        resource.rel = rel;
        resource.type = type;
        resource.href = baseUrl.resolved(QUrl::fromEncoded(href.toUtf8()));
        resource.title = title;

        resources.append(resource);
    }
#else
    QString baseUrlString = mainFrame()->evaluateJavaScript(QLatin1String("document.baseURI")).toString();
    QUrl baseUrl = QUrl::fromEncoded(baseUrlString.toUtf8());

    QFile file(QLatin1String(":fetchLinks.js"));
    if (!file.open(QFile::ReadOnly))
        return resources;
    QString script = QString::fromUtf8(file.readAll());

    QVariantList list = mainFrame()->evaluateJavaScript(script).toList();
    foreach (const QVariant &variant, list) {
        QVariantMap map = variant.toMap();
        QString rel = map[QLatin1String("rel")].toString();
        QString type = map[QLatin1String("type")].toString();
        QString href = map[QLatin1String("href")].toString();
        QString title = map[QLatin1String("title")].toString();

        if (href.isEmpty() || type.isEmpty())
            continue;
        if (!relation.isEmpty() && rel != relation)
            continue;

        WebPageLinkedResource resource;
        resource.rel = rel;
        resource.type = type;
        resource.href = baseUrl.resolved(QUrl::fromEncoded(href.toUtf8()));
        resource.title = title;

        resources.append(resource);
    }
#endif

    return resources;
}

void WebPage::populateNetworkRequest(QNetworkRequest &request)
{
    if (request == lastRequest) {
        request.setAttribute((QNetworkRequest::Attribute)(pageAttributeId() + 1), lastRequestType);
    }
    WebPageProxy::populateNetworkRequest(request);
}

void WebPage::addExternalBinding(QWebFrame *frame)
{
#if QT_VERSION < 0x040600
    QWebSettings *defaultSettings = QWebSettings::globalSettings();
    if (!defaultSettings->testAttribute(QWebSettings::JavascriptEnabled))
        return;
#endif
    if (!m_javaScriptExternalObject)
        m_javaScriptExternalObject = new JavaScriptExternalObject(this);

    if (frame == 0) { // called from QWebFrame::javaScriptWindowObjectCleared
        frame = qobject_cast<QWebFrame*>(sender());

        if (frame->url().scheme() == QLatin1String("qrc")
            && frame->url().path() == QLatin1String("/startpage.html")) {

            if (!m_javaScriptAroraObject)
                m_javaScriptAroraObject = new JavaScriptAroraObject(this);

            frame->addToJavaScriptWindowObject(QLatin1String("arora"), m_javaScriptAroraObject);
        }
    } else { // called from QWebPage::frameCreated
        connect(frame, SIGNAL(javaScriptWindowObjectCleared()),
                this, SLOT(addExternalBinding()));
    }
    frame->addToJavaScriptWindowObject(QLatin1String("external"), m_javaScriptExternalObject);
}

QString WebPage::userAgentForUrl(const QUrl &url) const
{
    if (s_userAgent.isEmpty())
        s_userAgent = QWebPage::userAgentForUrl(url);
    return s_userAgent;
}

bool WebPage::acceptNavigationRequest(QWebFrame *frame, const QNetworkRequest &request,
                                      NavigationType type)
{
    /* If we ask for https://x.com, get redirected to http://x.com, which contains resources
       at https://y.com with SSL errors and we attempt to load the error page, WebKit will
       bring us here. Since we are displaying an error page, we don't want to go any further */
    qDebug() << request.url();

    lastRequest = request;
    lastRequestType = type;

    QString scheme = request.url().scheme();
    if (scheme == QLatin1String("mailto")
        || scheme == QLatin1String("ftp")) {
        BrowserApplication::instance()->askDesktopToOpenUrl(request.url());
        return false;
    }

    if (type == QWebPage::NavigationTypeFormResubmitted) {
        QMessageBox::StandardButton button = QMessageBox::warning(view(), tr("Resending POST request"),
                             tr("In order to display the site, the request along with all the data must be sent once again, "
                                "which may lead to some unexpected behaviour of the site e.g. the same action might be "
                                "performed once again. Do you want to continue anyway?"), QMessageBox::Yes | QMessageBox::No);
        if (button != QMessageBox::Yes)
            return false;
    }

    TabWidget::OpenUrlIn openIn = frame ? TabWidget::CurrentTab : TabWidget::NewWindow;
    openIn = TabWidget::modifyWithUserBehavior(openIn);

    // handle the case where we want to do something different then
    // what qwebpage would do
    if (openIn == TabWidget::NewSelectedTab
        || openIn == TabWidget::NewNotSelectedTab
        || (frame && openIn == TabWidget::NewWindow)) {
        if (WebView *webView = qobject_cast<WebView*>(view())) {
            TabWidget *tabWidget = webView->tabWidget();
            if (tabWidget) {
                WebView *newView = tabWidget->getView(openIn, webView);
                QWebPage *page = 0;
                if (newView)
                    page = newView->page();
                if (page && page->mainFrame())
                    page->mainFrame()->load(request);
            }
        }
        return false;
    }

    bool accepted = QWebPage::acceptNavigationRequest(frame, request, type);
    if (accepted && frame == mainFrame()) {
        m_requestedUrl = request.url();
        emit aboutToLoadUrl(request.url());
    }

    /*FIXME: There must be a better way of establishing that we need to
             flush the certificates associated with a given frame */
    /* m_aboutToDisplaySSLError needs to work per frame so that we don't mess up
       redirects in other frames */
	qDebug() << "frame url" << frame->url();
	qDebug() << "frame requested url" << frame->requestedUrl();
	qDebug() << "request url" << request.url();
    if (accepted && isNewWebsite(frame, request.url()))
        clearFrameSSLErrors(frame);

    return accepted;
}

bool WebPage::isNewWebsite(QWebFrame *frame, QUrl url)
{
    QListIterator<AroraSSLCertificate*> certs(allCerts());
    while (certs.hasNext()) {
        AroraSSLCertificate *cert = certs.next();
        if (cert->frames().contains(frame) || hasOverlappingMembers(cert->frames(), frame->childFrames())) {
            if (!cert->url().isParentOf(url) && cert->url() != url)
                return true;
        }
    }
    return false;
}

bool WebPage::hasOverlappingMembers(QList<QWebFrame *>certFrames, QList<QWebFrame *>childFrames)
{
    QList<QWebFrame*> frames;
    frames.append(childFrames);
    while (!frames.isEmpty()) {
        QWebFrame *f = frames.takeFirst();
        if (certFrames.contains(f))
            return true;
        frames += f->childFrames();
    }
    return false;
}

void WebPage::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    m_openTargetBlankLinksIn = (TabWidget::OpenUrlIn)settings.value(QLatin1String("openTargetBlankLinksIn"),
                                                                    TabWidget::NewSelectedTab).toInt();
    settings.endGroup();
    s_userAgent = settings.value(QLatin1String("userAgent")).toString();
}

QWebPage *WebPage::createWindow(QWebPage::WebWindowType type)
{
    Q_UNUSED(type);
    if (WebView *webView = qobject_cast<WebView*>(view())) {
        TabWidget *tabWidget = webView->tabWidget();
        if (tabWidget) {
            TabWidget::OpenUrlIn openIn = m_openTargetBlankLinksIn;
            openIn = TabWidget::modifyWithUserBehavior(openIn);
            return tabWidget->getView(openIn, webView)->page();
        }
    }
    return 0;
}

QObject *WebPage::createPlugin(const QString &classId, const QUrl &url,
                               const QStringList &paramNames, const QStringList &paramValues)
{
    Q_UNUSED(classId);
    Q_UNUSED(url);
    Q_UNUSED(paramNames);
    Q_UNUSED(paramValues);
#if !defined(QT_NO_UITOOLS)
    if (classId == QLatin1String("QPushButton")) {
        for (int i = 0; i < paramNames.count(); ++i) {
            if (paramValues[i].contains(QLatin1String("SSLProceedButton")) ||
                paramValues[i].contains(QLatin1String("SSLCancelButton"))) {
              QUiLoader loader;
              QObject *object;
              object = loader.createWidget(classId, view());
              QListIterator<AroraSSLCertificate*> certs(allCerts());
              while (certs.hasNext()) {
                  AroraSSLCertificate *cert = certs.next();
                  if (!cert->hasError())
                      continue;
                  QListIterator<AroraSSLError*> errors(cert->errors());
                  while (errors.hasNext()) {
                      AroraSSLError *error = errors.next();
                      if (paramValues[i] == QString(QLatin1String("SSLProceedButton%1")).arg(error->errorid())) {
                          connect (object, SIGNAL(clicked()), error, SLOT(loadFrame()));
                          return object;
                      }
                      if (paramValues[i] == QString(QLatin1String("SSLCancelButton%1")).arg(error->errorid())) {
                          connect (object, SIGNAL(clicked()), this, SLOT(sslCancel()));
                          return object;
                      }
                  }
              }
            }
        }
    }
#endif
    return 0;
}

// The chromium guys have documented many examples of incompatibilities that
// different browsers have when they mime sniff.
// http://src.chromium.org/viewvc/chrome/trunk/src/net/base/mime_sniffer.cc
//
// All WebKit ports should share a common set of rules to sniff content.
// By having this here we are yet another browser that has different behavior :(
// But sadly QtWebKit does no sniffing at all so we are forced to do something.
static bool contentSniff(const QByteArray &data)
{
    if (data.contains("<!doctype")
        || data.contains("<script")
        || data.contains("<html")
        || data.contains("<!--")
        || data.contains("<head")
        || data.contains("<iframe")
        || data.contains("<h1")
        || data.contains("<div")
        || data.contains("<font")
        || data.contains("<table")
        || data.contains("<a")
        || data.contains("<style")
        || data.contains("<title")
        || data.contains("<b")
        || data.contains("<body")
        || data.contains("<br")
        || data.contains("<p"))
        return true;
    return false;
}

void WebPage::handleUnsupportedContent(QNetworkReply *reply)
{
    if (!reply)
        return;

    QUrl replyUrl = reply->url();

    if (replyUrl.scheme() == QLatin1String("abp"))
        return;

    switch (reply->error()) {
    case QNetworkReply::NoError:
        if (reply->header(QNetworkRequest::ContentTypeHeader).isValid()) {
            BrowserApplication::downloadManager()->handleUnsupportedContent(reply);
            return;
        }
        break;
    case QNetworkReply::ProtocolUnknownError: {
        QSettings settings;
        settings.beginGroup(QLatin1String("WebView"));
        QStringList externalSchemes = settings.value(QLatin1String("externalSchemes")).toStringList();
        if (externalSchemes.contains(replyUrl.scheme())) {
            QDesktopServices::openUrl(replyUrl);
            return;
        }
        break;
    }
    default:
        break;
    }

    // Find the frame that has the unsupported content
    if (replyUrl.isEmpty() || replyUrl != m_requestedUrl)
        return;

    QWebFrame *notFoundFrame = mainFrame();
    if (!notFoundFrame)
        return;

    if (reply->header(QNetworkRequest::ContentTypeHeader).toString().isEmpty()) {
        // do evil
        QByteArray data = reply->readAll();
        if (contentSniff(data)) {
            notFoundFrame->setHtml(QLatin1String(data), replyUrl);
            return;
        }
    }

    // Generate translated not found error page with an image
    QFile notFoundErrorFile(QLatin1String(":/notfound.html"));
    if (!notFoundErrorFile.open(QIODevice::ReadOnly))
        return;
    QString title = tr("Error loading page: %1").arg(QString::fromUtf8(replyUrl.toEncoded()));
    QString html = QLatin1String(notFoundErrorFile.readAll());
    QPixmap pixmap = qApp->style()->standardIcon(QStyle::SP_MessageBoxWarning, 0, view()).pixmap(QSize(32, 32));
    QBuffer imageBuffer;
    imageBuffer.open(QBuffer::ReadWrite);
    if (pixmap.save(&imageBuffer, "PNG")) {
        html.replace(QLatin1String("IMAGE_BINARY_DATA_HERE"),
                     QLatin1String(imageBuffer.buffer().toBase64()));
    }
    html = html.arg(title,
                    reply->errorString(),
                    tr("When connecting to: %1.").arg(QString::fromUtf8(replyUrl.toEncoded())),
                    tr("Check the address for errors such as <b>ww</b>.arora-browser.org instead of <b>www</b>.arora-browser.org"),
                    tr("If the address is correct, try checking the network connection."),
                    tr("If your computer or network is protected by a firewall or proxy, make sure that the browser is permitted to access the network."));
    notFoundFrame->setHtml(html, replyUrl);
    // Don't put error pages to the history.
    BrowserApplication::instance()->historyManager()->removeHistoryEntry(replyUrl, notFoundFrame->title());
}

#ifndef QT_NO_OPENSSL
void WebPage::handleSSLError(QNetworkReply *reply, const QList<QSslError> &error)
{
    if (error.count() <= 0)
        return;

    AroraSSLError *sslError;
    sslError = new AroraSSLError(error, reply->url());
	/*Do we need to check if we already this SSL error for this frame? */
    addSSLCert(reply->url(), reply, sslError);

	/* We don't do this earlier because we want to display the error in the location
	   bar, so we need to store the cert. */
    if (BrowserApplication::instance()->m_sslwhitelist.contains(reply->url().host())) {
        reply->ignoreSslErrors();
        return;
    }
	handleSSLErrorPage(sslError, reply);
}

void WebPage::addSSLCert(const QUrl url, QNetworkReply *reply, AroraSSLError *sslError)
{
    if (alreadyHasSSLCertForUrl(url, reply, sslError))
        return;

    AroraSSLCertificate *certificate = new AroraSSLCertificate(sslError, reply->sslConfiguration(), url);
    QSslCipher sessionCipher = reply->sslConfiguration().sessionCipher();
    bool lowGrade = lowGradeEncryption(sessionCipher);
    m_sslLowGradeEncryption = lowGrade;
    certificate->setLowGradeEncryption(lowGrade);

    QWebFrame *certFrame = getFrame(reply->request());

    if (certFrame) {
        if (sslError) {
            sslError->setFrame(certFrame);
            sslError->setUrl(certFrame->requestedUrl());
        }
        certificate->addFrame(certFrame);
        addCertToFrame(certificate, certFrame);
    }
}

void WebPage::addCertToFrame(AroraSSLCertificate *certificate, QWebFrame *frame)
{
    AroraSSLCertificates certs;
    if (m_frameSSLCertificates.contains(frame)) {
        certs.append(m_frameSSLCertificates.value(frame));
        m_frameSSLCertificates.remove(frame);
    }
    certs.append(certificate);
    m_frameSSLCertificates.insert(frame, certs);
}

QList<AroraSSLCertificate*> WebPage::allCerts()
{
    AroraSSLCertificates certs;
    QMapIterator<QWebFrame*, AroraSSLCertificates> i(m_frameSSLCertificates);
    while (i.hasNext()) {
        i.next();
        certs.append(i.value());
    }
    return certs;
}

bool WebPage::alreadyHasSSLCertForUrl(const QUrl url, QNetworkReply *reply, AroraSSLError *sslError)
{

    AroraSSLCertificates certs = allCerts();

    for (int i = 0; i < certs.count(); ++i) {
        AroraSSLCertificate *cert = certs.at(i);
        if (cert->url().host() == url.host()) {
            QWebFrame *replyframe = getFrame(reply->request());
            cert->addFrame(replyframe);
            if (sslError) {
                sslError->setFrame(replyframe);
                cert->addError(sslError);
            }
            return true;
        }
    }
    return false;
}

void WebPage::markPollutedFrame(QNetworkReply *reply)
{
    AroraSSLCertificates certs = allCerts();
    QWebFrame *replyframe = getFrame(reply->request());
    for (int i = 0; i < certs.count(); ++i) {
        AroraSSLCertificate *cert = certs.at(i);
        if (cert->frames().contains(replyframe) &&
            cert->url().host() == replyframe->url().host()) {
            m_pollutedFrames.append(replyframe);
            return;
        }
    }
}

bool WebPage::frameIsPolluted(QWebFrame *frame, AroraSSLCertificate *cert)
{
    if (!containsFrame(frame))
        return false;
    if (m_pollutedFrames.contains(frame) &&
        cert->url().host() == frame->url().host())
        return true;
    return false;
}

bool WebPage::certHasPollutedFrame(AroraSSLCertificate *cert)
{
    QListIterator<QWebFrame*> frames(cert->frames());
    while (frames.hasNext()) {
        QWebFrame *frame = frames.next();
        if (frameIsPolluted(frame, cert))
            return true;
    }
    return false;
}

void WebPage::handleSSLErrorPage(AroraSSLError *error, QNetworkReply* reply)
{
    QWebFrame *replyframe = getFrame(reply->request());
	/* We don't ask the 'proceed/go back' question for SSL errors resulting from
	   resources loaded from the body of a html document. We just display them
	   as errors on the loaded page. */
	if (reply->url() != replyframe->requestedUrl())
		return;
    QString html = sslErrorHtml(error);
    replyframe->setHtml(html, replyframe->requestedUrl());
}

void WebPage::setSSLConfiguration(QNetworkReply *reply)
{
    if (reply->sslConfiguration().sessionCipher().isNull()) {
        markPollutedFrame(reply);
        return;
    }
    addSSLCert(reply->url(), reply, 0L);
}

int WebPage::sslSecurity()
{
    if (hasSSLErrors())
        return SECURITY_LOW;
    if (m_sslLowGradeEncryption || !m_pollutedFrames.isEmpty())
        return SECURITY_MEDIUM;
    return SECURITY_HIGH;
}

bool WebPage::hasSSLErrors()
{
    AroraSSLCertificates certs = allCerts();
    for (int i = 0; i < certs.count(); ++i) {
        AroraSSLCertificate *cert = certs.at(i);
        if (cert->hasError())
            return true;
    }
    return false;
}

bool WebPage::hasSSLCerts()
{
    if (allCerts().count() > 0)
        return true;
    return false;
}

bool WebPage::frameHasSSLErrors(QWebFrame *frame)
{
    AroraSSLCertificates certs = m_frameSSLCertificates.value(frame);
    for (int i = 0; i < certs.count(); ++i) {
        AroraSSLCertificate *cert = certs.at(i);
        if (cert->hasError())
            return true;
    }
    return false;
}

bool WebPage::frameHasThisSSLError(QWebFrame *frame, const QUrl url)
{
    AroraSSLCertificates certs = m_frameSSLCertificates.value(frame);
    for (int i = 0; i < certs.count(); ++i) {
        AroraSSLCertificate *cert = certs.at(i);
        if (cert->hasError() && cert->url().host() == url.host())
            return true;
    }
    return false;
}

bool WebPage::frameHasSSLCerts(QWebFrame *frame)
{
    if (m_frameSSLCertificates.contains(frame));
        return true;
    return false;
}

void WebPage::clearFrameSSLErrors(QWebFrame *frame)
{
    if (!frame)
        return;

    if (frame == mainFrame()) {
        m_frameSSLCertificates.clear();
        return;
    }

    /* We could have frames within frames, each of which may have SSL certs.*/
    QList<QWebFrame*> frames;
    frames.append(frame);
    while (!frames.isEmpty()) {
        QWebFrame *f = frames.takeFirst();
        handleDesignFlaw(f);
        frames += f->childFrames();
    }
}

void WebPage::handleDesignFlaw(QWebFrame *f)
{
    QListIterator<AroraSSLCertificate*> certs(m_frameSSLCertificates.value(f));
    /*FIXME: This exercise is a result of the way we link certificates and frames.
              We have to take each cert stored primarily by the frame and store
              it with the first other frame we find that uses it.
              For now it works, but it exposes a flawed design. */
    while (certs.hasNext()) {
        AroraSSLCertificate *cert = certs.next();
        cert->removeFrame(f);
        QListIterator<AroraSSLError*> errs(cert->errors());
        while (errs.hasNext()) {
            AroraSSLError *err = errs.next();
            if (err->frame() == f)
              cert->removeError(err);
        }
        if (!cert->frames().isEmpty())
            addCertToFrame(cert, cert->frames().first());
    }
    m_frameSSLCertificates.remove(f);
    /* It gets better: the frame may also use certs stored primarily in other
      frames, so we have to delete references to those as well! */
    certs = allCerts();
    while (certs.hasNext()) {
        AroraSSLCertificate *cert = certs.next();
        if (cert->frames().contains(f)) {
            cert->removeFrame(f);
            QListIterator<AroraSSLError*> errs(cert->errors());
            while (errs.hasNext()) {
                AroraSSLError *err = errs.next();
                if (err->frame() == f)
                  cert->removeError(err);
            }
        }
    }
}

void WebPage::bindRequestToFrame(QWebFrame *frame, QNetworkRequest *request)
{
    if (!frame)
        return;

    QVariant var;
    var.setValue(frame);
    request->setAttribute((QNetworkRequest::Attribute)(QNetworkRequest::User + 200), var);
}

bool WebPage::containsFrame(QWebFrame *frame)
{
    QList<QWebFrame*> frames;
    frames.append(mainFrame());
    while (!frames.isEmpty()) {
        QWebFrame *f = frames.takeFirst();
        if (f == frame)
            return true;
        frames += f->childFrames();
    }
    return false;
}

QWebFrame* WebPage::getFrame(const QNetworkRequest& request)
{
    QVariant v;
    QWebFrame *frame;
    v = request.attribute((QNetworkRequest::Attribute)(QNetworkRequest::User + 200));
    frame = v.value<QWebFrame*>();
    if (containsFrame(frame))
        return frame;
    return 0L;
}

void WebPage::sslCancel()
{
    BrowserMainWindow *mainWindow = BrowserApplication::instance()->mainWindow();
    if (mainWindow->currentTab()->history()->backItems(1).empty())
        return;
    mainWindow->currentTab()->history()->goToItem(mainWindow->currentTab()->history()->backItems(1).first()); // back
}

#endif
