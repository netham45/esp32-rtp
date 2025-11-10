#include "rtp/live555_bridge.h"

#include <cstring>
#include <memory>
#include <new>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern "C" {
#include "lwip/inet.h"
#include "lwip/sockets.h"
}

#include "BasicUsageEnvironment.hh"
#include "Groupsock.hh"
#include "GroupsockHelper.hh"
#include "Media.hh"
#include "RTCP.hh"
#include "RTPSource.hh"

class BridgeRTPSource;
class BridgeRTPSink;
class CaptureGroupsock;

struct live555_bridge_receiver {
    UsageEnvironment *env;
    Groupsock *rtp_socket;
    CaptureGroupsock *rtcp_socket;
    BridgeRTPSource *rtp_source;
    RTCPInstance *rtcp_instance;
    SemaphoreHandle_t lock;
    uint32_t sample_rate;
    uint8_t payload_type;
    uint32_t local_ssrc;
    uint8_t pending_rr_buf[1500];
    size_t pending_rr_len;
    bool pending_rr_ready;

    void capture_rr_packet(const unsigned char *data, size_t len) {
        size_t copy = len > sizeof(pending_rr_buf) ? sizeof(pending_rr_buf) : len;
        memcpy(pending_rr_buf, data, copy);
        pending_rr_len = copy;
        pending_rr_ready = true;
    }
};

struct live555_bridge_sender {
    UsageEnvironment *env;
    Groupsock *rtp_socket;
    Groupsock *rtcp_socket;
    class BridgeRTPSink *rtp_sink;
    RTCPInstance *rtcp_instance;
    SemaphoreHandle_t lock;
    uint32_t sample_rate;
    uint16_t initial_seq;
    uint32_t initial_timestamp;
};

class BridgeRTPSource final : public RTPSource {
public:
    BridgeRTPSource(UsageEnvironment &env,
                    Groupsock *rtpGs,
                    unsigned char payloadType,
                    u_int32_t timestampFrequency)
        : RTPSource(env, rtpGs, payloadType, timestampFrequency) {}

protected:
    void setPacketReorderingThresholdTime(unsigned uSeconds) override {
        reorderThresholdUS_ = uSeconds;
    }

    void doGetNextFrame() override {
        fFrameSize = 0;
        fNumTruncatedBytes = 0;
        fPresentationTime = {0, 0};
        fDurationInMicroseconds = 0;
        FramedSource::afterGetting(this);
    }

private:
    unsigned reorderThresholdUS_{0};
};

class BridgeRTPSink final : public RTPSink {
public:
    BridgeRTPSink(UsageEnvironment &env,
                  Groupsock *rtpGs,
                  unsigned char payloadType,
                  u_int32_t timestampFrequency,
                  unsigned numChannels,
                  const char *payloadName)
        : RTPSink(env, rtpGs, payloadType, timestampFrequency, payloadName, numChannels) {}

    void recordPacket(u_int32_t rtpTimestamp, size_t payloadLen, u_int16_t seq) {
        fCurrentTimestamp = rtpTimestamp;
        fSeqNo = seq;
        ++fPacketCount;
        fOctetCount += static_cast<unsigned>(payloadLen);
        fTotalOctetCount += static_cast<unsigned>(payloadLen + 12U);
    }

protected:
    Boolean continuePlaying() override {
        return False;
    }
};

class CaptureGroupsock final : public Groupsock {
public:
    CaptureGroupsock(UsageEnvironment& env,
                     struct sockaddr_storage const& groupAddr,
                     Port port,
                     u_int8_t ttl,
                     live555_bridge_receiver *owner)
        : Groupsock(env, groupAddr, port, ttl), fOwner(owner) {}

    Boolean output(UsageEnvironment& env, unsigned char* buffer, unsigned bufferSize) override {
        (void)env;
        if (fOwner) {
            fOwner->capture_rr_packet(buffer, bufferSize);
        }
        return True;
    }

private:
    live555_bridge_receiver *fOwner;
};

namespace {

static const char *TAG = "live555_bridge";
static constexpr uint32_t kDefaultRtcpBwBps = 128000U;

class Live555EnvHolder {
public:
    static Live555EnvHolder &instance() {
        static Live555EnvHolder holder;
        return holder;
    }

    UsageEnvironment &env() { return *env_; }

private:
    Live555EnvHolder() {
        scheduler_ = BasicTaskScheduler::createNew();
        env_ = BasicUsageEnvironment::createNew(*scheduler_);
    }

    ~Live555EnvHolder() {
        if (env_ != nullptr) {
            env_->reclaim();
            env_ = nullptr;
        }
        delete scheduler_;
        scheduler_ = nullptr;
    }

