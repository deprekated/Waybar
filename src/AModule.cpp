#include "AModule.hpp"

#include <fmt/format.h>

#include <util/command.hpp>

namespace waybar {

AModule::AModule(const Json::Value& config, const std::string& name, const std::string& id,
                 bool enable_click, bool enable_scroll)
    : name_(std::move(name)),
      config_(std::move(config)),
      isTooltip{config_["tooltip"].isBool() ? config_["tooltip"].asBool() : true},
      distance_scrolled_y_(0.0),
      distance_scrolled_x_(0.0) {
  // Configure module action Map
  const Json::Value actions{config_["actions"]};
  for (Json::Value::const_iterator it = actions.begin(); it != actions.end(); ++it) {
    if (it.key().isString() && it->isString())
      if (eventActionMap_.count(it.key().asString()) == 0) {
        eventActionMap_.insert({it.key().asString(), it->asString()});
        enable_click = true;
        enable_scroll = true;
      } else
        spdlog::warn("Dublicate action is ignored: {0}", it.key().asString());
    else
      spdlog::warn("Wrong actions section configuration. See config by index: {}", it.index());
  }

  event_box_.signal_enter_notify_event().connect(sigc::mem_fun(*this, &AModule::handleMouseEnter));
  event_box_.signal_leave_notify_event().connect(sigc::mem_fun(*this, &AModule::handleMouseLeave));

  // configure events' user commands
  // hasUserEvent is true if any element from eventMap_ is satisfying the condition in the lambda
  bool hasUserEvent =
      std::find_if(eventMap_.cbegin(), eventMap_.cend(), [&config](const auto& eventEntry) {
        // True if there is any non-release type event
        return eventEntry.first.second != GdkEventType::GDK_BUTTON_RELEASE &&
               config[eventEntry.second].isString();
      }) != eventMap_.cend();

  if (enable_click || hasUserEvent) {
    event_box_.add_events(Gdk::BUTTON_PRESS_MASK);
    event_box_.signal_button_press_event().connect(sigc::mem_fun(*this, &AModule::handleToggle));
  }

  bool hasReleaseEvent =
      std::find_if(eventMap_.cbegin(), eventMap_.cend(), [&config](const auto& eventEntry) {
        // True if there is any non-release type event
        return eventEntry.first.second == GdkEventType::GDK_BUTTON_RELEASE &&
               config[eventEntry.second].isString();
      }) != eventMap_.cend();
  if (hasReleaseEvent) {
    event_box_.add_events(Gdk::BUTTON_RELEASE_MASK);
    event_box_.signal_button_release_event().connect(sigc::mem_fun(*this, &AModule::handleRelease));
  }
  if (config_["on-scroll-up"].isString() || config_["on-scroll-down"].isString() ||
      config_["on-scroll-left"].isString() || config_["on-scroll-right"].isString() ||
      enable_scroll) {
    event_box_.add_events(Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK);
    event_box_.signal_scroll_event().connect(sigc::mem_fun(*this, &AModule::handleScroll));
  }
}

AModule::~AModule() {
  for (const auto& pid : pid_) {
    if (pid != -1) {
      killpg(pid, SIGTERM);
    }
  }
}

auto AModule::update() -> void {
  // Run user-provided update handler if configured
  if (config_["on-update"].isString()) {
    pid_.push_back(util::command::forkExec(config_["on-update"].asString()));
  }
}
// Get mapping between event name and module action name
// Then call overrided doAction in order to call appropriate module action
auto AModule::doAction(const std::string& name) -> void {
  if (!name.empty()) {
    const std::map<std::string, std::string>::const_iterator& recA{eventActionMap_.find(name)};
    // Call overrided action if derrived class has implemented it
    if (recA != eventActionMap_.cend() && name != recA->second) this->doAction(recA->second);
  }
}

bool AModule::handleMouseEnter(GdkEventCrossing* const& e) {
  if (auto* module = event_box_.get_child(); module != nullptr) {
    module->set_state_flags(Gtk::StateFlags::STATE_FLAG_PRELIGHT);
  }
  return false;
}

bool AModule::handleMouseLeave(GdkEventCrossing* const& e) {
  if (auto* module = event_box_.get_child(); module != nullptr) {
    module->unset_state_flags(Gtk::StateFlags::STATE_FLAG_PRELIGHT);
  }
  return false;
}

bool AModule::handleToggle(GdkEventButton* const& e) { return handleUserEvent(e); }

bool AModule::handleRelease(GdkEventButton* const& e) { return handleUserEvent(e); }

bool AModule::handleUserEvent(GdkEventButton* const& e) {
  std::string format{};
  const std::map<std::pair<uint, GdkEventType>, std::string>::const_iterator& rec{
      eventMap_.find(std::pair(e->button, e->type))};
  if (rec != eventMap_.cend()) {
    // First call module actions
    this->AModule::doAction(rec->second);

    format = rec->second;
  }
  // Second call user scripts
  if (!format.empty()) {
    if (config_[format].isString())
      format = config_[format].asString();
    else
      format.clear();
  }
  if (!format.empty()) {
    pid_.push_back(util::command::forkExec(format));
  }
  dp.emit();
  return true;
}

AModule::SCROLL_DIR AModule::getScrollDir(GdkEventScroll* e) {
  // only affects up/down
  bool reverse = config_["reverse-scrolling"].asBool();
  bool reverse_mouse = config_["reverse-mouse-scrolling"].asBool();

  // ignore reverse-scrolling if event comes from a mouse wheel
  GdkDevice* device = gdk_event_get_source_device((GdkEvent*)e);
  if (device != NULL && gdk_device_get_source(device) == GDK_SOURCE_MOUSE) {
    reverse = reverse_mouse;
  }

  switch (e->direction) {
    case GDK_SCROLL_UP:
      return reverse ? SCROLL_DIR::DOWN : SCROLL_DIR::UP;
    case GDK_SCROLL_DOWN:
      return reverse ? SCROLL_DIR::UP : SCROLL_DIR::DOWN;
    case GDK_SCROLL_LEFT:
      return SCROLL_DIR::LEFT;
    case GDK_SCROLL_RIGHT:
      return SCROLL_DIR::RIGHT;
    case GDK_SCROLL_SMOOTH: {
      SCROLL_DIR dir{SCROLL_DIR::NONE};

      distance_scrolled_y_ += e->delta_y;
      distance_scrolled_x_ += e->delta_x;

      gdouble threshold = 0;
      if (config_["smooth-scrolling-threshold"].isNumeric()) {
        threshold = config_["smooth-scrolling-threshold"].asDouble();
      }

      if (distance_scrolled_y_ < -threshold) {
        dir = reverse ? SCROLL_DIR::DOWN : SCROLL_DIR::UP;
      } else if (distance_scrolled_y_ > threshold) {
        dir = reverse ? SCROLL_DIR::UP : SCROLL_DIR::DOWN;
      } else if (distance_scrolled_x_ > threshold) {
        dir = SCROLL_DIR::RIGHT;
      } else if (distance_scrolled_x_ < -threshold) {
        dir = SCROLL_DIR::LEFT;
      }

      switch (dir) {
        case SCROLL_DIR::UP:
        case SCROLL_DIR::DOWN:
          distance_scrolled_y_ = 0;
          break;
        case SCROLL_DIR::LEFT:
        case SCROLL_DIR::RIGHT:
          distance_scrolled_x_ = 0;
          break;
        case SCROLL_DIR::NONE:
          break;
      }

      return dir;
    }
    // Silence -Wreturn-type:
    default:
      return SCROLL_DIR::NONE;
  }
}

bool AModule::handleScroll(GdkEventScroll* e) {
  auto dir = getScrollDir(e);
  std::string eventName{};

  if (dir == SCROLL_DIR::UP)
    eventName = "on-scroll-up";
  else if (dir == SCROLL_DIR::DOWN)
    eventName = "on-scroll-down";
  else if (dir == SCROLL_DIR::LEFT)
    eventName = "on-scroll-left";
  else if (dir == SCROLL_DIR::RIGHT)
    eventName = "on-scroll-right";

  // First call module actions
  this->AModule::doAction(eventName);
  // Second call user scripts
  if (config_[eventName].isString())
    pid_.push_back(util::command::forkExec(config_[eventName].asString()));

  dp.emit();
  return true;
}

bool AModule::tooltipEnabled() { return isTooltip; }

AModule::operator Gtk::Widget&() { return event_box_; }

}  // namespace waybar
