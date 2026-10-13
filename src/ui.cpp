// >>> tools menu window, replaces the local settings page
#include "agent.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <future>
#include <memory>

namespace shy {

void runOnUi(std::function<void()> fn)
{
	QMetaObject::invokeMethod(qApp, [fn]() { fn(); }, Qt::QueuedConnection);
}

// >>> in-process window list, no powershell like the node agent
static json scanProjectors()
{
	static const QRegularExpression re(QStringLiteral("multiview|projector|projektor"),
					   QRegularExpression::CaseInsensitiveOption);
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (!w->isVisible())
			continue;
		QString title = w->windowTitle();
		if (re.match(title).hasMatch())
			return {{"open", true}, {"title", title.toStdString()}};
	}
	return {{"open", false}, {"title", nullptr}};
}

json probeProjector()
{
	if (QThread::currentThread() == qApp->thread())
		return scanProjectors();
	// >>> not blocking-queued, obs exit would deadlock on it
	auto p = std::make_shared<std::promise<json>>();
	auto f = p->get_future();
	QMetaObject::invokeMethod(
		qApp,
		[p]() {
			try {
				p->set_value(scanProjectors());
			} catch (...) {
			}
		},
		Qt::QueuedConnection);
	if (f.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
		return {{"open", false}, {"title", nullptr}};
	return f.get();
}

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QString statusText()
{
	switch (relayState()) {
	case RelayState::Connected:
		return T("Status.Connected").arg(QString::fromStdString(relayChannel()));
	case RelayState::Connecting:
		return T("Status.Connecting");
	case RelayState::InvalidToken:
		return T("Status.InvalidToken");
	case RelayState::Replaced:
		return T("Status.Replaced");
	default:
		return T("Status.Off");
	}
}

static QPointer<QDialog> dialog;

static void openDialog()
{
	if (dialog) {
		dialog->show();
		dialog->raise();
		dialog->activateWindow();
		return;
	}
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *d = new QDialog(main);
	d->setAttribute(Qt::WA_DeleteOnClose);
	d->setWindowTitle(QStringLiteral("ShyBoti"));
	d->resize(560, 420);
	dialog = d;

	auto *token = new QLineEdit(d);
	token->setEchoMode(QLineEdit::Password);
	token->setPlaceholderText(T("Token.Placeholder"));
	token->setText(QString::fromStdString(loadConfig().token));
	auto *connectBtn = new QPushButton(T("Connect"), d);
	auto *disconnectBtn = new QPushButton(T("Disconnect"), d);
	auto *status = new QLabel(statusText(), d);
	auto *logView = new QPlainTextEdit(d);
	logView->setReadOnly(true);
	logView->setMaximumBlockCount(300);

	auto *row = new QHBoxLayout();
	row->addWidget(token, 1);
	row->addWidget(connectBtn);
	row->addWidget(disconnectBtn);
	auto *lay = new QVBoxLayout(d);
	lay->addWidget(new QLabel(T("Token"), d));
	lay->addLayout(row);
	lay->addWidget(status);
	lay->addWidget(logView, 1);

	QObject::connect(connectBtn, &QPushButton::clicked, d, [token]() {
		Config c = loadConfig();
		c.token = token->text().trimmed().toStdString();
		saveConfig(c);
		relayStart();
	});
	QObject::connect(disconnectBtn, &QPushButton::clicked, d, []() {
		relayStop();
		log("Disconnected.");
	});

	auto shown = std::make_shared<QString>();
	auto refresh = [status, logView, shown]() {
		status->setText(statusText());
		QString all;
		for (auto &l : recentLog())
			all += QString::fromStdString(l) + QLatin1Char('\n');
		if (all == *shown)
			return;
		*shown = all;
		logView->setPlainText(all);
		logView->verticalScrollBar()->setValue(logView->verticalScrollBar()->maximum());
	};
	refresh();
	auto *timer = new QTimer(d);
	QObject::connect(timer, &QTimer::timeout, d, refresh);
	timer->start(1000);
	d->show();
}

void uiInit()
{
	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("Menu")));
	QObject::connect(action, &QAction::triggered, [] { openDialog(); });
}

} // namespace shy
