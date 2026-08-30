// verify_worker.cpp — Gate.io login verification via the existing
// GateRestClient (libcurl + HMAC-SHA512 signing). Reuses pulse::exchange
// exactly as the engine does, so the check validates the real production path.

#include "VerifyWorker.hpp"

#include "CredentialsStore.hpp"

#include "exchange/GateRestClient.hpp"
#include "exchange/gate_auth.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

namespace pulse::ui
{

namespace
{


// Read a numeric field that Gate may encode either as a decimal string or a
// JSON number. json::value(key, 0.0) is NOT safe here — it throws
// type_error.302 when the stored value is a string (which Gate does for all
// monetary amounts).
double getDoubleField(const nlohmann::json &obj, const char *key)
{
    if (obj.contains(key))
    {
        return VerifyWorker::jsonToDouble(obj[key]);
    }
    return 0.0;
}

QString describeFailure(const QString &what, const pulse::PulseError &err)
{
    const QString message = QString::fromStdString(err.message);
    // Transport-level failures (curl error N, Timeout, SSL) mean Gate.io is
    // unreachable — a connectivity/proxy problem, not a credentials problem.
    if (message.contains("curl error") || message.contains("Timeout")
        || message.contains("SSL") || message.contains("connect")
        || message.contains("Could not resolve"))
    {
        return what + QStringLiteral("：无法连接 Gate.io（网络不通或代理未生效）。\n"
                                     "请确认代理软件已启动，且 .env 中的 HTTPS_PROXY "
                                     "指向可用代理；或当前网络可直连 api.gateio.ws。");
    }
    // The two credential-failure classes worth telling the user apart.
    if (message.contains("401"))
    {
        return what + QStringLiteral("：API Key 或 Secret 无效（HTTP 401），"
                                     "请检查凭证和 Gate 账号的 API 权限。");
    }
    if (message.contains("403"))
    {
        return what + QStringLiteral("：权限不足（HTTP 403），"
                                     "该 API Key 未开通此接口权限。");
    }
    return what + QStringLiteral("：") + message;
}

// ---------------------------------------------------------------------------
// Write probe — DELETE a random, non-existent order id. Read-only keys are
// rejected before the id is looked up (403 INVALID_PERMISSION); read-write
// keys answer with an order-not-found error. No real order is ever touched.
// ---------------------------------------------------------------------------
VerifyWorker::WriteProbe probeWrite(pulse::exchange::GateRestClient &client,
                                    const std::string &method,
                                    const std::string &path)
{
    const auto res = client.request(method, path);
    if (pulse::ok(res))
    {
        return VerifyWorker::WriteProbe::ReadWrite;
    }
    return VerifyWorker::classifyWriteProbe(QString::fromStdString(pulse::error(res).message),
                                            false);
}

// A synthetic order id that cannot collide with a real one (exchange-assigned
// sequential ids never look like this).
std::string makeProbeOrderId()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "permprobe" + std::to_string(now % 1000000000ULL);
}

} // namespace

// ---------------------------------------------------------------------------
// Config building — same defaults the engine's loadConfig uses on mainnet.
// Shared by the login verification and the engine polling (MainWindow).
// ---------------------------------------------------------------------------
pulse::ExchangeConfig VerifyWorker::makeExchangeConfig(const QString &apiKey,
                                                       const QString &apiSecret)
{
    pulse::ExchangeConfig cfg;
    cfg.apiKey = apiKey.toStdString();
    cfg.apiSecret = apiSecret.toStdString();
    cfg.restBaseUrl = "https://api.gateio.ws";

    // Proxy resolution: process environment first (run.sh sources .env), then
    // fall back to the .env file itself — the GUI is often launched directly
    // (double-click / VS) without the run.sh environment, and direct
    // connections to api.gateio.ws are unreliable from CN networks.
    QString proxy;
    const char *envProxy = std::getenv("HTTPS_PROXY");
    if (nullptr == envProxy || '\0' == *envProxy)
    {
        envProxy = std::getenv("HTTP_PROXY");
    }
    if (nullptr != envProxy && '\0' != *envProxy)
    {
        proxy = QString::fromUtf8(envProxy);
    }
    else
    {
        const QMap<QString, QString> envVars =
            CredentialsStore::loadAllEnvVars(CredentialsStore::resolveEnvPath());
        proxy = envVars.value(QLatin1String("HTTPS_PROXY"));
        if (proxy.isEmpty())
        {
            proxy = envVars.value(QLatin1String("HTTP_PROXY"));
        }
    }
    cfg.proxyUrl = proxy.toStdString();
    return cfg;
}

