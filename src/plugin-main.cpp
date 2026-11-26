/*
ChromaMix for OBS
Färbt die Kanäle im OBS-Audiomixer pro Quelle ein.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "plugin-support.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

namespace {

/* ------------------------------------------------------------------ */
/* Einstellungen                                                       */
/* ------------------------------------------------------------------ */

struct Settings {
	QHash<QString, QColor> colors; // Quellen-UUID -> Farbe
	QHash<QString, QString> names; // UUID -> zuletzt bekannter Name (Fallback)
	bool tintName = false;
	bool tintStripe = true;
	bool tintColumn = true;
	int columnOpacity = 35; // Prozent
};

Settings g_settings;
QPointer<QTimer> g_timer;
QElapsedTimer g_lastMixerClick;
QString g_lastClickedName;

const char *kAppliedProp = "mixerColorsApplied";

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Übernimmt Farben aus der alten Version (Plugin hieß "obs-mixer-colors") */
void migrateOldConfig(const std::string &newPath)
{
	if (newPath.empty() || os_file_exists(newPath.c_str()))
		return;
	std::string oldPath = newPath;
	size_t pos = oldPath.rfind("chromamix");
	if (pos == std::string::npos)
		return;
	oldPath.replace(pos, strlen("chromamix"), "obs-mixer-colors");
	if (os_file_exists(oldPath.c_str()))
		os_copyfile(oldPath.c_str(), newPath.c_str());
}

std::string configFile()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("colors.json");
	std::string result = path ? path : "";
	bfree(path);
	migrateOldConfig(result);
	return result;
}

void loadSettings()
{
	g_settings = Settings();
	std::string path = configFile();
	if (path.empty())
		return;

	obs_data_t *data = obs_data_create_from_json_file(path.c_str());
	if (!data)
		return;

	obs_data_set_default_bool(data, "tint_stripe", true);
	g_settings.tintName = obs_data_get_bool(data, "tint_name");
	g_settings.tintStripe = obs_data_get_bool(data, "tint_stripe");
	obs_data_set_default_bool(data, "tint_column", true);
	obs_data_set_default_int(data, "column_opacity", 35);
	g_settings.tintColumn = obs_data_get_bool(data, "tint_column");
	g_settings.columnOpacity = (int)obs_data_get_int(data, "column_opacity");

	obs_data_array_t *arr = obs_data_get_array(data, "colors");
	size_t count = obs_data_array_count(arr);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		QString uuid = QString::fromUtf8(obs_data_get_string(item, "uuid"));
		QString name = QString::fromUtf8(obs_data_get_string(item, "name"));
		QColor color(QString::fromUtf8(obs_data_get_string(item, "color")));
		if (!uuid.isEmpty() && color.isValid()) {
			g_settings.colors.insert(uuid, color);
			g_settings.names.insert(uuid, name);
		}
		obs_data_release(item);
	}
	obs_data_array_release(arr);
	obs_data_release(data);
}

void saveSettings()
{
	std::string path = configFile();
	if (path.empty())
		return;

	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "tint_name", g_settings.tintName);
	obs_data_set_bool(data, "tint_stripe", g_settings.tintStripe);
	obs_data_set_bool(data, "tint_column", g_settings.tintColumn);
	obs_data_set_int(data, "column_opacity", g_settings.columnOpacity);

	obs_data_array_t *arr = obs_data_array_create();
	for (auto it = g_settings.colors.constBegin(); it != g_settings.colors.constEnd(); ++it) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "uuid", it.key().toUtf8().constData());
		obs_data_set_string(item, "name", g_settings.names.value(it.key()).toUtf8().constData());
		obs_data_set_string(item, "color", it.value().name(QColor::HexRgb).toUtf8().constData());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "colors", arr);
	obs_data_array_release(arr);

	obs_data_save_json_safe(data, path.c_str(), "tmp", "bak");
	obs_data_release(data);
}

/* ------------------------------------------------------------------ */
/* Quellen                                                             */
/* ------------------------------------------------------------------ */

