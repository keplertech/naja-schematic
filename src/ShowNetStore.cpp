#include "ShowNetStore.h"

#include <algorithm>
#include <vector>

#include "Console.h"
#include "TraceStore.h"

using json = nlohmann::json;

namespace {

struct HostNet {
  json        idPath;   // the pin: instance ids, term id, bit (null if none)
  unsigned    termId = 0;
  json        bit;
  std::string label;
  std::optional<ImU32> color;
  bool        trace = false;  // its fan-in cone (trace_driver), not just its net
  int         traceId = 0;
  bool        pending = true;  // load_equipotential sent, no reply yet
};

std::vector<HostNet> g_nets;
std::vector<int>     g_dropped;  // trace ids of forgotten nets still in flight

bool isIndex(const json& v) { return v.is_number_integer() && v.get<long long>() >= 0; }

// Registers the trace for `net` and gives it the host's color.
void beginTrace(HostNet& net) {
  net.traceId = TraceStore::begin(net.label);
  if (net.color && !TraceStore::setColor(net.traceId, *net.color))
    Console::Error(net.label + ": color too dark or too light, using the palette's");
}

// Records that are neither in flight nor drawn any more (their trace was
// removed, or the view cleared) no longer hold their pin back.
void prune() {
  g_nets.erase(std::remove_if(g_nets.begin(), g_nets.end(),
                              [](const HostNet& n) {
                                return !n.pending && !TraceStore::find(n.traceId);
                              }),
               g_nets.end());
}

}  // namespace

std::optional<ImU32> ShowNetStore::parseColor(const std::string& text) {
  if (text.size() != 7 || text[0] != '#') return std::nullopt;
  unsigned rgb[3];
  for (int i = 0; i < 3; ++i) {
    unsigned v = 0;
    for (int k = 1; k <= 2; ++k) {
      const char c = text[size_t(2 * i + k)];
      v <<= 4;
      if      (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else return std::nullopt;
    }
    rgb[i] = v;
  }
  return IM_COL32(rgb[0], rgb[1], rgb[2], 255);
}

std::optional<json> ShowNetStore::request(const json& push,
                                          const std::function<void()>& clearView) {
  const json idPath = push.value("id_path", json());
  const bool pathOk = idPath.is_array() &&
                      std::all_of(idPath.begin(), idPath.end(), isIndex);
  const json bit = push.contains("bit") && !push["bit"].is_null() ? push["bit"] : json();
  if (!pathOk || !push.contains("term_id") || !isIndex(push["term_id"]) ||
      !(bit.is_null() || isIndex(bit))) {
    Console::Error("Ignoring a malformed show_net: " + push.dump());
    return std::nullopt;
  }
  HostNet net;
  net.idPath = idPath;
  net.termId = push["term_id"].get<unsigned>();
  net.bit    = bit;
  net.label  = push.value("label", std::string("Net"));
  net.trace  = push.value("trace", false);
  if (push.contains("color") && push["color"].is_string()) {
    net.color = parseColor(push["color"].get<std::string>());
    if (!net.color) Console::Error(net.label + ": not a #rrggbb color, using the palette's");
  }

  if (!push.value("add", true)) {
    // Clearing the view also forgets its traces; nets still in flight
    // would come back into the cleared view.
    if (clearView) clearView();
    forget();
  }
  prune();
  const bool shown = std::any_of(g_nets.begin(), g_nets.end(), [&](const HostNet& n) {
    return n.idPath == net.idPath && n.termId == net.termId && n.bit == net.bit &&
           n.trace == net.trace;
  });
  if (shown) return std::nullopt;

  beginTrace(net);
  json req;
  req["request"]  = net.trace ? "trace_driver" : "load_equipotential";
  req["path"]     = net.idPath;
  req["term_id"]  = net.termId;
  if (!net.bit.is_null()) req["bit"] = net.bit;
  req["trace_id"] = net.traceId;
  g_nets.push_back(std::move(net));
  return req;
}

int ShowNetStore::takeReply(int traceId) {
  if (traceId == 0) return 0;
  auto dropped = std::find(g_dropped.begin(), g_dropped.end(), traceId);
  if (dropped != g_dropped.end()) {
    g_dropped.erase(dropped);
    return -1;
  }
  for (auto& net : g_nets) {
    if (net.traceId != traceId || !net.pending) continue;
    net.pending = false;
    if (!TraceStore::find(traceId)) beginTrace(net);  // a focus cleared it meanwhile
    return net.traceId;
  }
  return traceId;
}

void ShowNetStore::forget() {
  for (const auto& net : g_nets) {
    if (!net.pending) continue;
    g_dropped.push_back(net.traceId);
    TraceStore::remove(net.traceId);  // no row left for a net never drawn
  }
  g_nets.clear();
}
