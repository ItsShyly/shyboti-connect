// >>> tools menu window
#include "agent.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDialog>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSignalBlocker>
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

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <vector>

namespace shy {

void runOnUi(std::function<void()> fn)
{
	QMetaObject::invokeMethod(qApp, [fn]() { fn(); }, Qt::QueuedConnection);
}

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

// vvv shared state for dock, menu and settings window vvv
static const char *DOCK_ID = "shyboti-dock";
static bool autoConnect = true; // <<< cached, refresh runs twice a second

static QColor stateColor()
{
	switch (relayState()) {
	case RelayState::Connected:
		return QColor(0x3f, 0xb9, 0x50);
	case RelayState::Connecting:
		return QColor(0xd2, 0x99, 0x22);
	case RelayState::InvalidToken:
	case RelayState::Replaced:
		return QColor(0xf8, 0x51, 0x49);
	default:
		return QColor(0x8b, 0x94, 0x9e);
	}
}

static bool isOnline()
{
	RelayState s = relayState();
	return s == RelayState::Connected || s == RelayState::Connecting;
}

static void toggleConnection(const QString &token = QString())
{
	if (isOnline()) {
		relayStop();
		log("Disconnected.");
		return;
	}
	if (!token.isNull()) {
		Config c = loadConfig();
		c.token = token.trimmed().toStdString();
		saveConfig(c);
	}
	relayStart();
}

static void setAutoConnect(bool on)
{
	if (autoConnect == on)
		return;
	autoConnect = on;
	Config c = loadConfig();
	c.autoConnect = on;
	saveConfig(c);
	log(on ? "Auto connect on." : "Auto connect off.");
}

static QIcon dotIcon(const QColor &color)
{
	QPixmap pm(12, 12);
	pm.fill(Qt::transparent);
	{
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(1, 1, 10, 10);
	}
	return QIcon(pm);
}

struct Controls {
	QPointer<QLabel> dot;
	QPointer<QLabel> text;
	QPointer<QPushButton> button;
	QPointer<QCheckBox> autoBox;
};
static std::vector<Controls> controls;
static QPointer<QTimer> refreshTimer;

static void refreshControls()
{
	QString status = statusText();
	QString dotCss = QStringLiteral("background:%1;border-radius:5px;").arg(stateColor().name());
	QString label = isOnline() ? T("Disconnect") : T("Connect");
	controls.erase(std::remove_if(controls.begin(), controls.end(), [](const Controls &c) { return !c.text; }),
		       controls.end());
	for (auto &c : controls) {
		c.dot->setStyleSheet(dotCss);
		c.text->setText(status);
		c.button->setText(label);
		QSignalBlocker block(c.autoBox);
		c.autoBox->setChecked(autoConnect);
	}
}

// >>> status row + connect button + auto start box, same in dock and window
static void addControls(QWidget *parent, QVBoxLayout *lay, std::function<QString()> token)
{
	auto *dot = new QLabel(parent);
	dot->setFixedSize(10, 10);
	auto *text = new QLabel(parent);
	text->setWordWrap(true);
	auto *statusRow = new QHBoxLayout();
	statusRow->addWidget(dot, 0, Qt::AlignVCenter);
	statusRow->addWidget(text, 1);

	auto *button = new QPushButton(parent);
	auto *autoBox = new QCheckBox(T("AutoConnect"), parent);
	QObject::connect(button, &QPushButton::clicked, parent, [token]() {
		toggleConnection(token ? token() : QString());
		refreshControls();
	});
	QObject::connect(autoBox, &QCheckBox::toggled, parent, [](bool on) { setAutoConnect(on); });

	lay->addLayout(statusRow);
	lay->addWidget(button);
	lay->addWidget(autoBox);
	controls.push_back({dot, text, button, autoBox});
	refreshControls();
}

// vvv settings window vvv
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
	d->resize(560, 440);
	dialog = d;

	auto *token = new QLineEdit(d);
	token->setEchoMode(QLineEdit::Password);
	token->setPlaceholderText(T("Token.Placeholder"));
	token->setText(QString::fromStdString(loadConfig().token));
	auto *logView = new QPlainTextEdit(d);
	logView->setReadOnly(true);
	logView->setMaximumBlockCount(300);

	auto *lay = new QVBoxLayout(d);
	lay->addWidget(new QLabel(T("Token"), d));
	lay->addWidget(token);
	addControls(d, lay, [token]() { return token->text(); });
	lay->addWidget(logView, 1);

	auto shown = std::make_shared<QString>();
	auto refreshLog = [logView, shown]() {
		QString all;
		for (auto &l : recentLog())
			all += QString::fromStdString(l) + QLatin1Char('\n');
		if (all == *shown)
			return;
		*shown = all;
		logView->setPlainText(all);
		logView->verticalScrollBar()->setValue(logView->verticalScrollBar()->maximum());
	};
	refreshLog();
	auto *timer = new QTimer(d);
	QObject::connect(timer, &QTimer::timeout, d, refreshLog);
	timer->start(1000);
	d->show();
}

// vvv dock vvv
static void addDock()
{
	auto *w = new QWidget();
	w->setMinimumWidth(180);
	auto *lay = new QVBoxLayout(w);
	addControls(w, lay, nullptr);
	lay->addStretch(1);
	if (!obs_frontend_add_dock_by_id(DOCK_ID, obs_module_text("Dock"), w))
		log("Could not add the ShyBoti dock.");
}

// vvv tools menu vvv
static void addMenu()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("Menu")));
	auto *menu = new QMenu(main);
	auto *status = menu->addAction(QString());
	status->setEnabled(false);
	menu->addSeparator();
	auto *toggle = menu->addAction(QString());
	auto *autoAct = menu->addAction(T("AutoConnect"));
	autoAct->setCheckable(true);
	menu->addSeparator();
	auto *settings = menu->addAction(T("Settings"));
	action->setMenu(menu);

	QObject::connect(menu, &QMenu::aboutToShow, menu, [status, toggle, autoAct]() {
		status->setIcon(dotIcon(stateColor()));
		status->setText(statusText());
		toggle->setText(isOnline() ? T("Disconnect") : T("Connect"));
		QSignalBlocker block(autoAct);
		autoAct->setChecked(autoConnect);
	});
	QObject::connect(toggle, &QAction::triggered, menu, []() {
		toggleConnection();
		refreshControls();
	});
	QObject::connect(autoAct, &QAction::toggled, menu, [](bool on) {
		setAutoConnect(on);
		refreshControls();
	});
	QObject::connect(settings, &QAction::triggered, menu, []() { openDialog(); });
}

void uiInit()
{
	autoConnect = loadConfig().autoConnect;
	addMenu();
	addDock();
	refreshTimer = new QTimer(qApp);
	QObject::connect(refreshTimer, &QTimer::timeout, refreshTimer, []() { refreshControls(); });
	refreshTimer->start(500);
}

// >>> once after install, later the user's choice wins
void uiLoaded()
{
	Config c = loadConfig();
	if (c.dockShown)
		return;
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dock = main ? main->findChild<QDockWidget *>(QString::fromUtf8(DOCK_ID)) : nullptr;
	if (!dock)
		return;
	main->addDockWidget(Qt::RightDockWidgetArea, dock);
	dock->setFloating(false);
	dock->show();
	c.dockShown = true;
	saveConfig(c);
}

// >>> timer lambdas live in this dll, stop before it unloads
void uiShutdown()
{
	delete refreshTimer;
	controls.clear();
}

} // namespace shy