struct SourceInfo {
	QString uuid;
	QString name;
};

std::vector<SourceInfo> audioSources()
{
	std::vector<SourceInfo> list;
	std::set<std::string> seen;

	auto add = [&](obs_source_t *src) {
		if (!src)
			return;
		if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0)
			return;
		const char *uuid = obs_source_get_uuid(src);
		const char *name = obs_source_get_name(src);
		if (!uuid || !name || !seen.insert(uuid).second)
			return;
		list.push_back({QString::fromUtf8(uuid), QString::fromUtf8(name)});
	};

	// Globale Audiogeräte (Einstellungen -> Audio)
	for (uint32_t ch = 0; ch < MAX_CHANNELS; ch++) {
		obs_source_t *src = obs_get_output_source(ch);
		add(src);
		obs_source_release(src);
	}

	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			(*static_cast<decltype(add) *>(param))(src);
			return true;
		},
		&add);

	std::sort(list.begin(), list.end(), [](const SourceInfo &a, const SourceInfo &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return list;
}

QString uuidForName(const QString &name)
{
	for (const SourceInfo &info : audioSources()) {
		if (info.name == name)
			return info.uuid;
	}
	return QString();
}

QString currentNameForUuid(const QString &uuid)
{
	obs_source_t *src = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (src) {
		QString name = QString::fromUtf8(obs_source_get_name(src));
		obs_source_release(src);
		return name;
	}
	// Globale Geräte sind evtl. nicht per UUID auffindbar
	for (const SourceInfo &info : audioSources()) {
		if (info.uuid == uuid)
			return info.name;
	}
	return QString();
}

/* ------------------------------------------------------------------ */
/* Mixer einfärben                                                     */
/* ------------------------------------------------------------------ */

bool isClass(const QObject *obj, const char *className)
{
	return obj && qstrcmp(obj->metaObject()->className(), className) == 0;
}

QWidget *findVolumeControl(QObject *obj)
{
	while (obj) {
		if (isClass(obj, "VolumeControl"))
			return static_cast<QWidget *>(obj);
		obj = obj->parent();
	}
	return nullptr;
}

QString volumeControlName(QWidget *vc)
{
	for (QAbstractButton *btn : vc->findChildren<QAbstractButton *>()) {
		if (isClass(btn, "VolumeName"))
			return btn->text();
	}
	return vc->objectName();
}

QColor contrastText(const QColor &bg)
{
	double l = 0.2126 * bg.redF() + 0.7152 * bg.greenF() + 0.0722 * bg.blueF();
	return l > 0.55 ? QColor(20, 20, 24) : QColor(255, 255, 255);
}

QDockWidget *mixerDock()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main)
		return nullptr;
	return main->findChild<QDockWidget *>("mixerDock");
}

void styleControl(QWidget *vc, const QColor &color)
{
	QString key =
		color.isValid()
			? color.name() + (g_settings.tintName ? "n" : "") + (g_settings.tintStripe ? "s" : "") +
				  (g_settings.tintColumn ? QString("c%1").arg(g_settings.columnOpacity) : QString())
			: QString();
	if (vc->property(kAppliedProp).toString() == key)
		return;
	vc->setProperty(kAppliedProp, key);

	QString hex = color.name();
	QString text = contrastText(color).name();

	// Ganze Spalte einfärben (halbtransparent, damit der Pegel lesbar bleibt)
	if (color.isValid() && g_settings.tintColumn) {
		QColor bg = color;
		bg.setAlpha(qBound(0, g_settings.columnOpacity, 100) * 255 / 100);
		vc->setStyleSheet(QString("VolumeControl { background: rgba(%1, %2, %3, %4); border: 1px solid %5; "
					  "border-radius: 4px; }")
					  .arg(bg.red())
					  .arg(bg.green())
					  .arg(bg.blue())
					  .arg(bg.alpha())
					  .arg(hex));
	} else {
		vc->setStyleSheet(QString());
	}

	for (QLabel *label : vc->findChildren<QLabel *>()) {
		const QString cls = label->property("class").toString();

		if (cls.contains("mixer-category")) {
			label->setStyleSheet(color.isValid()
						     ? QString("QLabel { background: %1; color: %2; }").arg(hex, text)
						     : QString());
			continue;
		}

		// Text im Namens-Button (VolumeName)
		if (isClass(label->parentWidget(), "VolumeName")) {
			label->setStyleSheet(color.isValid() && g_settings.tintName
						     ? QString("QLabel { color: %1; font-weight: bold; }").arg(hex)
						     : QString());
		}
	}

	// Farbiger Streifen um den Pegel-Bereich
	for (QFrame *frame : vc->findChildren<QFrame *>()) {
		if (frame->objectName() == "volMeterFrame") {
			frame->setStyleSheet(
				color.isValid() && g_settings.tintStripe
					? QString("QFrame#volMeterFrame { border-bottom: 3px solid %1; }").arg(hex)
					: QString());
		}
	}
}

