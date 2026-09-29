#pragma once
#ifndef __EMSCRIPTEN__

#include "INetlistProvider.h"
#include <nlohmann/json.hpp>
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

    // Load a pre-built SNL database (Cap'n Proto serialized directory).
    void loadSNL(const std::string& path);

    // Load structural Verilog.  Liberty files define the primitive cell library.
    void loadVerilog(const std::vector<std::string>& verilogFiles,
                     const std::vector<std::string>& libertyFiles);

    // Load SystemVerilog via slang.
    // Each entry in sources is either a .sv/.v file or a .f/.flist file;
    // Flist entries are handed to slang's own "-f" command-file reader.
    // If topModule is non-empty, it is forced as the elaboration root (via
    // slang's "--top") instead of relying on findAndSetTop()'s heuristic.
    void loadSystemVerilog(const std::vector<std::string>& sources,
                           const std::string& topModule = "");

    // Serve an already-built DB (e.g. one a test builds with the naja API);
    // its top design is the design shown.
    void setDB(naja::NL::NLDB* db) { db_ = db; }

  private:
    std::function<void()>                   openCb_;
    std::function<void(const std::string&)> msgCb_;
    std::function<void()>                   closeCb_;
    std::function<void(const std::string&)> errCb_;

    naja::NL::NLDB* db_ {nullptr};

    // Reset the SNL universe to a clean state and return a fresh NLDB.
    naja::NL::NLDB* freshDB();

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
    std::string buildInstanceInternalsResponse(const nlohmann::json& req) const;
    std::string buildSourceResponse(const nlohmann::json& req) const;
    std::string buildPropertiesResponse(const nlohmann::json& req) const;
    std::string buildResolveInstanceResponse(const nlohmann::json& req) const;
};

#endif // __EMSCRIPTEN__
