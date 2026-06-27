
#include "obs-module.h"
#include <QDialog>
#include <QThread>
#include <QTreeView>
#include <QSortFilterProxyModel>
#include <QSet>
#include <QMap>
#include <util/source-profiler.h>
#include <obs-frontend-api.h>
#include <atomic>
#include <mutex>

class PerfTreeItem;

/* Broad, stable (locale-independent) categorisation of a tree node, used to
 * decide what gets broadcast over the websocket. */
enum class SourceCategory { None, Scene, Group, Source, Filter, Transition };

/* Category bitmask flags for the websocket "what to send" filter. */
enum WSCategoryFlag {
	WS_CAT_SCENE = 1 << 0,
	WS_CAT_GROUP = 1 << 1,
	WS_CAT_SOURCE = 1 << 2,
	WS_CAT_FILTER = 1 << 3,
	WS_CAT_TRANSITION = 1 << 4,
	WS_CAT_ALL = WS_CAT_SCENE | WS_CAT_GROUP | WS_CAT_SOURCE | WS_CAT_FILTER | WS_CAT_TRANSITION,
};

/* Immutable snapshot of the broadcast filter, built once per emit so the hot
 * path never locks per-node. */
struct WSFilter {
	unsigned categories = WS_CAT_ALL;
	bool kindFilter = false; /* when true, only kinds in `kinds` are sent */
	QSet<QString> kinds;     /* unversioned source ids, e.g. "browser_source" */
};

enum PerfTreeColumnType {
	COLUMN_TYPE_DEFAULT,
	COLUMN_TYPE_BOOL,
	COLUMN_TYPE_DURATION,
	COLUMN_TYPE_INTERVAL,
	COLUMN_TYPE_PERCENTAGE,
	COLUMN_TYPE_FPS,
	COLUMN_TYPE_COUNT,
	COLUMN_TYPE_GRAPH,
};

class PerfTreeColumn {
	QVariant (*m_get_value)(const PerfTreeItem *item);
	QString m_name;
	bool m_default_hidden;

public:
	PerfTreeColumn(QString name, QVariant (*getValue)(const PerfTreeItem *item),
		       enum PerfTreeColumnType column_type = COLUMN_TYPE_DEFAULT, bool default_hidden = false);
	QString Name() { return m_name; }
	QVariant Value(const PerfTreeItem *item) { return m_get_value(item); }
	bool DefaultHidden() { return m_default_hidden; }

private:
	enum PerfTreeColumnType m_column_type;

	friend class PerfTreeModel;
};

class PerfTreeModel;
class PerfViewerProxyModel;

class OBSPerfViewer : public QDialog {
	Q_OBJECT

	PerfTreeModel *model = nullptr;
	PerfViewerProxyModel *proxy = nullptr;

	QTreeView *treeView = nullptr;

	bool loaded = false;

public:
	OBSPerfViewer(QWidget *parent = nullptr);
	~OBSPerfViewer() override;

public slots:
	void sourceListUpdated();
};

class PerfTreeItem;

class PerfTreeModel : public QAbstractItemModel {
	Q_OBJECT

public:
	explicit PerfTreeModel(QObject *parent = nullptr);
	~PerfTreeModel() override;

	QVariant data(const QModelIndex &index, int role) const override;
	Qt::ItemFlags flags(const QModelIndex &index) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
	QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
	QModelIndex parent(const QModelIndex &index) const override;
	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	int columnCount(const QModelIndex &parent = QModelIndex()) const override;
	enum PerfTreeColumnType columnType(int column) const { return columns.at(column).m_column_type; }

	void itemChanged(PerfTreeItem *item);

	enum ShowMode { SCENE, SCENE_NESTED, SOURCE, FILTER, TRANSITION, ALL };

	void setShowMode(enum ShowMode s = ShowMode::SCENE)
	{
		showMode = s;
		refreshSources();
	}
	enum ShowMode getShowMode() { return showMode; }
	void setActiveOnly(bool a, bool refresh = true)
	{
		activeOnly = a;
		if (refresh)
			refreshSources();
	}
	bool getActiveOnly() { return activeOnly; }

	void setRefreshInterval(int interval);

	double targetFrameTime() const { return frameTime; }

	/* ---- WebSocket broadcast settings (read on the updater thread, set from UI) ---- */
	void setWsEnabled(bool e) { wsEnabled = e; }
	bool getWsEnabled() const { return wsEnabled; }
	void setWsInterval(int ms) { wsBroadcastInterval = (unsigned)(ms < 1 ? 1 : ms); }
	int getWsInterval() const { return (int)wsBroadcastInterval.load(); }
	void setWsCategories(unsigned c) { wsCategories = c; }
	unsigned getWsCategories() const { return wsCategories.load(); }
	void setWsKindFilter(bool on, const QSet<QString> &kinds)
	{
		std::lock_guard<std::mutex> lk(wsKindMutex);
		wsKindFilter = on;
		wsKinds = kinds;
	}
	bool getWsKindFilter() const
	{
		std::lock_guard<std::mutex> lk(wsKindMutex);
		return wsKindFilter;
	}
	QSet<QString> getWsKinds() const
	{
		std::lock_guard<std::mutex> lk(wsKindMutex);
		return wsKinds;
	}
	/* Register an input kind (id -> human display name) seen while building the
	 * tree, so the UI can offer a checklist of the kinds actually present. */
	void registerKind(const QString &id, const QString &display)
	{
		if (id.isEmpty())
			return;
		std::lock_guard<std::mutex> lk(wsKindMutex);
		knownKinds.insert(id, display);
	}
	QMap<QString, QString> availableKinds() const
	{
		std::lock_guard<std::mutex> lk(wsKindMutex);
		return knownKinds;
	}