void applyColors()
{
	QDockWidget *dock = mixerDock();
	if (!dock)
		return;

	// Name -> Farbe (aktuelle Namen, damit Umbenennen funktioniert)
	QHash<QString, QColor> byName;
	for (auto it = g_settings.colors.constBegin(); it != g_settings.colors.constEnd(); ++it) {
		QString name = currentNameForUuid(it.key());
		if (name.isEmpty())
			name = g_settings.names.value(it.key());
		else
			g_settings.names[it.key()] = name;
		if (!name.isEmpty())
			byName.insert(name, it.value());
	}

	for (QFrame *frame : dock->findChildren<QFrame *>()) {
		if (!isClass(frame, "VolumeControl"))
			continue;
		styleControl(frame, byName.value(volumeControlName(frame)));
	}
}

void forceReapply()
{
	QDockWidget *dock = mixerDock();
	if (dock) {
		for (QFrame *frame : dock->findChildren<QFrame *>())
			if (isClass(frame, "VolumeControl"))
				frame->setProperty(kAppliedProp, QVariant());
	}
	applyColors();
}

/* ------------------------------------------------------------------ */
/* Farbwahl                                                            */
/* ------------------------------------------------------------------ */

const QList<QColor> &presetColors()
{
	static const QList<QColor> colors = {
		QColor("#e5484d"), QColor("#f76b15"), QColor("#ffc53d"), QColor("#46a758"),
		QColor("#12a594"), QColor("#0090ff"), QColor("#3e63dd"), QColor("#8e4ec6"),
		QColor("#d6409f"), QColor("#1db954"), QColor("#5865f2"), QColor("#8b8d98"),
	};
	return colors;
}

QIcon swatch(const QColor &color)
{
	QPixmap pm(16, 16);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	if (color.isValid()) {
		p.setBrush(color);
		p.setPen(color.darker(140));
	} else {
		p.setBrush(Qt::NoBrush);
		p.setPen(QColor(128, 128, 128));
	}
	p.drawRoundedRect(QRectF(1, 1, 14, 14), 3, 3);
	return QIcon(pm);
}

void setColor(const QString &uuid, const QString &name, const QColor &color)
{
	if (uuid.isEmpty())
		return;
	if (color.isValid()) {
		g_settings.colors.insert(uuid, color);
		g_settings.names.insert(uuid, name);
	} else {
		g_settings.colors.remove(uuid);
		g_settings.names.remove(uuid);
	}
	saveSettings();
	forceReapply();
}

QColor pickColor(const QColor &initial, QWidget *parent, const QString &name)
{
	QColorDialog dlg(initial.isValid() ? initial : presetColors().at(5), parent);
	dlg.setWindowTitle(T("PickTitle").arg(name));
	for (int i = 0; i < presetColors().size() && i < QColorDialog::customCount(); i++)
		QColorDialog::setCustomColor(i, presetColors().at(i));
	if (dlg.exec() != QDialog::Accepted)
		return QColor();
	return dlg.selectedColor();
}

