#include "node/data/managed_security.h"

#include <modsecurity/collection/collection.h>
#include <modsecurity/modsecurity.h>
#include <modsecurity/rule_message.h>
#include <modsecurity/rule_with_actions.h>
#include <modsecurity/rules_set.h>
#include <modsecurity/transaction.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

#include <openssl/rand.h>

#include "common/file_digest.h"
#include "common/route_policy.h"
#include "security_assets.h"

namespace flexedge::node {
namespace {

const std::filesystem::path& asset_directory() {
    static const auto directory = [] {
        auto root = std::filesystem::absolute(std::filesystem::path("state/security") / security_assets_version);
        std::filesystem::create_directories(root);
        for (const auto& asset : security_assets) {
            const auto path = root / asset.name_;
            if (std::filesystem::exists(path)) {
                if (flexedge::crypto::file_sha256(path) != asset.digest_) {
                    throw std::runtime_error("OWASP CRS asset checksum mismatch");
                }
                continue;
            }
            std::array<unsigned char, 16> nonce{};
            if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
                throw std::runtime_error("cannot create OWASP CRS asset cache");
            }
            const auto temporary = root / (std::string(asset.name_) + "." + flexedge::crypto::hex_encode(nonce) + ".tmp");
            try {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                for (const auto chunk : asset.chunks_) {
                    output.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                }
                output.close();
                if (!output || flexedge::crypto::file_sha256(temporary) != asset.digest_) {
                    throw std::runtime_error("cannot write OWASP CRS asset cache");
                }
                std::error_code error;
                std::filesystem::rename(temporary, path, error);
                if (error) {
                    // Another process preparing the same embedded package may have won.
                    if (!std::filesystem::exists(path) || flexedge::crypto::file_sha256(path) != asset.digest_) {
                        throw std::runtime_error("cannot publish OWASP CRS asset cache");
                    }
                    std::filesystem::remove(temporary);
                }
            } catch (...) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                throw;
            }
        }
        return root;
    }();
    return directory;
}

// This process-lifetime engine is initialized before it is reachable by requests.
// In particular, libxml's global cleanup cannot run on a configuration reload.
struct engine final {
    engine() {
        value_.setConnectorInformation("FlexEdge");
        (void)value_.whoAmI();
        value_.setServerLogCb([](void* context, const void* message) {
            if (!context || !message) {
                return;
            }
            auto& result = *static_cast<managed_security_result*>(context);
            const auto& rule = *static_cast<const modsecurity::RuleMessage*>(message);
            // No request contents or borrowed RuleMessage pointers leave the callback.
            if (result.matched_rules_.size() < 64) {
                result.matched_rules_.push_back(static_cast<std::uint32_t>(rule.m_rule.m_ruleId));
            }
        },
            modsecurity::RuleMessageLogProperty);
    }
    modsecurity::ModSecurity value_;
};

std::shared_ptr<engine> process_engine() {
    static const auto instance = std::make_shared<engine>();
    return instance;
}

void load(modsecurity::RulesSet& rules, const std::string& text) {
    if (rules.load(text.c_str()) < 0) {
        throw std::runtime_error("managed security configuration: " + rules.getParserError());
    }
}

bool intervention(modsecurity::Transaction& transaction, managed_security_result& result) {
    modsecurity::ModSecurityIntervention value{};
    value.status = 200;
    const bool disrupted = transaction.intervention(&value) && value.disruptive;
    if (disrupted) {
        result.status_ = value.status >= 400 && value.status <= 599
                             ? static_cast<std::uint16_t>(value.status)
                             : std::uint16_t{403};
    }
    std::free(value.url);
    std::free(value.log);
    return disrupted;
}

}  // namespace

struct managed_security::state final {
    // Destruction order: rules before engine. Neither is exposed to a consumer.
    std::shared_ptr<engine> engine_{process_engine()};
    modsecurity::RulesSet rules_;
    std::uint32_t body_limit_{};
};