VerifyResult VerifyWorker::run(const QString &apiKey, const QString &apiSecret)
{
    VerifyResult result;
    result.maskedKey = CredentialsStore::maskKey(apiKey);
    const QString keyTrimmed = apiKey.trimmed();
    const QString secretTrimmed = apiSecret.trimmed();
    if (keyTrimmed.isEmpty() || secretTrimmed.isEmpty())
    {
        result.error = QStringLiteral("API Key 和 API Secret 均不能为空。");
        return result;
    }

    const pulse::ExchangeConfig cfg = makeExchangeConfig(keyTrimmed, secretTrimmed);

    try
    {
        // -------------------------------------------------------------------
        // 1. Spot account balance — the credential validity probe.
        // -------------------------------------------------------------------
        pulse::exchange::GateRestClient spot(cfg, pulse::MarketType::Spot);
        const auto accounts = spot.getSpotAccounts();
        if (false == pulse::ok(accounts))
        {
            result.error = describeFailure(QStringLiteral("余额查询失败"), pulse::error(accounts));
            return result;
        }
        for (const auto &acc : pulse::value(accounts))
        {
            if (false == acc.is_object())
            {
                continue;
            }
            if ("USDT" != acc.value("currency", ""))
            {
                continue;
            }
            result.spotUsdt = getDoubleField(acc, "available");
            break;
        }

        // -------------------------------------------------------------------
        // 2. CFD (TradFi) equity — optional, permission-gated.
        // -------------------------------------------------------------------
        pulse::exchange::GateRestClient cfd(cfg, pulse::MarketType::Cfd);
        const auto assets = cfd.getCfdAssets();
        if (pulse::ok(assets))
        {
            const nlohmann::json *obj = unwrapData(firstObject(pulse::value(assets)));
            if (nullptr != obj)
            {
                result.cfdAvailable = true;
                result.cfdEquity = getDoubleField(*obj, "equity");
            }
        }

        // -------------------------------------------------------------------
        // 3. XAUUSD ticker — public endpoint, current gold price.
        // -------------------------------------------------------------------
        const auto ticker = cfd.getCfdTicker("XAUUSD");
        if (pulse::ok(ticker))
        {
            const nlohmann::json *obj = unwrapData(firstObject(pulse::value(ticker)));
            if (nullptr != obj)
            {
                result.goldAvailable = true;
                result.goldPrice = getDoubleField(*obj, "last_price");
            }
        }

        // -------------------------------------------------------------------
        // 4. Permissions — read/write split per product group.
        // -------------------------------------------------------------------
        // Preferred source: GET /account/main_keys (authoritative perms
        // array, added in API v4.106.4). Fallback: safe write probes.
        const auto mainKeys = spot.request("GET", "/api/v4/account/main_keys");
        if (pulse::ok(mainKeys))
        {
            result.permissions = VerifyWorker::parsePermissions(pulse::value(mainKeys));

            // TradFi (CFD) is not part of the main_keys perms list — overlay
            // the read observation from step 2 when the endpoint answered.
            bool sawCfd = false;
            for (const auto &p : result.permissions)
            {
                if ("tradfi" == p.name)
                {
                    sawCfd = true;
                    break;
                }
            }
            if (result.cfdAvailable && false == sawCfd)
            {
                PermissionInfo cfdPerm;
                cfdPerm.name = QStringLiteral("tradfi");
                cfdPerm.available = true;  // read verified by step 2
                cfdPerm.known = false;     // read/write split unknown from here
                result.permissions.push_back(std::move(cfdPerm));
            }
        }
        else
        {
            // --- Fallback: read probes + write probes ---------------------
            // Spot read was verified in step 1 (it gates the whole login).
            const auto spotProbe = probeWrite(
                spot, "DELETE", "/api/v4/spot/orders/" + makeProbeOrderId());
            PermissionInfo spotPerm;
            spotPerm.name = QStringLiteral("spot");
            spotPerm.available = true;
            spotPerm.known = (WriteProbe::Unknown != spotProbe);
            spotPerm.readOnly = (WriteProbe::ReadOnly == spotProbe);
            result.permissions.push_back(std::move(spotPerm));

            // Futures: read probe, then write probe when readable.
            pulse::exchange::GateRestClient futures(cfg, pulse::MarketType::Futures);
            const auto futuresAccounts = futures.getFuturesAccounts();
            PermissionInfo futuresPerm;
            futuresPerm.name = QStringLiteral("futures");
            futuresPerm.available = pulse::ok(futuresAccounts);
            if (futuresPerm.available)
            {
                const auto futuresProbe = probeWrite(
                    futures, "DELETE", "/api/v4/futures/usdt/orders/" + makeProbeOrderId());
                futuresPerm.known = (WriteProbe::Unknown != futuresProbe);
                futuresPerm.readOnly = (WriteProbe::ReadOnly == futuresProbe);
            }
            result.permissions.push_back(std::move(futuresPerm));

            // CFD: read observation from step 2, then write probe.
            PermissionInfo cfdPerm;
            cfdPerm.name = QStringLiteral("tradfi");
            cfdPerm.available = result.cfdAvailable;
            if (cfdPerm.available)
            {
                const auto cfdProbe = probeWrite(
                    cfd, "DELETE", "/api/v4/tradfi/orders/" + makeProbeOrderId());
                cfdPerm.known = (WriteProbe::Unknown != cfdProbe);
                cfdPerm.readOnly = (WriteProbe::ReadOnly == cfdProbe);
            }
            result.permissions.push_back(std::move(cfdPerm));
        }

        result.ok = true;
    }
    catch (const std::exception &e)
    {
        result.error = QStringLiteral("校验异常：%1").arg(QString::fromUtf8(e.what()));
    }
    return result;
}