/* Untermenü für das Rechtsklick-Menü eines Mixer-Kanals */
void addColorMenu(QMenu *menu, const QString &name)
{
	if (menu->property("mixerColorsAdded").toBool())
		return;
	menu->setProperty("mixerColorsAdded", true);

	QString uuid = uuidForName(name);
	if (uuid.isEmpty())
		return;

	QMenu *sub = new QMenu(T("ColorMenu"), menu);
	QColor current = g_settings.colors.value(uuid);
	sub->setIcon(swatch(current));

	for (const QColor &c : presetColors()) {
		QAction *a = sub->addAction(swatch(c), c.name().toUpper());
		QObject::connect(a, &QAction::triggered, [uuid, name, c]() { setColor(uuid, name, c); });
	}
	sub->addSeparator();
	QAction *custom = sub->addAction(T("CustomColor"));
	QObject::connect(custom, &QAction::triggered, [uuid, name, current]() {
		QColor c = pickColor(current, static_cast<QWidget *>(obs_frontend_get_main_window()), name);
		if (c.isValid())
			setColor(uuid, name, c);
	});
	QAction *reset = sub->addAction(T("RemoveColor"));
	reset->setEnabled(current.isValid());
	QObject::connect(reset, &QAction::triggered, [uuid, name]() { setColor(uuid, name, QColor()); });

	menu->addSeparator();
	menu->addMenu(sub);
	menu->adjustSize();
}

/* Merkt sich Klicks auf Mixer-Kanäle und erweitert das danach geöffnete Menü */
class MenuWatcher : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		switch (ev->type()) {
		case QEvent::MouseButtonPress:
		case QEvent::ContextMenu:
			if (QWidget *vc = findVolumeControl(obj)) {
				g_lastClickedName = volumeControlName(vc);
				g_lastMixerClick.restart();
			}
			break;
		case QEvent::Show:
			if (auto *menu = qobject_cast<QMenu *>(obj)) {
				if (g_lastMixerClick.isValid() && g_lastMixerClick.elapsed() < 1500 &&
				    !g_lastClickedName.isEmpty() && isMixerMenu(menu)) {
					g_lastMixerClick.invalidate();
					addColorMenu(menu, g_lastClickedName);
				}
			}
			break;
		default:
			break;
		}
		return false;
	}

private:
	static bool isMixerMenu(QMenu *menu)
	{
		// Das Kanal-Menü enthält Aktionen mit einer "source"-Eigenschaft
		// (Filter, Eigenschaften, Umbenennen ...).
		for (QAction *a : menu->actions()) {
			if (a->property("source").isValid())
				return true;
		}
		return false;
	}
};

QPointer<MenuWatcher> g_watcher;

/* ------------------------------------------------------------------ */
/* Einstellungsfenster (Tools -> ChromaMix)                             */
/* ------------------------------------------------------------------ */