    TaskScheduler *scheduler_{nullptr};
    UsageEnvironment *env_{nullptr};
};

struct sockaddr_storage make_any_address() {
    struct sockaddr_storage storage {};
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(0);
    memcpy(&storage, &addr, sizeof(addr));
    return storage;
}

struct sockaddr_storage make_ipv4_address(uint32_t addr_be, uint16_t port_host) {
    struct sockaddr_storage storage {};
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = addr_be;
    addr.sin_port = htons(port_host);
    memcpy(&storage, &addr, sizeof(addr));
    return storage;
}

bool take_lock(SemaphoreHandle_t lock) {
    if (lock == nullptr) {
        return false;
    }
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    return true;
}

void give_lock(SemaphoreHandle_t lock) {
    if (lock != nullptr) {
        xSemaphoreGive(lock);
    }
}

} // namespace

esp_err_t live555_bridge_receiver_create(const live555_bridge_receiver_config_t *config,
                                         live555_bridge_receiver_t **out_handle) {
    if (!config || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = nullptr;
    auto &env = Live555EnvHolder::instance().env();

    std::unique_ptr<live555_bridge_receiver_t> ctx(new (std::nothrow) live555_bridge_receiver_t);
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    ctx->env = &env;
    ctx->sample_rate = (config->sample_rate == 0U) ? 48000U : config->sample_rate;
    ctx->payload_type = config->payload_type;
    ctx->local_ssrc = config->local_ssrc;
    ctx->lock = xSemaphoreCreateMutex();
    ctx->pending_rr_len = 0;
    ctx->pending_rr_ready = false;
    if (!ctx->lock) {
        ESP_LOGE(TAG, "Failed to create receiver mutex");
        return ESP_ERR_NO_MEM;
    }

    auto any_addr = make_any_address();
    ctx->rtp_socket = new (std::nothrow) Groupsock(env, any_addr, Port(0), 1);
    ctx->rtcp_socket = new (std::nothrow) CaptureGroupsock(env, any_addr, Port(0), 1, ctx.get());
    if (!ctx->rtp_socket || !ctx->rtcp_socket) {
        ESP_LOGE(TAG, "Failed to create Groupsock for receiver");
        live555_bridge_receiver_destroy(ctx.release());
        return ESP_ERR_NO_MEM;
    }

    ctx->rtp_source = new (std::nothrow) BridgeRTPSource(env,
                                                         ctx->rtp_socket,
                                                         ctx->payload_type,
                                                         ctx->sample_rate);
    if (!ctx->rtp_source) {
        ESP_LOGE(TAG, "Failed to allocate BridgeRTPSource");
        live555_bridge_receiver_destroy(ctx.release());
        return ESP_ERR_NO_MEM;
    }

    uint32_t bw = config->rtcp_bandwidth_bps ? config->rtcp_bandwidth_bps : kDefaultRtcpBwBps;
    const char *cname = config->cname ? config->cname : "esp32-rtp";
    const unsigned char *cname_bytes = reinterpret_cast<const unsigned char *>(cname);

    ctx->rtcp_instance = RTCPInstance::createNew(env,
                                                 ctx->rtcp_socket,
                                                 bw,
                                                 cname_bytes,
                                                 nullptr,
                                                 ctx->rtp_source,
                                                 False);
    if (!ctx->rtcp_instance) {
        ESP_LOGE(TAG, "Failed to create RTCPInstance for receiver");
        live555_bridge_receiver_destroy(ctx.release());
        return ESP_ERR_NO_MEM;
    }

    *out_handle = ctx.release();
    return ESP_OK;
}

void live555_bridge_receiver_destroy(live555_bridge_receiver_t *handle) {
    if (!handle) {
        return;
    }

    if (handle->rtcp_instance) {
        Medium::close(handle->rtcp_instance);
        handle->rtcp_instance = nullptr;
    }
    if (handle->rtp_source) {
        Medium::close(handle->rtp_source);
        handle->rtp_source = nullptr;
    }
    delete handle->rtp_socket;
    delete handle->rtcp_socket;
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
        handle->lock = nullptr;
    }
    delete handle;
}

esp_err_t live555_bridge_receiver_note_rtp(live555_bridge_receiver_t *handle,
                                           uint32_t ssrc,
                                           uint16_t seq,
                                           uint32_t rtp_ts,
                                           size_t payload_len,
                                           bool marker) {
    (void)marker;
    if (!handle || !handle->rtp_source) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(handle->lock)) {
        return ESP_ERR_TIMEOUT;
    }

    RTPReceptionStatsDB &db = handle->rtp_source->receptionStatsDB();
    struct timeval pres_time {0, 0};
    Boolean synced = False;
    db.noteIncomingPacket(ssrc,
                          seq,
                          rtp_ts,
                          handle->sample_rate,
                          True /*useForJitterCalculation*/,
                          pres_time,
                          synced,
                          static_cast<unsigned>(payload_len));
    give_lock(handle->lock);
    return ESP_OK;
}