double VerifyWorker::jsonToDouble(const nlohmann::json &j)
{
    if (j.is_string())
    {
        try
        {
            return std::stod(j.get<std::string>());
        }
        catch (const std::exception &)
        {
            return 0.0;
        }
    }
    if (j.is_number())
    {
        return j.get<double>();
    }
    return 0.0;
}

const nlohmann::json *VerifyWorker::firstObject(const nlohmann::json &j)
{
    if (j.is_object())
    {
        return &j;
    }
    if (j.is_array() && false == j.empty() && j.front().is_object())
    {
        return &j.front();
    }
    return nullptr;
}

const nlohmann::json *VerifyWorker::unwrapData(const nlohmann::json *obj)
{
    if (nullptr == obj)
    {
        return nullptr;
    }
    if (obj->is_object() && obj->contains("data") && (*obj)["data"].is_object())
    {
        return &(*obj)["data"];
    }
    return obj;
}

std::vector<PermissionInfo> VerifyWorker::parsePermissions(const nlohmann::json &mainKeysJson)
{
    std::vector<PermissionInfo> out;

    // Collect candidate records: the payload is either a single key record
    // (object) or an array of key records.
    std::vector<nlohmann::json> records;
    if (mainKeysJson.is_array())
    {
        records = mainKeysJson.get<std::vector<nlohmann::json>>();
    }
    else if (mainKeysJson.is_object())
    {
        records.push_back(mainKeysJson);
    }
    else
    {
        return out;
    }

    // Each record may be one of two shapes:
    //   a) {"key": ..., "perms": [{"name": "spot", "read_only": false}, ...]}
    //   b) a bare perms entry {"name": ..., "read_only": ...}
    for (const auto &record : records)
    {
        if (false == record.is_object())
        {
            continue;
        }
        if (record.contains("perms") && record["perms"].is_array())
        {
            for (const auto &p : record["perms"])
            {
                if (false == p.is_object() || false == p.contains("name") || false == p["name"].is_string())
                {
                    continue;
                }
                PermissionInfo info;
                info.name = QString::fromStdString(p["name"].get<std::string>());
                if (p.contains("read_only") && p["read_only"].is_boolean())
                {
                    info.readOnly = p["read_only"].get<bool>();
                    info.known = true;
                }
                info.available = true;
                out.push_back(std::move(info));
            }
        }
        else if (record.contains("name") && record["name"].is_string())
        {
            PermissionInfo info;
            info.name = QString::fromStdString(record["name"].get<std::string>());
            if (record.contains("read_only") && record["read_only"].is_boolean())
            {
                info.readOnly = record["read_only"].get<bool>();
                info.known = true;
            }
            info.available = true;
            out.push_back(std::move(info));
        }
    }
    return out;
}

VerifyWorker::WriteProbe VerifyWorker::classifyWriteProbe(const QString &errorMessage,
                                                          bool writeSucceeded)
{
    if (writeSucceeded)
    {
        return WriteProbe::ReadWrite;
    }
    const QString msg = errorMessage.toLower();
    // 1. Transport-level failures (DNS/TLS/timeout) — no verdict.
    if (msg.isEmpty()
        || msg.contains("timeout")
        || msg.contains("connect")
        || msg.contains("resolve"))
    {
        return WriteProbe::Unknown;
    }
    // 2. Auth/rate-limit anomalies — credentials were verified in step 1,
    //    so 401/429 here is not a permission verdict either.
    if (msg.contains("401") || msg.contains("429"))
    {
        return WriteProbe::Unknown;
    }
    // 3. 403 / INVALID_PERMISSION — a read-only key rejects the write.
    if (msg.contains("403") || msg.contains("invalid_permission") || msg.contains("forbidden"))
    {
        return WriteProbe::ReadOnly;
    }
    // 4. Anything else (ORDER_NOT_FOUND etc.) — the write channel is open.
    return WriteProbe::ReadWrite;
}

} // namespace pulse::ui
