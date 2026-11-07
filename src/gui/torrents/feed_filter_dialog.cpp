/**
 * Taiga
 * Copyright (C) 2010-2026, Eren Okka
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "feed_filter_dialog.hpp"

#include <QApplication>
#include <QCollator>
#include <QComboBox>
#include <QDate>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <map>
#include <ranges>
#include <tuple>

#include "base/string.hpp"
#include "gui/utils/theme.hpp"
#include "media/anime.hpp"
#include "media/anime_db.hpp"
#include "media/anime_list.hpp"
#include "media/anime_list_utils.hpp"
#include "media/anime_utils.hpp"
#include "track/feed.hpp"

namespace gui {

using namespace track::feed;

namespace {

constexpr int kPresetRole = Qt::UserRole;
constexpr int kDescriptionRole = Qt::UserRole + 1;
constexpr int kAnimeIdRole = Qt::UserRole;

////////////////////////////////////////////////////////////////////////////////
// Icons

QIcon composeIcon(const QIcon& base, const std::function<void(QPainter&, const QRectF&)>& badge) {
  QIcon icon;
  for (const int size : {16, 20, 24, 32, 48}) {
    auto pixmap = base.pixmap(QSize{size, size});
    QPainter painter{&pixmap};
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF rect{size * 0.42, size * 0.42, size * 0.58, size * 0.58};
    badge(painter, rect);
    painter.end();
    icon.addPixmap(pixmap);
  }
  return icon;
}

void drawBadgeCircle(QPainter& painter, const QRectF& rect, const QColor& color) {
  painter.setPen(Qt::NoPen);
  painter.setBrush(color);
  painter.drawEllipse(rect);
}

QPen badgePen(const QRectF& rect) {
  return QPen{Qt::white, std::max(1.0, rect.width() / 6), Qt::SolidLine, Qt::RoundCap,
              Qt::RoundJoin};
}

QIcon statusIcon(anime::Status status) {
  static std::map<anime::Status, QIcon> icons;
  if (const auto it = icons.find(status); it != icons.end()) return it->second;

  const auto color = [status]() -> QColor {
    switch (status) {
      case anime::Status::Airing:
        return QColor{0x3a, 0x9b, 0x40};
      case anime::Status::FinishedAiring:
        return QColor{0x3b, 0x82, 0xd6};
      case anime::Status::NotYetAired:
        return QColor{0xd6, 0x3b, 0x3b};
      default:
        return QColor{0x9e, 0x9e, 0x9e};
    }
  }();

  QPixmap pixmap{16, 16};
  pixmap.fill(Qt::transparent);
  QPainter painter{&pixmap};
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(color);
  painter.drawEllipse(QRectF{4, 4, 8, 8});
  painter.end();

  return icons[status] = QIcon{pixmap};
}

void addSectionHeader(QVBoxLayout* layout, const QString& text, QWidget* parent) {
  auto label = new QLabel(text, parent);
  auto font = label->font();
  font.setBold(true);
  label->setFont(font);
  layout->addWidget(label);

  auto line = new QFrame(parent);
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Sunken);
  layout->addWidget(line);
}

QToolButton* createToolButton(const QString& icon, const QString& tooltip, QWidget* parent) {
  auto button = new QToolButton(parent);
  button->setIcon(theme.getIcon(icon));
  button->setToolTip(tooltip);
  button->setAutoRaise(true);
  return button;
}

// Title and description in two lines, like the tile view of Taiga 1.x
class PresetDelegate final : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const auto* style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

    const auto rect = opt.rect.adjusted(8, 4, -8, -4);
    const int iconSize = 20;
    opt.icon.paint(painter, QRect{rect.left(), rect.top() + 2, iconSize, iconSize});

    const auto textRect = rect.adjusted(iconSize + 8, 0, 0, 0);
    const auto lineHeight = opt.fontMetrics.height();
    const auto selected = opt.state.testFlag(QStyle::State_Selected);

    painter->save();
    painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
    painter->drawText(QRect{textRect.left(), textRect.top(), textRect.width(), lineHeight},
                      Qt::AlignLeft | Qt::AlignVCenter, index.data(Qt::DisplayRole).toString());
    painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText
                                               : QPalette::PlaceholderText));
    painter->drawText(
        QRect{textRect.left(), textRect.top() + lineHeight, textRect.width(), lineHeight},
        Qt::AlignLeft | Qt::AlignVCenter, index.data(kDescriptionRole).toString());
    painter->restore();
  }

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
    return {option.rect.width(), option.fontMetrics.height() * 2 + 12};
  }
};

std::vector<std::pair<int, QString>> titlesOfAnimeInProgress() {
  // Anime IDs and titles, as in Taiga 1.x: everything in the list but completed and dropped
  std::vector<std::pair<int, QString>> titles;
  for (const auto& entry : anime::db.entries()) {
    switch (entry.status) {
      case anime::list::Status::NotInList:
      case anime::list::Status::Completed:
      case anime::list::Status::Dropped:
        continue;
      default:
        break;
    }
    if (const auto* item = anime::db.item(entry.anime_id)) {
      titles.emplace_back(item->id, QString::fromStdString(anime::preferredTitle(*item)));
    }
  }

  QCollator collator;
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  std::ranges::sort(titles, [&collator](const auto& a, const auto& b) {
    return collator.compare(a.second, b.second) < 0;
  });
  return titles;
}

}  // namespace

QIcon feedFilterIcon(Action action) {
  static std::map<Action, QIcon> icons;
  if (const auto it = icons.find(action); it != icons.end()) return it->second;

  const auto& funnel = theme.getIcon(u"filter_alt"_s);

  QIcon icon;
  switch (action) {
    case Action::Discard:
      icon = composeIcon(funnel, [](QPainter& painter, const QRectF& rect) {
        drawBadgeCircle(painter, rect, QColor{0xd1, 0x34, 0x38});
        painter.setPen(badgePen(rect));
        const auto r = rect.adjusted(rect.width() * 0.3, rect.height() * 0.3,
                                     -rect.width() * 0.3, -rect.height() * 0.3);
        painter.drawLine(r.topLeft(), r.bottomRight());
        painter.drawLine(r.topRight(), r.bottomLeft());
      });
      break;
    case Action::Select:
      icon = composeIcon(funnel, [](QPainter& painter, const QRectF& rect) {
        drawBadgeCircle(painter, rect, QColor{0x2e, 0x9d, 0x48});
        painter.setPen(badgePen(rect));
        QPainterPath path;
        path.moveTo(rect.left() + rect.width() * 0.27, rect.top() + rect.height() * 0.52);
        path.lineTo(rect.left() + rect.width() * 0.44, rect.top() + rect.height() * 0.70);
        path.lineTo(rect.left() + rect.width() * 0.74, rect.top() + rect.height() * 0.33);
        painter.drawPath(path);
      });
      break;
    case Action::Prefer:
      icon = composeIcon(funnel, [](QPainter& painter, const QRectF& rect) {
        drawBadgeCircle(painter, rect, QColor{0x2e, 0x9d, 0x48});
        painter.setPen(badgePen(rect));
        const auto c = rect.center();
        const auto d = rect.width() * 0.25;
        painter.drawLine(QPointF{c.x() - d, c.y()}, QPointF{c.x() + d, c.y()});
        painter.drawLine(QPointF{c.x(), c.y() - d}, QPointF{c.x(), c.y() + d});
      });
      break;
  }

  return icons[action] = icon;
}

QIcon feedFilterCustomIcon() {
  static const QIcon icon = composeIcon(
      theme.getIcon(u"filter_alt"_s), [](QPainter& painter, const QRectF& rect) {
        drawBadgeCircle(painter, rect, QColor{0xe0, 0x8a, 0x1e});
        theme.getIcon(u"edit"_s).paint(&painter, rect.adjusted(1, 1, -1, -1).toRect());
      });
  return icon;
}

////////////////////////////////////////////////////////////////////////////////
// Condition dialog

FeedConditionDialog::FeedConditionDialog(QWidget* parent, const Condition& condition)
    : QDialog(parent),
      m_element(new QComboBox(this)),
      m_operator(new QComboBox(this)),
      m_value(new QComboBox(this)) {
  // Like Taiga 1.x, a condition that isn't empty is being edited (which is also the case for
  // new conditions, as their default element is the file name).
  const Condition empty{.element = Element::MetaId, .op = Operator::Equals, .value = {}};
  setWindowTitle(condition.element != empty.element || condition.op != empty.op ||
                         !condition.value.empty()
                     ? tr("Edit Condition")
                     : tr("Add Condition"));
  setMinimumWidth(640);

  auto layout = new QVBoxLayout(this);
  auto grid = new QGridLayout();
  grid->addWidget(new QLabel(tr("Element:"), this), 0, 0);
  grid->addWidget(new QLabel(tr("Operator:"), this), 0, 1);
  grid->addWidget(new QLabel(tr("Value:"), this), 0, 2);
  grid->addWidget(m_element, 1, 0);
  grid->addWidget(m_operator, 1, 1);
  grid->addWidget(m_value, 1, 2);
  grid->setColumnStretch(0, 3);
  grid->setColumnStretch(1, 2);
  grid->setColumnStretch(2, 3);
  layout->addLayout(grid);
  layout->addStretch();

  auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  // Add elements
  for (const auto element : kElements) {
    m_element->addItem(translateElement(element), static_cast<int>(element));
  }

  // Set element
  m_element->setCurrentIndex(static_cast<int>(condition.element));
  chooseElement(condition.element);
  connect(m_element, &QComboBox::currentIndexChanged, this, [this](int index) {
    chooseElement(static_cast<Element>(m_element->itemData(index).toInt()));
  });

  // Set operator
  m_operator->setCurrentIndex(std::max(0, m_operator->findData(static_cast<int>(condition.op))));

  // Set value
  const auto value = QString::fromStdString(condition.value);
  switch (condition.element) {
    case Element::MetaId: {
      m_value->setCurrentIndex(0);
      const int index = m_value->findData(value.toInt());
      if (index > -1) {
        m_value->setCurrentIndex(index);
      } else if (value.toInt() > 0) {
        // Keep anime that aren't in progress (e.g. completed), instead of losing them
        const auto* item = anime::db.item(value.toInt());
        m_value->addItem(item ? QString::fromStdString(anime::preferredTitle(*item))
                              : u"%1 (?)"_s.arg(value),
                         value.toInt());
        m_value->setCurrentIndex(m_value->count() - 1);
      }
      break;
    }
    case Element::UserStatus:
      m_value->setCurrentIndex(value.toInt());
      break;
    case Element::MetaStatus:
    case Element::MetaType:
      m_value->setCurrentIndex(value.toInt() - 1);
      break;
    case Element::LocalEpisodeAvailable:
      m_value->setCurrentIndex(value == u"True" ? 1 : 0);
      break;
    default:
      m_value->setCurrentText(value);
      break;
  }
}

std::optional<Condition> FeedConditionDialog::edit(QWidget* parent, const Condition& condition) {
  FeedConditionDialog dialog(parent, condition);
  if (dialog.exec() != QDialog::Accepted) return std::nullopt;
  return dialog.condition();
}

Condition FeedConditionDialog::condition() const {
  Condition condition{
      .element = static_cast<Element>(m_element->currentData().toInt()),
      .op = static_cast<Operator>(m_operator->currentData().toInt()),
      .value = {},
  };

  switch (condition.element) {
    case Element::MetaId:
    case Element::MetaStatus:
    case Element::MetaType:
    case Element::UserStatus:
      condition.value = std::to_string(m_value->currentData().toInt());
      break;
    default:
      condition.value = m_value->currentText().toStdString();
      break;
  }

  if (condition.element == Element::FileSize) {
    const auto value = QString::fromStdString(condition.value);
    if (!value.isEmpty() && std::ranges::all_of(value, [](QChar c) { return c.isDigit(); })) {
      condition.value += " MiB";
    }
  }

  return condition;
}

void FeedConditionDialog::chooseElement(Element element) {
  // Operator
  const auto previousOperator = m_operator->currentData();
  m_operator->clear();

  const auto addOperator = [this](Operator op) {
    m_operator->addItem(translateOperator(op), static_cast<int>(op));
  };

  switch (element) {
    using enum Element;
    case FileSize:
    case MetaId:
    case EpisodeNumber:
    case MetaDateStart:
    case MetaDateEnd:
    case MetaEpisodes:
      for (const auto op : {Operator::Equals, Operator::NotEquals, Operator::IsGreaterThan,
                            Operator::IsGreaterThanOrEqualTo, Operator::IsLessThan,
                            Operator::IsLessThanOrEqualTo}) {
        addOperator(op);
      }
      break;
    case FileCategory:
    case LocalEpisodeAvailable:
    case MetaStatus:
    case MetaType:
    case UserStatus:
      addOperator(Operator::Equals);
      addOperator(Operator::NotEquals);
      break;
    case UserNotes:
    case EpisodeTitle:
    case EpisodeGroup:
    case EpisodeVideoType:
    case FileTitle:
    case FileDescription:
    case FileLink:
      for (const auto op : {Operator::Equals, Operator::NotEquals, Operator::BeginsWith,
                            Operator::EndsWith, Operator::Contains, Operator::NotContains}) {
        addOperator(op);
      }
      break;
    default:
      for (const auto op : kOperators) addOperator(op);
      break;
  }

  m_operator->setCurrentIndex(std::max(0, m_operator->findData(previousOperator)));

  // Value
  m_value->clear();
  m_value->setEditable(true);
  m_value->lineEdit()->setPlaceholderText({});

  const auto setDropDownList = [this]() { m_value->setEditable(false); };

  switch (element) {
    using enum Element;
    case FileCategory:
      setDropDownList();
      for (const auto category :
           {TorrentCategory::Anime, TorrentCategory::Batch, TorrentCategory::Other}) {
        m_value->addItem(categoryName(category), static_cast<int>(category));
      }
      break;
    case FileSize:
      m_value->addItems({u"10 MiB"_s, u"100 MiB"_s, u"1 GiB"_s});
      break;
    case MetaId:
    case EpisodeTitle:
      if (element == MetaId) {
        setDropDownList();
        m_value->addItem(tr("(Unknown)"), 0);
      }
      for (const auto& [id, title] : titlesOfAnimeInProgress()) {
        m_value->addItem(title, id);
      }
      break;
    case MetaDateStart:
    case MetaDateEnd:
      m_value->addItem(QDate::currentDate().toString(Qt::ISODate));
      m_value->lineEdit()->setPlaceholderText(u"YYYY-MM-DD"_s);
      break;
    case MetaStatus:
      setDropDownList();
      for (const auto status : anime::kStatuses) {
        m_value->addItem(translateAiringStatus(static_cast<int>(status)),
                         static_cast<int>(status));
      }
      break;
    case MetaType:
      setDropDownList();
      for (const auto type : anime::kTypes) {
        m_value->addItem(translateType(static_cast<int>(type)), static_cast<int>(type));
      }
      break;
    case UserStatus:
      setDropDownList();
      m_value->addItem(translateListStatus(static_cast<int>(anime::list::Status::NotInList)),
                       static_cast<int>(anime::list::Status::NotInList));
      for (const auto status : anime::list::kStatuses) {
        m_value->addItem(translateListStatus(static_cast<int>(status)), static_cast<int>(status));
      }
      break;
    case EpisodeNumber:
    case MetaEpisodes:
      m_value->addItems({u"%watched%"_s, u"%total%"_s});
      break;
    case EpisodeVersion:
      m_value->addItems({u"2"_s, u"3"_s, u"4"_s, u"0"_s});
      break;
    case LocalEpisodeAvailable:
      setDropDownList();
      m_value->addItems({u"False"_s, u"True"_s});
      break;
    case EpisodeVideoResolution:
      m_value->addItems({u"1080p"_s, u"720p"_s, u"480p"_s, u"400p"_s});
      break;
    case EpisodeVideoType:
      m_value->addItems({u"h264"_s, u"x264"_s, u"XviD"_s});
      break;
    default:
      break;
  }

  m_value->setCurrentIndex(m_value->count() > 0 ? 0 : -1);
}

////////////////////////////////////////////////////////////////////////////////
// Filter dialog

FeedFilterDialog::FeedFilterDialog(QWidget* parent, const std::optional<Filter>& filter)
    : QDialog(parent), m_header(new QLabel(this)), m_pages(new QStackedWidget(this)) {
  resize(760, 570);

  auto layout = new QVBoxLayout(this);

  // Main instruction
  auto headerFont = m_header->font();
  headerFont.setPointSizeF(headerFont.pointSizeF() * 1.35);
  m_header->setFont(headerFont);
  m_header->setForegroundRole(QPalette::Link);
  m_header->setWordWrap(true);
  layout->addWidget(m_header);
  layout->addSpacing(8);

  m_pages->addWidget(createPage0());
  m_pages->addWidget(createPage1());
  m_pages->addWidget(createPage2());
  layout->addWidget(m_pages, 1);

  auto line = new QFrame(this);
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Sunken);
  layout->addWidget(line);

  auto buttons = new QHBoxLayout();
  buttons->addStretch();
  m_backButton = new QPushButton(tr("< Back"), this);
  m_nextButton = new QPushButton(tr("Next >"), this);
  m_nextButton->setDefault(true);
  auto cancelButton = new QPushButton(tr("Cancel"), this);
  buttons->addWidget(m_backButton);
  buttons->addWidget(m_nextButton);
  buttons->addSpacing(8);
  buttons->addWidget(cancelButton);
  layout->addLayout(buttons);

  connect(m_backButton, &QPushButton::clicked, this, [this]() { choosePage(m_currentPage - 1); });
  connect(m_nextButton, &QPushButton::clicked, this, [this]() { choosePage(m_currentPage + 1); });
  connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

  // Choose and display a page
  if (filter && !filter->conditions.empty()) {
    setWindowTitle(tr("Edit Filter"));
    m_filter = *filter;
    m_currentPage = 1;
    choosePage(1);
  } else {
    setWindowTitle(tr("Add New Filter"));
    choosePage(0);
  }
}

std::optional<Filter> FeedFilterDialog::edit(QWidget* parent, const std::optional<Filter>& filter) {
  FeedFilterDialog dialog(parent, filter);
  if (dialog.exec() != QDialog::Accepted) return std::nullopt;
  return dialog.filter();
}

const Filter& FeedFilterDialog::filter() const {
  return m_filter;
}

bool FeedFilterDialog::eventFilter(QObject* watched, QEvent* event) {
  // Keys of the condition list in Taiga 1.x
  if (watched == m_conditionList && event->type() == QEvent::KeyPress) {
    switch (static_cast<QKeyEvent*>(event)->key()) {
      case Qt::Key_Delete:
        deleteCondition();
        return true;
      case Qt::Key_Minus:
        moveCondition(-1);
        return true;
      case Qt::Key_Plus:
        moveCondition(1);
        return true;
      case Qt::Key_Return:
      case Qt::Key_Enter:
        editCondition();
        return true;
    }
  }
  return QDialog::eventFilter(watched, event);
}

QWidget* FeedFilterDialog::createPage0() {
  auto page = new QWidget(this);
  auto layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);

  m_presetList = new QListWidget(page);
  m_presetList->setItemDelegate(new PresetDelegate(m_presetList));
  m_presetList->setSelectionMode(QAbstractItemView::SingleSelection);
  layout->addWidget(m_presetList);

  const auto& allPresets = presets();
  for (size_t i = 0; i < allPresets.size(); ++i) {
    const auto& preset = allPresets[i];
    if (preset.is_default) continue;
    auto item = new QListWidgetItem(QString::fromStdString(preset.filter.name), m_presetList);
    item->setIcon(preset.filter.conditions.empty() ? feedFilterCustomIcon()
                                                   : feedFilterIcon(preset.filter.action));
    item->setData(kPresetRole, static_cast<int>(i));
    item->setData(kDescriptionRole, QString::fromStdString(preset.description));
  }
  m_presetList->setCurrentRow(0);

  connect(m_presetList, &QListWidget::itemDoubleClicked, this, [this]() { choosePage(1); });

  return page;
}

QWidget* FeedFilterDialog::createPage1() {
  auto page = new QWidget(this);
  auto layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);

  // Filter name
  addSectionHeader(layout, tr("Filter name"), page);
  m_name = new QLineEdit(page);
  m_name->setPlaceholderText(tr("Type something to identify this filter"));
  layout->addWidget(m_name);
  layout->addSpacing(8);

  // Conditions
  addSectionHeader(layout, tr("Conditions"), page);
  auto conditionsLayout = new QHBoxLayout();
  m_conditionList = new QTreeWidget(page);
  m_conditionList->setRootIsDecorated(false);
  m_conditionList->setUniformRowHeights(true);
  m_conditionList->setAllColumnsShowFocus(true);
  m_conditionList->setHeaderLabels({tr("Element"), tr("Operator"), tr("Value")});
  m_conditionList->header()->setStretchLastSection(true);
  m_conditionList->setColumnWidth(0, 210);
  m_conditionList->setColumnWidth(1, 200);
  m_conditionList->installEventFilter(this);
  conditionsLayout->addWidget(m_conditionList, 1);

  auto toolbar = new QVBoxLayout();
  auto addButton = createToolButton(u"add"_s, tr("Add new condition..."), page);
  m_deleteConditionButton = createToolButton(u"remove"_s, tr("Delete condition"), page);
  m_moveUpButton = createToolButton(u"arrow_upward"_s, tr("Move up"), page);
  m_moveDownButton = createToolButton(u"arrow_downward"_s, tr("Move down"), page);
  auto separator = new QFrame(page);
  separator->setFrameShape(QFrame::HLine);
  separator->setFrameShadow(QFrame::Sunken);
  toolbar->addWidget(addButton);
  toolbar->addWidget(m_deleteConditionButton);
  toolbar->addWidget(separator);
  toolbar->addWidget(m_moveUpButton);
  toolbar->addWidget(m_moveDownButton);
  toolbar->addStretch();
  conditionsLayout->addLayout(toolbar);
  layout->addLayout(conditionsLayout, 1);
  layout->addSpacing(8);

  connect(addButton, &QToolButton::clicked, this, &FeedFilterDialog::addCondition);
  connect(m_deleteConditionButton, &QToolButton::clicked, this, &FeedFilterDialog::deleteCondition);
  connect(m_moveUpButton, &QToolButton::clicked, this, [this]() { moveCondition(-1); });
  connect(m_moveDownButton, &QToolButton::clicked, this, [this]() { moveCondition(1); });
  connect(m_conditionList, &QTreeWidget::itemSelectionChanged, this,
          &FeedFilterDialog::updateConditionButtons);
  connect(m_conditionList, &QTreeWidget::itemDoubleClicked, this, &FeedFilterDialog::editCondition);

  // Options
  addSectionHeader(layout, tr("Options"), page);
  auto options = new QGridLayout();
  options->setColumnStretch(0, 1);
  options->setColumnStretch(1, 1);
  options->setHorizontalSpacing(48);

  m_match = new QComboBox(page);
  for (const auto match : {Match::All, Match::Any}) {
    m_match->addItem(translateMatching(match), static_cast<int>(match));
  }
  m_action = new QComboBox(page);
  for (const auto action : {Action::Discard, Action::Select, Action::Prefer}) {
    m_action->addItem(translateAction(action), static_cast<int>(action));
  }
  m_option = new QComboBox(page);
  for (const auto option : {Option::Default, Option::Deactivate, Option::Hide}) {
    m_option->addItem(translateOption(option), static_cast<int>(option));
  }
  m_optionLabel = new QLabel(tr("Discard type:"), page);

  options->addWidget(new QLabel(tr("Match:"), page), 0, 0);
  options->addWidget(new QLabel(tr("Action:"), page), 0, 1);
  options->addWidget(m_match, 1, 0);
  options->addWidget(m_action, 1, 1);
  options->addWidget(m_optionLabel, 2, 1);
  options->addWidget(m_option, 3, 1);
  layout->addLayout(options);

  connect(m_action, &QComboBox::activated, this, &FeedFilterDialog::changeAction);

  return page;
}

QWidget* FeedFilterDialog::createPage2() {
  auto page = new QWidget(this);
  auto layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);

  m_animeList = new QTreeWidget(page);
  m_animeList->setHeaderHidden(true);
  m_animeList->setUniformRowHeights(true);
  layout->addWidget(m_animeList, 1);

  m_limitLabel = new QLabel(page);
  m_limitLabel->setWordWrap(true);
  layout->addWidget(m_limitLabel);

  connect(m_animeList, &QTreeWidget::itemChanged, this, &FeedFilterDialog::updateLimitText);

  return page;
}

void FeedFilterDialog::choosePage(int index) {
  switch (index) {
    case 0:
      m_header->setText(tr("Choose one of the preset filters, or create a custom one"));
      break;

    case 1:
      if (m_currentPage == 0) buildFilterFromPreset();
      m_name->setText(QString::fromStdString(m_filter.name));
      refreshConditionList();
      m_match->setCurrentIndex(static_cast<int>(m_filter.match));
      m_action->setCurrentIndex(static_cast<int>(m_filter.action));
      m_option->setCurrentIndex(static_cast<int>(m_filter.option));
      changeAction();
      m_header->setText(tr("Change filter options and add conditions"));
      break;

    case 2:
      if (m_currentPage == 1) {
        if (m_filter.conditions.empty()) {
          QMessageBox box(QMessageBox::Critical, windowTitle(),
                          tr("There must be at least one condition in order to create a filter."),
                          QMessageBox::Ok, this);
          box.setInformativeText(
              tr("You may add a condition using the Add New Condition button, or by choosing a "
                 "preset from the previous page."));
          box.exec();
          return;
        }
        buildFilterFromOptions();
      }
      refreshAnimeList();
      m_header->setText(tr("Limit this filter to one or more anime title, or leave it blank to "
                           "apply to all items"));
      break;

    case 3:
      if (m_currentPage == 2) buildFilterFromAnimeList();
      accept();
      return;

    default:
      return;
  }

  m_pages->setCurrentIndex(index);
  m_backButton->setEnabled(index > 0);
  m_nextButton->setText(index < 2 ? tr("Next >") : tr("Finish"));
  m_currentPage = index;
}

void FeedFilterDialog::buildFilterFromPreset() {
  const auto* item = m_presetList->currentItem();
  if (!item || !item->isSelected()) return;  // keep the filter as it is

  const auto& preset = presets().at(static_cast<size_t>(item->data(kPresetRole).toInt()));
  m_filter = preset.filter.conditions.empty() ? Filter{} : preset.filter;

  // Going back and forth shouldn't reset the filter again
  m_presetList->clearSelection();
}

void FeedFilterDialog::buildFilterFromOptions() {
  m_filter.name = m_name->text().toStdString();
  m_filter.match = static_cast<Match>(m_match->currentIndex());
  m_filter.action = static_cast<Action>(m_action->currentIndex());
  m_filter.option = static_cast<Option>(m_option->currentIndex());
}

void FeedFilterDialog::buildFilterFromAnimeList() {
  m_filter.anime_ids.clear();
  for (int i = 0; i < m_animeList->topLevelItemCount(); ++i) {
    const auto* group = m_animeList->topLevelItem(i);
    for (int j = 0; j < group->childCount(); ++j) {
      const auto* child = group->child(j);
      if (child->checkState(0) == Qt::Checked) {
        m_filter.anime_ids.push_back(child->data(0, kAnimeIdRole).toInt());
      }
    }
  }
  m_filter.anime_ids.insert(m_filter.anime_ids.end(), m_hiddenAnimeIds.begin(),
                            m_hiddenAnimeIds.end());
}

void FeedFilterDialog::refreshConditionList() {
  m_conditionList->clear();
  for (const auto& condition : m_filter.conditions) {
    auto item = new QTreeWidgetItem(m_conditionList);
    item->setIcon(0, theme.getIcon(u"filter_alt"_s));
    item->setText(0, translateElement(condition.element));
    item->setText(1, translateOperator(condition.op));
    item->setText(2, translateValue(condition));
  }
  updateConditionButtons();
}

void FeedFilterDialog::refreshAnimeList() {
  const QSignalBlocker blocker{m_animeList};
  m_animeList->clear();
  m_hiddenAnimeIds.clear();

  std::map<anime::list::Status, QTreeWidgetItem*> groups;
  for (const auto status : anime::list::kStatuses) {
    auto group = new QTreeWidgetItem(m_animeList);
    group->setText(0, translateListStatus(static_cast<int>(status)));
    auto font = group->font(0);
    font.setBold(true);
    group->setFont(0, font);
    group->setFlags(Qt::ItemIsEnabled);
    groups[status] = group;
  }

  std::vector<std::tuple<anime::list::Status, QString, int, anime::Status>> rows;
  for (const auto& entry : anime::db.entries()) {
    if (!anime::list::isInList(&entry)) continue;
    const auto* item = anime::db.item(entry.anime_id);
    if (!item) continue;
    rows.emplace_back(entry.status, QString::fromStdString(anime::preferredTitle(*item)), item->id,
                      item->status);
  }

  QCollator collator;
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  std::ranges::sort(rows, [&collator](const auto& a, const auto& b) {
    return collator.compare(std::get<1>(a), std::get<1>(b)) < 0;
  });

  std::vector<int> listedIds;
  for (const auto& [status, title, id, airingStatus] : rows) {
    const auto group = groups.find(status);
    if (group == groups.end()) continue;
    auto child = new QTreeWidgetItem(group->second);
    child->setText(0, title);
    child->setIcon(0, statusIcon(airingStatus));
    child->setData(0, kAnimeIdRole, id);
    child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
    const bool checked = std::ranges::contains(m_filter.anime_ids, id);
    child->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
    listedIds.push_back(id);
  }

  // Limits for anime that are not in the list are kept as they are.
  for (const auto id : m_filter.anime_ids) {
    if (!std::ranges::contains(listedIds, id)) m_hiddenAnimeIds.push_back(id);
  }

  // Groups other than "Currently watching" start collapsed, unless they have checked items
  for (const auto& [status, group] : groups) {
    group->setText(0, u"%1 (%2)"_s.arg(group->text(0)).arg(group->childCount()));
    bool hasChecked = false;
    for (int i = 0; i < group->childCount(); ++i) {
      hasChecked = hasChecked || group->child(i)->checkState(0) == Qt::Checked;
    }
    group->setExpanded(status == anime::list::Status::Watching || hasChecked);
  }

  updateLimitText();
}

void FeedFilterDialog::updateConditionButtons() {
  const auto index = m_conditionList->currentIndex().row();
  const auto selected = !m_conditionList->selectedItems().isEmpty();
  const auto count = m_conditionList->topLevelItemCount();
  m_deleteConditionButton->setEnabled(selected);
  m_moveUpButton->setEnabled(selected && index > 0);
  m_moveDownButton->setEnabled(selected && index > -1 && index < count - 1);
}

void FeedFilterDialog::updateLimitText() {
  QStringList titles;
  for (int i = 0; i < m_animeList->topLevelItemCount(); ++i) {
    const auto* group = m_animeList->topLevelItem(i);
    for (int j = 0; j < group->childCount(); ++j) {
      if (group->child(j)->checkState(0) == Qt::Checked) titles.append(group->child(j)->text(0));
    }
  }
  auto text = titles.isEmpty() ? tr("(nothing)") : titles.join(u", "_s);
  if (!m_hiddenAnimeIds.empty()) {
    text += tr(" (and %n anime not in your list)", nullptr,
               static_cast<int>(m_hiddenAnimeIds.size()));
  }
  m_limitLabel->setText(tr("Currently limited to: %1").arg(text));
}

void FeedFilterDialog::changeAction() {
  const auto action = static_cast<Action>(m_action->currentIndex());
  const bool enabled = action == Action::Discard || action == Action::Prefer;
  if (!enabled) m_option->setCurrentIndex(static_cast<int>(Option::Default));
  m_option->setEnabled(enabled);
  m_option->setVisible(enabled);
  m_optionLabel->setVisible(enabled);
}

void FeedFilterDialog::addCondition() {
  const auto condition = FeedConditionDialog::edit(this);
  if (!condition) return;
  m_filter.conditions.push_back(*condition);
  refreshConditionList();
  m_conditionList->setCurrentItem(
      m_conditionList->topLevelItem(m_conditionList->topLevelItemCount() - 1));
}

void FeedFilterDialog::editCondition() {
  const auto index = m_conditionList->currentIndex().row();
  if (index < 0 || index >= static_cast<int>(m_filter.conditions.size())) return;
  const auto condition = FeedConditionDialog::edit(this, m_filter.conditions[index]);
  if (!condition) return;
  m_filter.conditions[index] = *condition;
  refreshConditionList();
  m_conditionList->setCurrentItem(m_conditionList->topLevelItem(index));
}

void FeedFilterDialog::deleteCondition() {
  const auto index = m_conditionList->currentIndex().row();
  if (index < 0 || index >= static_cast<int>(m_filter.conditions.size())) return;
  m_filter.conditions.erase(m_filter.conditions.begin() + index);
  refreshConditionList();
}

void FeedFilterDialog::moveCondition(int offset) {
  const auto index = m_conditionList->currentIndex().row();
  const auto target = index + offset;
  if (index < 0 || target < 0 || target >= static_cast<int>(m_filter.conditions.size())) return;
  std::iter_swap(m_filter.conditions.begin() + index, m_filter.conditions.begin() + target);
  refreshConditionList();
  m_conditionList->setCurrentItem(m_conditionList->topLevelItem(target));
}

}  // namespace gui
