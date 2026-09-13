# 工作台 UI 升级与交互优化（1.0.5 周期）

> 提交：`90f1da8`　完整 diff：`git show 90f1da8`　补丁文件：`release/ui-upgrade-90f1da8.patch`
> 范围：`src/gui/`（8 文件 / +939 −228）。**配色、主题令牌、既有功能一律未改**，只重排信息层级与交互路径。

## 0. 需求对照

| 需求 | 落点 | 关键文件 |
|---|---|---|
| 指标卡加趋势小图与环比标识、数值用等宽字体、拉开对比度 | `Sparkline` / `MicroStrip` 两个新控件 + `MetricTile` 扩展 | `src/gui/widgets.h` |
| 预算环形图占比过大、信息量少；事件流过高；重排栅格 | 预算卡紧凑化 + 总览栅格重分配 + 事件流封顶 | `src/gui/panels/dashboard_panel.cpp` |
| 图表 hover 与点击下钻；事件流类型过滤 | 图表点击回调 + 跨面板下钻通道 + 过滤下拉 | `widgets.h` / `panel_base.h` / `mainwindow.cpp` |
| 离线 Agent 操作入口、空状态、骨架屏 | `InlineEmpty` / `Skeleton` + Agent 卡片菜单 | `widgets.h` / `dashboard_panel.cpp` |
| 左侧导航「错误报告」重复 | 导航项改按稳定 id 定位 | `src/gui/mainwindow.cpp` |

## 1. 视觉层次

### 1.1 两个迷你图表控件（`widgets.h:119`、`widgets.h:178`）

```cpp
// 趋势小图：无坐标轴的迷你面积折线，只表达"走向"（无轴、无网格，不抢主数值）
class Sparkline : public QWidget {
public:
    void setSeries(const QVector<qint64>& series, const QColor& c);  // 面积渐变 + 末点强调
};

// 微条：一行分段条，表达"构成比例"（在线/离线、错误严重度构成）
class MicroStrip : public QWidget {
public:
    void setSegments(const QVector<QPair<QColor, int>>& segs);
    void setTooltipText(const QString& t);
};
```

### 1.2 指标卡扩展两个槽位（`widgets.h:1116`、`widgets.h:1124`）

连续型指标配折线、构成型指标配微条；同时数值改等宽字体：

```cpp
void setSparkline(const QVector<qint64>& series, const QColor& c);   // 卡片最小高 68 → 100
void setMicroStrip(const QVector<QPair<QColor, int>>& segs, const QString& tip);

// 构造函数里：
value_->setStyleSheet(th("color:@text@; font-family:'@mono@'; font-size:23px;"
                         " font-weight:700; background:transparent;"));
```

### 1.3 环比不只给数字，还给参照系（`widgets.h`）

```cpp
void setDelta(const QString& text, const QColor& c);      // 语义色胶囊（成本类：涨红跌绿）
void setDeltaTooltip(const QString& tip);                 // "最近 7 天 vs 前 7 天""今天 vs 昨天"
```

数据接线（`dashboard_panel.cpp`）：

| 指标卡 | 趋势小图 | 微条 | 环比 |
|---|---|---|---|
| 本周 Token | 14 天逐日序列 | — | 最近 7 天 vs 前 7 天 |
| Agent | — | 在线/离线 | 全部在线 / N 个离线 |
| 今日消耗 | — | — | 今天 vs 昨天 |
| 未解决错误 | — | critical/warning/note 构成 | 相对上次刷新（会话内增量） |

## 2. 布局重排

```
KPI 行：  [本周 Token 1] [Agent 1] [今日消耗 1] [未解决错误 1]
主行：    [预算卡 2] [各 Agent 用量 4] [逐日趋势 4]     ← 环形图不再独占大块
次行：    [Agent 状态 3] [模型分布 2]
底行：    [事件流 3] [告警 2]                          ← 事件流封顶 168px / 最多 8 条
```

预算卡：小环 + 右侧三行等宽数字 + 14 天迷你走势 + 调整入口（信息量↑、占地↓）

```cpp
ring_ = new ui::RingProgress(budgetCard_);
ring_->setMinimumSize(112, 112);
ring_->setMaximumWidth(132);
ring_->setValues(sum.total_tokens, sum.budget, QString());  // 环内不再重复"剩余"
```

事件流（它是"抬头看一眼"的窗口，不是清单）：

```cpp
timeline_->setMaximumHeight(168);
if (shown >= 8) break;   // 完整审计仍在「操作日志」面板
```

## 3. 交互

### 3.1 跨面板下钻通道（`panel_base.h:31`）

面板只登记意图，由主窗口落地——面板之间不互相持有引用，加面板不会牵动别的面板：

```cpp
using Navigator = std::function<void(const QString& panelId, const QString& key,
                                     const QString& value)>;
void setNavigator(Navigator n) { navigate_ = std::move(n); }
virtual void applyFilter(const QString& key, const QString& value) {}   // 目标面板接收筛选
protected:
void drillTo(const QString& panelId, const QString& key = {}, const QString& value = {}) const;
```

