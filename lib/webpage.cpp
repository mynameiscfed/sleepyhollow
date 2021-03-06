#include <iostream>
#include <QApplication>
#include <QUrl>
#include <QAuthenticator>
#include <QWebPage>
#include <QWebFrame>
#include <QNetworkReply>

#include <hollow/core.h>
#include <hollow/networkaccessmanager.h>
#include <hollow/webpage.h>
#include <hollow/error.h>
#include <hollow/response.h>
#include <hollow/jserror.h>


// Copied from WebCore/loader/cache/MemoryCache.cpp
static const int cDefaultCacheCapacity = 8192 * 1024;
static int QUIT_IMMEDIATELY = 0;

void abort_now(int signal)
{
  switch (signal) {
  case SIGHUP:
  case SIGINT:
    QUIT_IMMEDIATELY = 1;
    break;
  default:
    break;
  }
}

WebPage::WebPage(QObject* parent, Config& config)
  : QWebPage(parent)
  , m_hasErrors(false)
  , m_shouldWaitForJS(false)
  , m_jsReady(false)
  , m_loadFinished(false)
  , m_lastResponse(NULL)
  , m_config(config)
{
  // Some more configuration to the page and to the page itself
  setForwardUnsupportedContent(true);

  // Everytime a new resource is requested, we increment our internal
  // counter and we won't return untill all the requested resources are
  // downloaded. This is why we have to extend the network access
  // manager and add our custom instance here
  m_networkAccessManager = NetworkAccessManager::instance();
  setNetworkAccessManager(m_networkAccessManager);

  connect(m_networkAccessManager, SIGNAL(resourceRequested(const QNetworkRequest&)),
          this, SLOT(handleResourceRequested(const QNetworkRequest&)),
          Qt::DirectConnection);

  connect(m_networkAccessManager, SIGNAL(authenticationRequired(QNetworkReply*, QAuthenticator*)),
          this, SLOT(handleAuthentication(QNetworkReply*, QAuthenticator*)),
          Qt::DirectConnection);

  // We need this object to track the replies and get info when the
  // request does not work
  connect(m_networkAccessManager, SIGNAL(finished(QNetworkReply *)),
          this, SLOT(handleNetworkReplies(QNetworkReply *)),
          Qt::DirectConnection);

  connect(mainFrame(), SIGNAL(javaScriptWindowObjectCleared()),
          this, SLOT(prepareJS()),
          Qt::DirectConnection);

  // Setting the internal finished flag to true when the page finished
  // loading. We use both this flag and the number of resources being
  // downloaded to ensure that the page is loaded
  connect(this, SIGNAL(loadFinished(bool)),
          this, SLOT(handleLoadFinished(bool)),
          Qt::DirectConnection);

  // Setting the default style for our page
  QWebSettings::globalSettings()->setDefaultTextEncoding("utf-8");
  QWebSettings::globalSettings()->setFontSize(QWebSettings::MinimumFontSize, 10);
  QWebSettings::globalSettings()->setFontSize(QWebSettings::MinimumLogicalFontSize, 10);
  QWebSettings::globalSettings()->setFontSize(QWebSettings::DefaultFontSize, 12);
  QWebSettings::globalSettings()->setFontSize(QWebSettings::DefaultFixedFontSize, 14);
  QWebSettings::globalSettings()->setAttribute(QWebSettings::LocalStorageEnabled, true);
  QWebSettings::globalSettings()->setAttribute(QWebSettings::JavaEnabled, false);
  QWebSettings::globalSettings()->setAttribute(QWebSettings::JavascriptEnabled, true);
  QWebSettings::globalSettings()->setAttribute(QWebSettings::PluginsEnabled, true);
  QWebSettings::globalSettings()->setAttribute(QWebSettings::JavascriptCanOpenWindows, false);
  QWebSettings::globalSettings()->setAttribute(QWebSettings::JavascriptCanAccessClipboard, true);
  QWebSettings::globalSettings()->setAttribute(QWebSettings::FrameFlatteningEnabled, true);
  QWebSettings::globalSettings()->setAttribute(QWebSettings::DeveloperExtrasEnabled, true);
  QWebSettings::globalSettings()->setAttribute(QWebSettings::LocalContentCanAccessRemoteUrls, true);
  QWebSettings::globalSettings()->setAttribute(QWebSettings::DnsPrefetchEnabled, true);

  mainFrame()->setScrollBarPolicy(Qt::Horizontal, Qt::ScrollBarAlwaysOff);
  mainFrame()->setScrollBarPolicy(Qt::Vertical, Qt::ScrollBarAlwaysOff);

  // Currently, this is the only control we have over the cache, using
  // it or not.

  if (!config["cache"]) {
    QWebSettings::globalSettings()->setObjectCacheCapacities(0, 0, 0);
  } else {
    QWebSettings::globalSettings()->setObjectCacheCapacities(0, cDefaultCacheCapacity, cDefaultCacheCapacity);
  }
  setViewportSize(QSize(1024, 768));
  signal(SIGINT, abort_now);
  signal(SIGHUP, abort_now);
}