managed_security::managed_security(const managed_security_settings& settings)
    : state_(std::make_unique<state>()) {
    if (settings.paranoia_level_ < 1 || settings.paranoia_level_ > 4 ||
        settings.inbound_threshold_ < 1 || settings.inbound_threshold_ > 1000 ||
        settings.body_limit_bytes_ < 1024 || settings.body_limit_bytes_ > 8 * 1024 * 1024 ||
        settings.excluded_rules_.size() > 512) {
        throw std::invalid_argument("invalid managed security settings");
    }
    state_->body_limit_ = settings.body_limit_bytes_;
    auto& rules = state_->rules_;
    const auto directory = asset_directory().generic_string();
    std::unordered_set<std::string> installed;
    for (const auto& asset : security_assets) {
        if (flexedge::crypto::file_sha256(std::filesystem::path(directory) / asset.name_) != asset.digest_) {
            throw std::runtime_error("OWASP CRS asset checksum mismatch");
        }
        installed.emplace(asset.name_);
    }
    load(rules, std::string("SecRuleEngine ") + (settings.detection_only_ ? "DetectionOnly" : "On") +
                    "\nSecRequestBodyAccess On\nSecResponseBodyAccess Off\nSecAuditEngine Off\nSecDebugLogLevel 0\n"
                    "SecRequestBodyLimit " +
                    std::to_string(settings.body_limit_bytes_) +
                    "\nSecRequestBodyNoFilesLimit " + std::to_string(settings.body_limit_bytes_) +
                    "\nSecRequestBodyLimitAction Reject\nSecRequestBodyJsonDepthLimit 64\nSecXmlExternalEntity Off\n"
                    "SecPcreMatchLimit 100000\nSecPcreMatchLimitRecursion 100000\n"
                    "SecArgumentSeparator &\nSecCookieFormat 0\n"
                    "SecArgumentsLimit 1000\n"
                    "SecUploadKeepFiles Off\nSecTmpSaveUploadedFiles Off\n"
                    "SecUnicodeMapFile " +
                    directory +
                    "/unicode.mapping 20127\n"
                    "SecDefaultAction \"phase:1,log,auditlog,pass\"\n"
                    "SecDefaultAction \"phase:2,log,auditlog,pass\"\n"
                    "SecRule REQUEST_HEADERS:Content-Type \"^(?:application(?:/soap\\+|/)|text/)xml\" "
                    "\"id:200001,phase:1,pass,nolog,ctl:requestBodyProcessor=XML\"\n"
                    "SecRule REQUEST_HEADERS:Content-Type \"^application/(?:[a-z0-9.-]+\\+)?json\" "
                    "\"id:200002,phase:1,pass,nolog,ctl:requestBodyProcessor=JSON\"\n"
                    "SecRule REQBODY_ERROR \"!@eq 0\" \"id:200003,phase:2,deny,status:400,log\"\n"
                    "SecRule MULTIPART_STRICT_ERROR \"!@eq 0\" \"id:200004,phase:2,deny,status:400,log\"\n"
                    "SecAction \"id:200010,phase:1,pass,nolog,setvar:tx.crs_setup_version=4250,"
                    "setvar:tx.blocking_paranoia_level=" +
                    std::to_string(settings.paranoia_level_) +
                    ",setvar:tx.detection_paranoia_level=" + std::to_string(settings.paranoia_level_) +
                    ",setvar:tx.inbound_anomaly_score_threshold=" + std::to_string(settings.inbound_threshold_) + "\"\n");
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        // Inbound CRS only. Response-body buffering is intentionally not part of this detector.
        if (name.starts_with("REQUEST-") && entry.path().extension() == ".conf") {
            if (!installed.contains(name)) {
                throw std::runtime_error("unexpected OWASP CRS rule file");
            }
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    if (files.size() < 15) {
        throw std::runtime_error("OWASP CRS installation is incomplete");
    }
    for (const auto& file : files) {
        if (rules.loadFromUri(file.generic_string().c_str()) < 0) {
            throw std::runtime_error("cannot load OWASP CRS: " + rules.getParserError());
        }
    }
    // The shipped CRS's optional persistent-collection initialization is never executable.
    // Plugins and external SecLang are not accepted by this adapter.
    load(rules, "SecRuleRemoveById 901320\n");
    for (const auto id : settings.excluded_rules_) {
        if (id < 910000 || id > 949999) {
            throw std::invalid_argument("invalid OWASP CRS exclusion");
        }
        load(rules, "SecRuleRemoveById " + std::to_string(id) + "\n");
    }
}

managed_security::~managed_security() = default;

managed_security_result managed_security::inspect(std::string_view client_ip,
    std::string_view method, std::string_view uri, bool secure,
    const std::vector<std::pair<std::string_view, std::string_view>>& headers,
    std::string_view body) const {
    managed_security_result result;
    for (const auto& [name, value] : headers) {
        if ((flexedge::route_policy::ascii_equal(name, "Authorization") || flexedge::route_policy::ascii_equal(name, "Host")) &&
            value.find_first_not_of(" \t") == std::string_view::npos) {
            result.status_ = 400;
            return result;
        }
    }
    if (body.size() > state_->body_limit_) {
        result.status_ = 413;
        return result;
    }
    // A fresh transaction for every request, including concurrent requests on one Worker.
    // The caller pins the immutable policy for this synchronous evaluation.
    modsecurity::Transaction transaction(&state_->engine_->value_, &state_->rules_, &result);
    const auto check = [&](int code) {
        if (intervention(transaction, result)) {
            return false;
        }
        if (code <= 0) {
            result.status_ = 503;
            result.engine_error_ = true;
            return false;
        }
        return true;
    };
    const std::string ip(client_ip), target(uri), verb(method);
    if (check(transaction.processConnection(ip.c_str(), 0, "127.0.0.1", secure ? 443 : 80)) &&
        check(transaction.processURI(target.c_str(), verb.c_str(), "1.1"))) {
        for (const auto& [name, value] : headers) {
            if (!check(transaction.addRequestHeader(reinterpret_cast<const unsigned char*>(name.data()), name.size(),
                    reinterpret_cast<const unsigned char*>(value.data()), value.size()))) {
                return result;
            }
        }
        if (check(transaction.processRequestHeaders()) &&
            check(transaction.appendRequestBody(reinterpret_cast<const unsigned char*>(body.data()), body.size()))) {
            (void)check(transaction.processRequestBody());
        }
    }
    if (const auto score = !result.engine_error_ ? transaction.m_collections.m_tx_collection->resolveFirst("blocking_inbound_anomaly_score") : nullptr) {
        std::uint32_t value{};
        const auto parsed = std::from_chars(score->data(), score->data() + score->size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == score->data() + score->size()) {
            result.anomaly_score_ = value;
        }
    }
    transaction.processLogging();
    return result;
}

}  // namespace flexedge::node
