/*
 * SPDX-License-Identifier: MIT
 */

#include "dla_session.h"

#include <atomic>
#include <cstdio>
#include <utility>

#include "cli_plugins.h"
#include "ipc_messages.h"

namespace fnirsi::tools {

namespace host = bitoscope::host;
namespace ipc = bitoscope::ipc;

namespace {

void printErrLine(std::string_view line) {
  std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
}

const ipc::FacetInfo* findFacet(const ipc::DeviceInfo& device, std::string_view name) {
  for (const ipc::FacetInfo& facet : device.facets) {
    if (facet.name == name) return &facet;
  }
  return nullptr;
}

}

std::expected<std::unique_ptr<DlaSession>, std::string> DlaSession::open(const DlaSessionOptions& options) {
  std::unique_ptr<DlaSession> session(new DlaSession());

  host::PipePluginHostOptions host_options;
  host_options.pluginDirectory = options.plugin_dir;
  host_options.requestTimeout = options.request_timeout;
  host_options.startTimeout = options.start_timeout;
  host_options.onStderrLine = [](std::string_view line) { printErrLine(line); };
  host_options.onLog = [](std::string_view plugin_id, const ipc::LogEvent& event) {
    printErrLine(bitoscope::cli::formatLogLine(plugin_id, event));
  };
  host_options.onClosed = [closing = session->closing_](std::string_view plugin_id, std::string_view reason) {
    if (!closing->load()) printErrLine("driver '" + std::string(plugin_id) + "' ended: " + std::string(reason));
  };
  session->host_ = std::make_unique<host::PipePluginHost>(std::move(host_options));

  auto device = bitoscope::cli::openDevice(*session->host_, kDlaDriver, options.device_id, options.plugin_dir);
  if (!device) return std::unexpected(device.error());
  session->device_ = std::move(*device);
  const std::string& id = session->device_.plugin_id;

  const ipc::FacetInfo* la = findFacet(session->device_.device, host::pipe::kLaAcquisitionFacetName);
  const ipc::FacetInfo* siggen = findFacet(session->device_.device, host::pipe::kSiggenFacetName);
  if (la == nullptr || siggen == nullptr) {
    return std::unexpected("driver '" + id + "' does not report both '" +
                           std::string(host::pipe::kSiggenFacetName) + "' and '" +
                           std::string(host::pipe::kLaAcquisitionFacetName) + "'");
  }
  for (const ipc::FacetInfo* facet : {la, siggen}) {
    if (!facet->unusable.empty()) {
      return std::unexpected("driver '" + id + "': its " + facet->name + " cannot be used: " + facet->unusable);
    }
  }
  auto la_caps = host::pipe::LaAcquisitionClient::parseCapabilities(la->info);
  if (!la_caps) return std::unexpected("driver '" + id + "': " + la_caps.error());
  auto siggen_caps = host::pipe::SiggenClient::parseCapabilities(siggen->info);
  if (!siggen_caps) return std::unexpected("driver '" + id + "': " + siggen_caps.error());

  session->la_.emplace(host::FacetClient(*session->host_, id, std::string(host::pipe::kLaAcquisitionFacetName)),
                       std::move(*la_caps));
  session->siggen_.emplace(host::FacetClient(*session->host_, id, std::string(host::pipe::kSiggenFacetName)),
                           std::move(*siggen_caps));
  return session;
}

DlaSession::~DlaSession() {
  closing_->store(true);
  siggen_.reset();
  la_.reset();
  host_.reset();
}

std::expected<void, std::string> DlaSession::setProperties(std::string_view facet,
                                                              std::span<const ipc::props::PropertyValue> values) {
  auto set = host_->setProperties(device_.plugin_id, facet, values);
  if (set) return {};
  return std::unexpected(set.error().message +
                         (set.error().property.empty() ? std::string() : " (property " + set.error().property + ")"));
}

std::optional<std::string> DlaSession::closedReason() const {
  if (!host_) return std::string("driver is not open");
  const auto connection = host_->connection(device_.plugin_id);
  if (!connection) return std::string("driver is not open");
  return connection->closedReason();
}

}