esp_err_t live555_bridge_receiver_note_rtcp(live555_bridge_receiver_t *handle,
                                            const uint8_t *packet,
                                            size_t len) {
    if (!handle || !packet || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->rtcp_instance) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!take_lock(handle->lock)) {
        return ESP_ERR_TIMEOUT;
    }

    struct sockaddr_storage from_addr = make_any_address();
    handle->rtcp_instance->injectReport(packet, static_cast<unsigned>(len), from_addr);
    give_lock(handle->lock);
    return ESP_OK;
}

bool live555_bridge_receiver_get_stats(live555_bridge_receiver_t *handle,
                                       uint32_t ssrc,
                                       live555_bridge_rx_stats_t *out_stats) {
    if (!handle || !out_stats) {
        return false;
    }
    if (!handle->rtp_source) {
        return false;
    }
    if (!take_lock(handle->lock)) {
        return false;
    }

    RTPReceptionStatsDB &db = handle->rtp_source->receptionStatsDB();
    RTPReceptionStats *stats = db.lookup(ssrc);
    if (!stats) {
        give_lock(handle->lock);
        return false;
    }

    out_stats->ext_max_seq = stats->highestExtSeqNumReceived();
    out_stats->base_seq = stats->baseExtSeqNumReceived();
    out_stats->highest_seq = stats->highestExtSeqNumReceived();
    unsigned expected = stats->totNumPacketsExpected();
    unsigned received = stats->totNumPacketsReceived();
    out_stats->cumulative_lost = static_cast<int32_t>(expected) - static_cast<int32_t>(received);
    out_stats->jitter_ticks = static_cast<double>(stats->jitter());
    out_stats->last_sr_ntp_msw = stats->lastReceivedSR_NTPmsw();
    out_stats->last_sr_ntp_lsw = stats->lastReceivedSR_NTPlsw();
    const struct timeval &last_sr_time = stats->lastReceivedSR_time();
    out_stats->last_sr_wallclock_us =
        (static_cast<uint64_t>(last_sr_time.tv_sec) * 1000000ULL) +
        static_cast<uint64_t>(last_sr_time.tv_usec);
    out_stats->has_last_sr = (out_stats->last_sr_ntp_msw != 0) || (out_stats->last_sr_ntp_lsw != 0);

    give_lock(handle->lock);
    return true;
}

esp_err_t live555_bridge_receiver_build_rr(live555_bridge_receiver_t *handle,
                                           uint8_t *buffer,
                                           size_t buffer_size,
                                           size_t *packet_size) {
    if (!handle || !buffer || !packet_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->rtcp_instance) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!take_lock(handle->lock)) {
        return ESP_ERR_TIMEOUT;
    }
    handle->pending_rr_ready = false;
    handle->pending_rr_len = 0;
    handle->rtcp_instance->sendReport();

    esp_err_t rc = ESP_ERR_INVALID_STATE;
    if (handle->pending_rr_ready && handle->pending_rr_len > 0) {
        if (handle->pending_rr_len <= buffer_size) {
            memcpy(buffer, handle->pending_rr_buf, handle->pending_rr_len);
            *packet_size = handle->pending_rr_len;
            handle->pending_rr_ready = false;
            handle->pending_rr_len = 0;
            rc = ESP_OK;
        } else {
            rc = ESP_ERR_NO_MEM;
        }
    } else {
        rc = ESP_ERR_NOT_FINISHED;
    }
    give_lock(handle->lock);
    return rc;
}

