#include "ViewOptions.h"

using json = nlohmann::json;

ViewOptions& ViewOptions::current() {
  static ViewOptions options;
  return options;
}

const char* ViewOptions::name(InstanceLabel v) {
  switch (v) {
    case InstanceLabel::Model: return "model";
    case InstanceLabel::Both:  return "both";
    default:                   return "name";
  }
}

const char* ViewOptions::name(FanoutGroupBy v) {
  switch (v) {
    case FanoutGroupBy::Hierarchy: return "hierarchy";
    case FanoutGroupBy::Role:      return "role";
    case FanoutGroupBy::Pin:       return "pin";
    default:                       return "model";
  }
}

bool ViewOptions::apply(const json& message, std::string& error) {
  ViewOptions next = *this;
  if (message.contains("instance_label")) {
    const json& v = message["instance_label"];
    const std::string s = v.is_string() ? v.get<std::string>() : "";
    if (s == "name")       next.instanceLabel = InstanceLabel::Name;
    else if (s == "model") next.instanceLabel = InstanceLabel::Model;
    else if (s == "both")  next.instanceLabel = InstanceLabel::Both;
    else { error = "instance_label: expected \"name\", \"model\" or \"both\", got " + v.dump(); return false; }
  }
  for (auto [key, field] : { std::pair{ "sink_rows", &next.sinkRows },
                             std::pair{ "max_sinks", &next.maxSinks } }) {
    if (!message.contains(key)) continue;
    const json& v = message[key];
    if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > 1000000) {
      error = std::string(key) + ": expected a count (0 = no limit), got " + v.dump();
      return false;
    }
    *field = v.get<int>();
  }
  if (message.contains("group_by")) {
    const json& v = message["group_by"];
    const std::string s = v.is_string() ? v.get<std::string>() : "";
    if (s == "model")          next.groupBy = FanoutGroupBy::Model;
    else if (s == "hierarchy") next.groupBy = FanoutGroupBy::Hierarchy;
    else if (s == "role")      next.groupBy = FanoutGroupBy::Role;
    else if (s == "pin")       next.groupBy = FanoutGroupBy::Pin;
    else { error = "group_by: expected \"model\", \"hierarchy\", \"role\" or \"pin\", got " + v.dump(); return false; }
  }
  for (auto [key, field] : { std::pair{ "hide_ui_glyphs", &next.hideUiGlyphs },
                             std::pair{ "legend", &next.legend },
                             std::pair{ "convergence_outline", &next.convergenceOutline } }) {
    if (!message.contains(key)) continue;
    const json& v = message[key];
    if (!v.is_boolean()) { error = std::string(key) + ": expected true or false, got " + v.dump(); return false; }
    *field = v.get<bool>();
  }
  for (auto [key, field] : { std::pair{ "title", &next.title }, std::pair{ "caption", &next.caption } }) {
    if (!message.contains(key)) continue;
    const json& v = message[key];
    if (!v.is_string()) { error = std::string(key) + ": expected a string, got " + v.dump(); return false; }
    *field = v.get<std::string>();
  }
  *this = next;
  return true;
}

json ViewOptions::toJson() const {
  return { { "instance_label", name(instanceLabel) },
           { "sink_rows", sinkRows },
           { "max_sinks", maxSinks },
           { "group_by", name(groupBy) },
           { "convergence_outline", convergenceOutline },
           { "hide_ui_glyphs", hideUiGlyphs },
           { "title", title },
           { "caption", caption },
           { "legend", legend } };
}

json ViewOptions::layoutJson() const {
  json j = toJson();
  for (const char* key : { "convergence_outline", "hide_ui_glyphs", "title", "caption", "legend" })
    j.erase(key);
  return j;
}