// -- Public API --

bool
WebPage::finished()
{

  if (QUIT_IMMEDIATELY) {
    Error::set(Error::USER_ABORTED, "You aborted the request");
    return true;
  }

  if (m_shouldWaitForJS)
    return m_jsReady && m_loadFinished && allResourcesDownloaded();
  else
    return m_loadFinished && allResourcesDownloaded();
}

bool
WebPage::allResourcesDownloaded()
{
  int totalRequested = m_requestedResources.size();
  int totalRetrieved = m_retrievedResources.size();

  return totalRequested <= totalRetrieved;
}


bool
WebPage::hasErrors()
{
  return m_hasErrors;
}

QImage
WebPage::renderImage(int width, int height)
{
    QSize contentsSize = mainFrame()->contentsSize();
    if (contentsSize.width() < width) {
      contentsSize.setWidth(width);
    }
    if (contentsSize.height() < height) {
      contentsSize.setHeight(height);
    }
    QRect frameRect = QRect(QPoint(0, 0), contentsSize);

    QImage buffer(frameRect.size(), QImage::Format_ARGB32);

    QPainter painter;

    setViewportSize(contentsSize);
    painter.begin(&buffer);
    painter.setCompositionMode(QPainter::CompositionMode_Source);

    mainFrame()->render(&painter, QRegion(frameRect));

    painter.end();

    return buffer;
}

QByteArray
WebPage::renderPNGBase64(int width, int height)
{
  QImage rawPageRendering = renderImage(width, height);

  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  rawPageRendering.save(&buffer, "PNG");
  return bytes.toBase64();
}

QVariant
WebPage::evaluateJavaScript(QString& script)
{
  const char *error = NULL;

  // Cleaning existing JS errors because we are about to execute our
  // own js
  m_js_errors.clear();
  QVariant variant = mainFrame()->evaluateJavaScript(script);

  lastResponse();

  error = getJSTraceback();
  if (error != NULL) {
    Error::set(Error::BAD_JSON_RETURN_VALUE, error);
  }
  return variant;
}

const char*
WebPage::getJSTraceback(void)
{
  std::string traceback;
  char lineno[20];


  JSErrorListIterator iter;
  int pos;
  for (iter = m_js_errors.begin(), pos = 0; iter != m_js_errors.end(); iter++, pos++){
    traceback += "'";
    traceback += (*iter).getMessage();
    traceback += "' ";
    traceback += (*iter).getSourceID();
    traceback += ":";
    sprintf(lineno, "%d", (*iter).getLineNumber());
    traceback += lineno;
    traceback += "\n";
  }
  return traceback.length() > 0 ? traceback.data() : NULL;
}


Response *
WebPage::lastResponse()
{
  if (m_lastResponse) {
    // We're setting this value here because we can't get the response
    // text before this point. We have to wait the page to be processed
    // by the html engine after finishing the request.
    m_lastResponse->setHtml(mainFrame()->toHtml().toUtf8().constData());
    m_lastResponse->setText(mainFrame()->toPlainText().toUtf8().constData());
    m_lastResponse->setJSErrors(m_js_errors);
    m_lastResponse->setURL(mainFrame()->url().toString().toStdString().c_str());
    m_lastResponse->setRetrievedResources(m_retrievedResources);

    if (m_config["screenshot"])
      m_lastResponse->setScreenshotData(renderPNGBase64(m_config["width"], m_config["height"]).constData());
  }
  return m_lastResponse;
}


// -- Slots --

bool
WebPage::shouldInterruptJavaScript() {
  QApplication::processEvents(QEventLoop::AllEvents, 42);
  return false;
}


void
WebPage::prepareJS()
{
  m_shouldWaitForJS = true;
  m_jsReady = false;
  m_loadFinished = false;
  if (mainFrame()->evaluateJavaScript("typeof(window._SLEEPYHOLLOW) !== 'undefined';").toBool() == false) {
    mainFrame()->addToJavaScriptWindowObject("_SLEEPYHOLLOW", this);
    mainFrame()->evaluateJavaScript("document.addEventListener('DOMContentLoaded', function(){window._SLEEPYHOLLOW.setJSReady();}, false)");
  }
}