esp_err_t live555_bridge_sender_create(const live555_bridge_sender_config_t *config,
                                       live555_bridge_sender_t **out_handle) {
    if (!config || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = nullptr;
    auto &env = Live555EnvHolder::instance().env();

    std::unique_ptr<live555_bridge_sender_t> ctx(new (std::nothrow) live555_bridge_sender_t);
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    ctx->env = &env;
    ctx->sample_rate = (config->sample_rate == 0U) ? 48000U : config->sample_rate;
    ctx->lock = xSemaphoreCreateMutex();
    if (!ctx->lock) {
        ESP_LOGE(TAG, "Failed to create sender mutex");
        return ESP_ERR_NO_MEM;
    }

    uint8_t ttl = (config->ttl == 0U) ? 1U : config->ttl;
    ctx->rtp_socket = new (std::nothrow) Groupsock(env, make_any_address(), Port(0), ttl);
    ctx->rtcp_socket = new (std::nothrow) Groupsock(env, make_any_address(), Port(0), ttl);
    if (!ctx->rtp_socket || !ctx->rtcp_socket) {
        ESP_LOGE(TAG, "Failed to create sender Groupsock");
        live555_bridge_sender_destroy(ctx.release());
        return ESP_ERR_NO_MEM;
    }

    const char *payload_name = config->payload_name ? config->payload_name : "L16";
    unsigned channels = config->channels ? config->channels : 2U;
    ctx->rtp_sink = new (std::nothrow) BridgeRTPSink(env,
                                                     ctx->rtp_socket,
                                                     config->payload_type,
                                                     ctx->sample_rate,
                                                     channels,
                                                     payload_name);
    if (!ctx->rtp_sink) {
        ESP_LOGE(TAG, "Failed to create BridgeRTPSink");
        live555_bridge_sender_destroy(ctx.release());
        return ESP_ERR_NO_MEM;
    }

    ctx->initial_seq = ctx->rtp_sink->currentSeqNo();
    ctx->initial_timestamp = ctx->rtp_sink->presetNextTimestamp();

    uint32_t bw = config->rtcp_bandwidth_bps ? config->rtcp_bandwidth_bps : kDefaultRtcpBwBps;
    const char *cname = config->cname ? config->cname : "esp32-rtp-send";
    const unsigned char *cname_bytes = reinterpret_cast<const unsigned char *>(cname);

    ctx->rtcp_instance = RTCPInstance::createNew(env,
                                                 ctx->rtcp_socket,
                                                 bw,
                                                 cname_bytes,
                                                 ctx->rtp_sink,
                                                 nullptr,
                                                 False);
    if (!ctx->rtcp_instance) {
        ESP_LOGE(TAG, "Failed to create RTCPInstance for sender");
        live555_bridge_sender_destroy(ctx.release());
        return ESP_ERR_NO_MEM;
    }

    esp_err_t dest_rc = live555_bridge_sender_set_destination(ctx.get(),
                                                              config->dest_ipv4,
                                                              config->dest_rtcp_port,
                                                              ttl);
    if (dest_rc != ESP_OK) {
        live555_bridge_sender_destroy(ctx.release());
        return dest_rc;
    }

    *out_handle = ctx.release();
    return ESP_OK;
}

void live555_bridge_sender_destroy(live555_bridge_sender_t *handle) {
    if (!handle) {
        return;
    }
    if (handle->rtcp_instance) {
        Medium::close(handle->rtcp_instance);
        handle->rtcp_instance = nullptr;
    }
    if (handle->rtp_sink) {
        Medium::close(handle->rtp_sink);
        handle->rtp_sink = nullptr;
    }
    delete handle->rtp_socket;
    delete handle->rtcp_socket;
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
        handle->lock = nullptr;
    }
    delete handle;
}

uint32_t live555_bridge_sender_ssrc(live555_bridge_sender_t *handle) {
    if (!handle || !handle->rtp_sink) {
        return 0;
    }
    return handle->rtp_sink->SSRC();
}

uint16_t live555_bridge_sender_initial_seq(live555_bridge_sender_t *handle) {
    if (!handle) {
        return 0;
    }
    return handle->initial_seq;
}

uint32_t live555_bridge_sender_initial_timestamp(live555_bridge_sender_t *handle) {
    if (!handle) {
        return 0;
    }
    return handle->initial_timestamp;
}

esp_err_t live555_bridge_sender_note_rtp(live555_bridge_sender_t *handle,
                                         uint32_t rtp_timestamp,
                                         size_t payload_len,
                                         uint16_t seq) {
    if (!handle || !handle->rtp_sink) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(handle->lock)) {
        return ESP_ERR_TIMEOUT;
    }
    handle->rtp_sink->recordPacket(rtp_timestamp, payload_len, seq);
    give_lock(handle->lock);
    return ESP_OK;
}

esp_err_t live555_bridge_sender_send_sr(live555_bridge_sender_t *handle) {
    if (!handle || !handle->rtcp_instance) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(handle->lock)) {
        return ESP_ERR_TIMEOUT;
    }
    handle->rtcp_instance->sendReport();
    give_lock(handle->lock);
    return ESP_OK;
}

esp_err_t live555_bridge_sender_set_destination(live555_bridge_sender_t *handle,
                                                uint32_t dest_ipv4,
                                                uint16_t dest_rtcp_port,
                                                uint8_t ttl) {
    if (!handle || !handle->rtcp_socket) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(handle->lock)) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t eff_ttl = ttl ? ttl : 1U;
    struct sockaddr_storage dest_addr = make_ipv4_address(dest_ipv4, dest_rtcp_port);
    handle->rtcp_socket->changeDestinationParameters(dest_addr,
                                                     Port(dest_rtcp_port),
                                                     eff_ttl);
    give_lock(handle->lock);
    return ESP_OK;
}

void live555_bridge_set_ipv4(uint32_t addr_be) {
    if (addr_be == 0) {
        ReceivingInterfaceAddr = INADDR_ANY;
        SendingInterfaceAddr = INADDR_ANY;
        return;
    }
    ReceivingInterfaceAddr = addr_be;
    SendingInterfaceAddr = addr_be;
}
