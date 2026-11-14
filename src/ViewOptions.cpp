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
    else { error = "group_by: expected \"model\", \"hierarchy\" or \"role\", got " + v.dump(); return false; }
  }
  *this = next;
  return true;
}

json ViewOptions::toJson() const {
  return { { "instance_label", name(instanceLabel) },
           { "sink_rows", sinkRows },
           { "max_sinks", maxSinks },
           { "group_by", name(groupBy) } };
}
