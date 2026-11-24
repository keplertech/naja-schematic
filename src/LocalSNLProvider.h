#pragma once
#ifndef __EMSCRIPTEN__

#include "INetlistProvider.h"
#include "NajaProgress.h"
#include <atomic>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include <filesystem>

namespace naja::NL {
  class NLDB;
}

// INetlistProvider for standalone desktop mode.
// Backs the tree with naja SNL objects loaded directly in-process.
class LocalSNLProvider : public INetlistProvider {
  public:
    LocalSNLProvider();
    ~LocalSNLProvider() override;

    void send(const std::string& msg) override;
    void on_open(std::function<void()> callback) override;
    void on_message(std::function<void(const std::string&)> callback) override;
    void on_close(std::function<void()> callback) override;
    void on_error(std::function<void(const std::string&)> callback) override;
    void start() override;

    // The loaders below return whether the design loaded. One that fails
    // (an error, or cancelled through the progress callback) keeps the
    // design loaded before it, and logs why.

    // Load a pre-built SNL database (Cap'n Proto serialized directory).
    bool loadSNL(const std::string& path);

    // Load structural Verilog.  Liberty files define the primitive cell library.
    // `yosysSourceAttributes` decodes Yosys (* src *) attributes into source
    // origins (naja only does it when asked: nothing establishes Yosys
    // provenance otherwise).
    bool loadVerilog(const std::vector<std::string>& verilogFiles,
                     const std::vector<std::string>& libertyFiles,
                     bool yosysSourceAttributes = false);

    // Load SystemVerilog via slang.
    // Each entry in sources is either a .sv/.v file or a .f/.flist file;
    // Flist entries are handed to slang's own "-f" command-file reader.
    // If topModule is non-empty, it is forced as the elaboration root (via
    // slang's "--top") instead of relying on findAndSetTop()'s heuristic.
    bool loadSystemVerilog(const std::vector<std::string>& sources,
                           const std::string& topModule = "");

    // Load VHDL (naja's experimental frontend; files in any order). If
    // topEntity is empty, naja infers the unique uninstantiated entity.
    bool loadVHDL(const std::vector<std::string>& files,
                  const std::string& topEntity = "");

    // Called by the loaders with naja's progress (stage, detail, work done
    // and to do), on the thread loading; returning false cancels the load.
    // Empty: none (the default).
    void setProgressCallback(naja::NajaProgressCallback callback) { progress_ = std::move(callback); }

    // While set, requests are dropped unanswered: a load on another thread
    // (DesignLoader) is replacing the DB they'd read. The viewer reloads
    // its tree once the load is over.
    void setLoading(bool loading) { loading_ = loading; }
    bool loading() const { return loading_; }
    // How many requests were dropped while loading, since the last call:
    // after a failed load (the design kept), the viewer reloads its tree if
    // any were, since they'll never be answered.
    size_t takeDroppedRequests() { return dropped_.exchange(0); }

    // A terminal named for a human or a script -- {"id_path"|"path": the
    // containing instance (root excluded, [] = a top-level port),
    // "terminal": its base name, "bit": optional bus bit} -- turned into the
    // {"path": ids, "term_id", "bit"} a load_equipotential/trace_driver
    // request carries. nullopt (and `error` set) if it doesn't resolve.
    // Whether a design with a top was loaded.
    bool hasDesign() const;

    std::optional<nlohmann::json> termRequest(const nlohmann::json& spec, std::string& error) const;

    // A directory relative source file references are looked up in, after
    // those added before and before the current directory (see
    // resolveSourceFile()).
    void addSourceRoot(const std::string& dir);
    // A path prefix rewrite for source files recorded on another machine
    // (a synthesis server's paths for a local checkout): a referenced path
    // starting with `from`, at a path boundary, is tried with `to` instead,
    // first (see resolveSourceFile()).
    void addSourceMap(const std::string& from, const std::string& to);

    // Serve an already-built DB (e.g. one a test builds with the naja API);
    // its top design is the design shown.
    void setDB(naja::NL::NLDB* db) {
      db_ = db;
      loadedAt_ = std::filesystem::file_time_type::clock::now();
    }

  private:
    std::function<void()>                   openCb_;
    std::function<void(const std::string&)> msgCb_;
    std::function<void()>                   closeCb_;
    std::function<void(const std::string&)> errCb_;

    naja::NL::NLDB* db_ {nullptr};
    naja::NajaProgressCallback progress_;
    std::atomic<bool> loading_ {false};
    std::atomic<size_t> dropped_ {0};
    std::vector<std::string> sourceRoots_;
    // When the design was loaded (freshDB(), setDB()): naja recorded its
    // source ranges then, so a source file modified later is "stale".
    std::filesystem::file_time_type loadedAt_ {};
    std::vector<std::pair<std::string, std::string>> sourceMaps_;

    // Reset the SNL universe to a clean state and return a fresh NLDB.
    naja::NL::NLDB* freshDB();
    // Run one loader's body: on an exception (cancelled included) log it
    // and put the design loaded before back.
    bool runLoad(const std::string& what, const std::function<void()>& body);

    // Find the top design among user libraries and register it in the DB.
    void findAndSetTop();

    // Find a design by name among user libraries and register it as top.
    // Returns false (and leaves the DB's top design untouched) if not found.
    bool setTopByName(const std::string& name);

    void        handleRequest(const std::string& jsonRequest);
    std::string buildRootResponse() const;
    std::string buildInstancesResponse(unsigned guiId, unsigned dbId,
                                       unsigned libId, unsigned designId,
                                       bool primitives) const;
    std::string buildTermsResponse(unsigned guiId, unsigned dbId,
                                   unsigned libId, unsigned designId) const;
    std::string buildNetsResponse(unsigned guiId, unsigned dbId,
                                  unsigned libId, unsigned designId) const;
    std::string buildEquipotentialResponse(const nlohmann::json& req) const;
    std::string buildTraceDriverResponse(const nlohmann::json& req) const;
    std::string buildExpandInstanceTermsResponse(const nlohmann::json& req) const;
    std::string buildInstanceViewResponse(const nlohmann::json& req) const;
    std::string buildSourcesResponse(const nlohmann::json& req) const;
    std::string buildSourceResponse(const nlohmann::json& req) const;
    std::string resolveSourceFile(const std::string& file) const;
    std::string buildPropertiesResponse(const nlohmann::json& req) const;
    std::string buildResolveInstanceResponse(const nlohmann::json& req) const;
    std::string buildTopViewResponse(const nlohmann::json& req) const;
};

#endif // __EMSCRIPTEN__