	QList<int> getDefaultHiddenColumns();
	void setGraphWidthFunc(std::function<int()> func) { graphWidthFunc = func; }

public slots:
	void refreshSources();

private slots:
	void updateData();

private:
	PerfTreeItem *rootItem = nullptr;
	QList<PerfTreeColumn> columns;
	std::unique_ptr<QThread> updater;
	bool updaterRunning;
	std::function<int()> graphWidthFunc = nullptr;

	enum ShowMode showMode = ShowMode::SCENE;
	bool activeOnly = true;
	bool refreshing = false;
	double frameTime = 0.0;
	unsigned int refreshInterval = 1000;

	/* WebSocket broadcast state */
	std::atomic<bool> wsEnabled{false};
	std::atomic<unsigned> wsBroadcastInterval{1000};
	std::atomic<unsigned> wsCategories{WS_CAT_ALL};
	bool wsKindFilter = false;
	QSet<QString> wsKinds;
	QMap<QString, QString> knownKinds;
	mutable std::mutex wsKindMutex;
	uint64_t lastBroadcastNs = 0;

	static bool EnumAll(void *data, obs_source_t *source);
	static bool EnumNotPrivateSource(void *data, obs_source_t *source);
	static bool EnumScene(void *data, obs_source_t *source);
	static bool EnumSceneNested(void *data, obs_source_t *source);
	static bool EnumFilterSource(void *data, obs_source_t *source);
	static bool EnumTransition(void *data, obs_source_t *source);
	static bool EnumAllSource(void *data, obs_source_t *source);
	static bool EnumSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *data);
	static void EnumFilter(obs_source_t *, obs_source_t *child, void *data);
	static void EnumTree(obs_source_t *, obs_source_t *child, void *data);
	static bool ExistsChild(PerfTreeItem *parent, obs_source_t *source);
	static void source_add(void *data, calldata_t *cd);
	static void source_remove(void *data, calldata_t *cd);
	static void source_activate(void *data, calldata_t *cd);
	static void source_deactivate(void *data, calldata_t *cd);
	static void frontend_event(obs_frontend_event event, void *private_data);

	void add_filter(obs_source_t *source, obs_source_t *filter, const QModelIndex &parent = QModelIndex());
	void remove_source(obs_source_t *source, const QModelIndex &parent = QModelIndex());
	void remove_weak_source(obs_weak_source_t *source, const QModelIndex &parent = QModelIndex());
	void add_sceneitem(obs_source_t *scene, obs_sceneitem_t *item, const QModelIndex &parent = QModelIndex());
	void remove_sceneitem(obs_source_t *scene, obs_sceneitem_t *item, const QModelIndex &parent = QModelIndex());

	void remove_siblings(const QModelIndex &parent = QModelIndex());

	friend class PerfTreeItem;
};

class PerfTreeItem {
public:
	explicit PerfTreeItem(obs_sceneitem_t *sceneitem, PerfTreeItem *parentItem = nullptr, PerfTreeModel *model = nullptr);
	explicit PerfTreeItem(obs_source_t *source, PerfTreeItem *parentItem = nullptr, PerfTreeModel *model = nullptr);
	~PerfTreeItem();
	void disconnect();

	void appendChild(PerfTreeItem *item);
	void prependChild(PerfTreeItem *item);

	PerfTreeItem *child(int row) const;
	int childCount() const;
	int columnCount() const;
	int row() const;
	PerfTreeItem *parentItem();

	PerfTreeModel *model() const { return m_model; }

	void update();
	QIcon getIcon(obs_source_t *source) const;
	obs_source_t *getSource() const { return obs_weak_source_get_source(m_source); }

	/* Serialize this item's own stats (no children) into an obs_data object for
	 * the obs-websocket broadcast. Caller owns the returned ref. */
	obs_data_t *toDataSelf() const;
	/* Whether this node should be broadcast given the active filter. */
	bool passesFilter(const WSFilter &f) const;
	/* Recursively append this item's children that pass `f` into `out`,
	 * bubbling included descendants up through excluded levels. */
	void collectBroadcast(const WSFilter &f, obs_data_array_t *out) const;

private:
	QList<PerfTreeItem *> m_childItems;
	PerfTreeItem *m_parentItem;
	PerfTreeModel *m_model;

	profiler_result_t *m_perf = nullptr;
	obs_weak_source_t *m_source = nullptr;
	obs_sceneitem_t *m_sceneitem = nullptr;
	QString name;
	QString sourceDisplayName;
	QString sourceType;
	SourceCategory category = SourceCategory::None;
	QString kindId; /* unversioned source id for inputs, e.g. "browser_source" */
	bool async = false;
	bool rendered = false;
	bool active = false;
	bool enabled = false;
	bool is_private = false;
	bool is_filter = false;
	int child_count = 0;
	QIcon icon;
	QImage graph;
	int prev_graph_value = 0;

	static void filter_add(void *, calldata_t *);
	static void filter_remove(void *, calldata_t *);
	static void sceneitem_add(void *, calldata_t *);
	static void sceneitem_remove(void *, calldata_t *);
	static void sceneitem_visible(void *, calldata_t *);

	friend class PerfTreeModel;
};

class PerfViewerProxyModel : public QSortFilterProxyModel {
	Q_OBJECT

public:
	PerfViewerProxyModel(QObject *parent = nullptr) : QSortFilterProxyModel(parent)
	{
		setRecursiveFilteringEnabled(true);
		setSortRole(Qt::UserRole);
	}

public slots:
	void setFilterText(const QString &filter);

protected:
	bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
};

class QuickThread : public QThread {
public:
	explicit inline QuickThread(std::function<void()> func_) : func(func_) {}

private:
	virtual void run() override { func(); }

	std::function<void()> func;
};