void openSettingsDialog()
{
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());

	QDialog dlg(main);
	dlg.setWindowTitle("ChromaMix");
	dlg.resize(380, 440);

	auto *layout = new QVBoxLayout(&dlg);
	auto *hint = new QLabel(T("DialogHint"));
	hint->setWordWrap(true);
	layout->addWidget(hint);

	auto *list = new QListWidget(&dlg);
	list->setIconSize(QSize(16, 16));
	layout->addWidget(list, 1);

	auto refill = [list]() {
		QString selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
		list->clear();
		for (const SourceInfo &info : audioSources()) {
			auto *item = new QListWidgetItem(swatch(g_settings.colors.value(info.uuid)), info.name);
			item->setData(Qt::UserRole, info.uuid);
			list->addItem(item);
			if (info.uuid == selected)
				list->setCurrentItem(item);
		}
	};
	refill();

	auto *btnRow = new QHBoxLayout();
	auto *pickBtn = new QPushButton(T("PickButton"));
	auto *clearBtn = new QPushButton(T("ClearButton"));
	btnRow->addWidget(pickBtn);
	btnRow->addWidget(clearBtn);
	btnRow->addStretch();
	layout->addLayout(btnRow);

	auto *stripeBox = new QCheckBox(T("StripeOption"));
	stripeBox->setChecked(g_settings.tintStripe);
	layout->addWidget(stripeBox);

	auto *columnBox = new QCheckBox(T("ColumnOption"));
	columnBox->setChecked(g_settings.tintColumn);
	layout->addWidget(columnBox);

	auto *opacityRow = new QHBoxLayout();
	auto *opacityLabel = new QLabel(T("Intensity").arg(g_settings.columnOpacity));
	auto *opacitySlider = new QSlider(Qt::Horizontal);
	opacitySlider->setRange(5, 100);
	opacitySlider->setValue(g_settings.columnOpacity);
	opacitySlider->setEnabled(g_settings.tintColumn);
	opacityRow->addWidget(opacityLabel);
	opacityRow->addWidget(opacitySlider, 1);
	layout->addLayout(opacityRow);

	auto *nameBox = new QCheckBox(T("NameOption"));
	nameBox->setChecked(g_settings.tintName);
	layout->addWidget(nameBox);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
	layout->addWidget(buttons);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	auto pick = [&dlg, list, refill]() {
		QListWidgetItem *item = list->currentItem();
		if (!item)
			return;
		QString uuid = item->data(Qt::UserRole).toString();
		QColor c = pickColor(g_settings.colors.value(uuid), &dlg, item->text());
		if (c.isValid()) {
			setColor(uuid, item->text(), c);
			refill();
		}
	};
	QObject::connect(pickBtn, &QPushButton::clicked, pick);
	QObject::connect(list, &QListWidget::itemDoubleClicked, pick);
	QObject::connect(clearBtn, &QPushButton::clicked, [list, refill]() {
		QListWidgetItem *item = list->currentItem();
		if (!item)
			return;
		setColor(item->data(Qt::UserRole).toString(), item->text(), QColor());
		refill();
	});
	QObject::connect(stripeBox, &QCheckBox::toggled, [](bool on) {
		g_settings.tintStripe = on;
		saveSettings();
		forceReapply();
	});
	QObject::connect(columnBox, &QCheckBox::toggled, [opacitySlider](bool on) {
		g_settings.tintColumn = on;
		opacitySlider->setEnabled(on);
		saveSettings();
		forceReapply();
	});
	QObject::connect(opacitySlider, &QSlider::valueChanged, [opacityLabel](int v) {
		g_settings.columnOpacity = v;
		opacityLabel->setText(T("Intensity").arg(v));
		saveSettings();
		forceReapply();
	});
	QObject::connect(nameBox, &QCheckBox::toggled, [](bool on) {
		g_settings.tintName = on;
		saveSettings();
		forceReapply();
	});

	dlg.exec();
}

/* ------------------------------------------------------------------ */
/* Lebenszyklus                                                        */
/* ------------------------------------------------------------------ */

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		if (!g_watcher) {
			g_watcher = new MenuWatcher(qApp);
			qApp->installEventFilter(g_watcher);
		}
		if (!g_timer) {
			// Der Mixer baut seine Kanäle bei vielen Ereignissen neu auf;
			// ein leichter Timer stellt sicher, dass die Farben immer drauf sind.
			g_timer = new QTimer(qApp);
			g_timer->setInterval(500);
			QObject::connect(g_timer, &QTimer::timeout, []() { applyColors(); });
			g_timer->start();
		}
		forceReapply();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (g_timer) {
			g_timer->stop();
			delete g_timer;
		}
		if (g_watcher) {
			qApp->removeEventFilter(g_watcher);
			delete g_watcher;
		}
	}
}

} // namespace

bool obs_module_load(void)
{
	loadSettings();

	QAction *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("ChromaMix"));
	QObject::connect(action, &QAction::triggered, []() { openSettingsDialog(); });

	obs_frontend_add_event_callback(onFrontendEvent, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