### 3.2 主窗口落地（`mainwindow.cpp:233`，注入见 `:272`）

```cpp
void MainWindow::goToPanel(const QString& panelId, const QString& filterKey,
                           const QString& filterValue) {
    const int row = navRowOf(panelId);
    if (row < 0) return;
    nav_->setCurrentRow(row);                                     // 切页触发 refresh
    if (!filterKey.isEmpty()) panels_[row]->applyFilter(filterKey, filterValue);
}
```

### 3.3 图表点击回调（`widgets.h:472`、`widgets.h:699`）

坐标换算与各自的 hover 保持一致，点击即"从看到某一行到看这一行的细节"：

```cpp
void setOnEntryClick(std::function<void(int)> fn);   // HBar：按 y 定位行
void setOnBarClick(std::function<void(int)> fn);     // VBar：按 x 定位柱
```

接线一览：

| 触发 | 目标 | 携带筛选 |
|---|---|---|
| 各 Agent 用量条 | 用量分析 | `agent=<名>` |
| 模型分布条 | 用量分析 | `model=<名>` |
| 逐日趋势柱 | 用量分析 | `range=14` |
| 告警卡 | 错误报告 | — |
| 事件流行双击 | 对应面板（按动作族） | — |
| 用量页模型条 / 表格行双击 | 本页筛选联动（页内下钻） | — |

### 3.4 事件流类型过滤

过滤与下钻共用同一张动作族映射，避免两处各写一套判断：

```cpp
static QString kindOfEvent(const QString& action, const QString& target);
// → memory | knowledge | skills | messages | errors | agents | audit
connect(eventFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this](int) { lastTimeline_.clear(); refresh(); });
```

## 4. 状态设计

```cpp
class Skeleton : public QWidget { void setRows(int); void start(); void stop(); };  // 微光扫过
class InlineEmpty : public QFrame { /* 图标 + 主文案 + 出路提示 */ };
```

- **骨架屏**：面板构造即 `skeleton_->start()`，首次刷新成功 `stop()`；`resizeEvent` 同步几何
- **空状态**：告警区 / 事件流 / 用量表 / Agent 网格；文案统一为"说明会出现什么 + 下一步做什么"
- **离线 Agent 操作入口**（`showAgentActions`）：复制接入命令 / 复制名称 / 查看该 Agent 用量 / 打开交流，卡片手型光标

## 5. 缺陷修复

### 5.1 左侧导航「错误报告」重复（根因：写死下标）

插入「用量分析」后 `nav_->item(5)` 已指向「Agent 交流」，于是那一行被改写成"错误报告"：

```diff
- if (auto* errItem = nav_->item(5)) {
+ const int errRow = navRowOf("errors");
+ if (auto* errItem = errRow >= 0 ? nav_->item(errRow) : nullptr) {
-     : (nav_->currentRow() == 5 ? ui::selText() : ui::muted())));
+     : (nav_->currentRow() == errRow ? ui::selText() : ui::muted())));
```

配套：导航项改为带稳定 id 的单一来源（`navItems_`），徽标与下钻都按 id 定位，Ctrl 快捷键数量随导航项自适应（`mainwindow.cpp:205 / :226`）。

### 5.2 截图核对时发现的三个显示缺陷

| 现象 | 根因 | 修法 |
|---|---|---|
| 预算卡「占比」显示成颜色值 | 富文本 `%8` 被颜色串占用，百分比 `.arg` 落空 | 参数顺序修正，百分比单独填 `%8` |
| 条形占比出现"18%"这类错值 | 用了带**入场动画进度**的条形比例 | 改为 该项 / 合计 |
| Y 轴 `1.1M` 与 `571k` 混排；0 值柱多画一个 `0` | 刻度逐值切换单位 | 按轴最大值统一单位；零刻度写 `0`；0 值柱不标注 |

## 6. 验证

| 项目 | 结果 |
|---|---|
| 单元测试 | 291 checks / 0 failures |
| 可行性集成断言 | 44 / 44 |
| MCP 集成断言 | 33 / 33 |
| 构建 | `cmake --build build --config Release` exit 0 |
| 视觉核对 | 中文/英文双语 + **有数据 / 空数据**两种状态逐项截图比对 |
| 下钻实测 | UI 自动化真实点击：点第 1 条 Agent 柱 → 导航切到 `Usage`，combo 读回 `Last 14 days \| hermes \| All models` |

## 7. 如何查看这次改动

```powershell
git show 90f1da8                       # 完整 diff
git show --stat 90f1da8                # 文件与规模
git apply --check release/ui-upgrade-90f1da8.patch   # 校验补丁可应用
```

样式相关：所有颜色仍取自 `theme.h` 既有令牌（`@brand@ @accent@ @ok@ @danger@ @warn@ @muted@ @card@ @line@ @fieldhover@`），换主题/换字号的行为不变。