void
WebPage::setJSReady()
{
  m_jsReady = true;
}

void
WebPage::javaScriptConsoleMessage(const QString& message, int lineNumber, const QString& sourceID)
{
  m_js_errors.push_back(JSError(lineNumber, message.toStdString(), sourceID.toStdString()));
}

void
WebPage::javaScriptAlert(QWebFrame* frame, const QString& msg)
{
  // TODO: Save a list of those alerts and make them available in as
  // attributes of the Response in the python layer
  Q_UNUSED(frame);
  Q_UNUSED(msg);
}

bool
WebPage::javaScriptConfirm(QWebFrame* frame, const QString& msg)
{
  // TODO: implement a callback to be called in this situation, the
  // python layer should provide this callback as a python callable
  // that will end up being called here.

  Q_UNUSED(frame);
  Q_UNUSED(msg);
  return true;
}

bool
WebPage::javaScriptPrompt(QWebFrame* frame, const QString& msg, const QString& defaultValue, QString* result)
{
  // TODO: implement a callback to be called in this situation, the
  // python layer should provide this callback as a python callable
  // that will end up being called here.

  Q_UNUSED(frame);
  Q_UNUSED(msg);
  Q_UNUSED(defaultValue);
  Q_UNUSED(result);
  return true;
}

void
WebPage::handleLoadFinished(bool ok)
{
  m_loadFinished = true;
  m_hasErrors = !ok;
}


void
WebPage::handleResourceRequested(const QNetworkRequest& request)
{
  StringList::iterator iterator;
  std::string resource = request.url().toString().toStdString();

  for (iterator = m_requestedResources.begin();
       iterator != m_requestedResources.end();
       iterator++)
    if ((*iterator).compare(resource) == 0)
      return;


  m_requestedResources.push_back(resource);
}


void
WebPage::handleNetworkReplies(QNetworkReply *reply)
{
  time_t now;
  now = time(NULL);
  StringHashMap meta;
  meta["status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toString().toStdString();
  meta["url"] = reply->url().toString().toStdString();

  m_retrievedResources.push_back(meta);

  // Making sure we're handling the right url
  QUrl url = mainFrame()->url();
  if (url.isEmpty())
    url = mainFrame()->requestedUrl();
  if (url != reply->url()) {
    return;
  }

  // Cleaning up the last response. Maybe it's a good place to track
  // which requests the caller made.
  if (m_lastResponse) {
    delete m_lastResponse;
    m_lastResponse = NULL;
  }

  // Error handling
  QNetworkReply::NetworkError errCode = reply->error();
  switch (errCode) {
  case QNetworkReply::NoError:
    // Creating the new response object with the info gathered from this
    // reply
    m_lastResponse = buildResponseFromNetworkReply(reply, now);
    break;

  case QNetworkReply::ConnectionRefusedError:
    Error::set(Error::CONNECTION_REFUSED, C_STRING(reply->errorString()));
    break;

  default:
    // Maybe we can create a response when an error happens. If the
    // reply does not have all the data needed to create it, the method
    // buildResponseFromNetworkReply() will return NULL.
    if ((m_lastResponse = buildResponseFromNetworkReply(reply, now)) == NULL)
      Error::set(Error::UNKNOWN, C_STRING(reply->errorString()));
    break;
  }
}


// -- Helper methods/private API --


Response *
WebPage::buildResponseFromNetworkReply(QNetworkReply *reply, utimestamp when)
{
  QVariant statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
  QVariant reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute);

  // Not an HTTP error, let's give up
  if (!statusCode.isValid())
    return NULL;

  // Iterating over the headers
  StringHashMap headers;
  foreach (QByteArray headerName, reply->rawHeaderList())
    headers[headerName.constData()] = reply->rawHeader(headerName).constData();

  // We can't set the content right now, so we'll fill the text with an
  // empty string and let the ::lastResponse() method fill with the
  // right content
  return new Response(statusCode.toInt(),
                      TO_STRING(reply->url()),
                      "",
                      "",
                      TO_STRING(reason),
                      "",
                      headers,
                      when);
}


void
WebPage::handleAuthentication(QNetworkReply* reply, QAuthenticator* authenticator)
{
  Q_UNUSED(authenticator);
  Error::set(Error::BAD_CREDENTIALS, C_STRING(reply->errorString()));

  this->handleNetworkReplies(reply);
  reply->close();
}
