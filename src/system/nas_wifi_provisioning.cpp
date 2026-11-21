#include "nas_wifi_provisioning.h"

#include <atomic>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
// ESP-IDF 6.1 的 mbedtls 4.x 已移除 legacy GCM/PKCS5 接口，改用 PSA Crypto。
#include "psa/crypto.h"
#include "storage_io.h"
#include "nas_catalog_service.h"
#include "wifi_service.h"

namespace {
static const char* TAG = "NAS配对";
static constexpr uint16_t PORT = 46322;
static constexpr size_t MAX_PAYLOAD = 700;
static constexpr TickType_t SESSION_TICKS = pdMS_TO_TICKS(180000);
static std::atomic<bool> active{false};
static char shown_code[7] = {};
static constexpr unsigned KDF_ROUNDS = 10000;
static constexpr unsigned MAX_ATTEMPTS = 3;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static constexpr char kPending[] = "/sdcard/System/nas_catalog.conf.pending";

struct Payload {
    uint8_t session_key[16] = {};
    uint8_t plaintext[MAX_PAYLOAD + 1] = {};
    uint8_t ciphertext[MAX_PAYLOAD] = {};
    uint8_t tag[16] = {};
    uint8_t nonce[12] = {};
    char config[900] = {};
};

// Payload contains the decrypted NAS credentials and an AES session key.
// Volatile byte writes ensure the sensitive data is not optimized away on
// release builds. The object has implicit lifetime in PSRAM-allocated storage.
static void secure_wipe_payload(Payload *ctx)
{
    if (ctx == nullptr) return;
    volatile uint8_t *bytes = reinterpret_cast<volatile uint8_t *>(ctx);
    for (size_t i = 0; i < sizeof(*ctx); ++i) {
        bytes[i] = 0U;
    }
}

static bool read_exact(int fd, void* buffer, size_t size) {
    uint8_t* out = static_cast<uint8_t*>(buffer);
    while (size != 0) {
        const int got = recv(fd, out, size, 0);
        if (got <= 0) return false;
        out += got; size -= static_cast<size_t>(got);
    }
    return true;
}
static bool send_exact(int fd, const void* buffer, size_t size) {
    const uint8_t* in = static_cast<const uint8_t*>(buffer);
    while (size != 0) {
        const int sent = send(fd, in, size, 0);
        if (sent <= 0) return false;
        in += sent; size -= static_cast<size_t>(sent);
    }
    return true;
}
static bool crypto(const uint8_t* key, bool encrypt, const uint8_t* nonce, const uint8_t* in, size_t len,
                   const uint8_t* aad, size_t aad_len, uint8_t* out, uint8_t* tag) {
    if (key == nullptr || nonce == nullptr || in == nullptr || out == nullptr || tag == nullptr ||
        len == 0 || len > MAX_PAYLOAD) {
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes,
        encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);

    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_import_key(&attributes, key, 16, &key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        return false;
    }

    psa_aead_operation_t op = PSA_AEAD_OPERATION_INIT;
    status = encrypt ? psa_aead_encrypt_setup(&op, key_id, PSA_ALG_GCM)
                     : psa_aead_decrypt_setup(&op, key_id, PSA_ALG_GCM);
    bool ok = status == PSA_SUCCESS;

    if (ok) {
        status = psa_aead_set_nonce(&op, nonce, 12);
        ok = status == PSA_SUCCESS;
    }
    if (ok && aad_len != 0) {
        status = psa_aead_update_ad(&op, aad, aad_len);
        ok = status == PSA_SUCCESS;
    }

    // GCM 没有缓冲输出，update 一次即可输出全部密文/明文。
    size_t produced = 0;
    if (ok) {
        status = psa_aead_update(&op, in, len, out, len, &produced);
        ok = status == PSA_SUCCESS && produced == len;
    }

    uint8_t tail[16] = {};
    size_t tail_len = 0;
    if (ok) {
        if (encrypt) {
            size_t tag_len = 0;
            status = psa_aead_finish(&op, tail, sizeof(tail), &tail_len,
                tag, 16, &tag_len);
            ok = status == PSA_SUCCESS && tail_len == 0 && tag_len == 16;
        } else {
            status = psa_aead_verify(&op, tail, sizeof(tail), &tail_len, tag, 16);
            ok = status == PSA_SUCCESS && tail_len == 0;
        }
    }

    if (!ok) {
        (void)psa_aead_abort(&op);
    }
    (void)psa_destroy_key(key_id);
    return ok;
}
// FPN2: short PIN plus per-connection random salt. This slows offline guessing,
// but a 6-digit PIN is not equivalent to the original 128-bit display secret.
static bool derive_key(const char pin[7], const uint8_t salt[16], uint8_t out[16]) {
    if (pin == nullptr || salt == nullptr || out == nullptr) {
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }

    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(
        &op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    // PBKDF2 的输入顺序：迭代次数 -> salt -> password。
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_integer(
            &op, PSA_KEY_DERIVATION_INPUT_COST, KDF_ROUNDS);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(
            &op, PSA_KEY_DERIVATION_INPUT_SALT, salt, 16);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(
            &op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
            reinterpret_cast<const uint8_t*>(pin), 6);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&op, out, 16);
    }
    (void)psa_key_derivation_abort(&op);
    return status == PSA_SUCCESS;
}
static bool clean_field(const char* text, size_t max_len) {
    const size_t len = strlen(text);
    if (len == 0 || len >= max_len) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 32 || c == 127) return false;
    }
    return true;
}
static bool build_config(Payload* ctx, size_t size) {
    ctx->plaintext[size] = 0;
    char* music = reinterpret_cast<char*>(ctx->plaintext);
    char* delim1 = strchr(music, '\n');
    if (!delim1) return false;
    *delim1 = 0;
    char* user = delim1 + 1;
    char* delim2 = strchr(user, '\n');
    if (!delim2) return false;
    *delim2 = 0;
    char* password = delim2 + 1;
    if (strchr(password, '\n') || strchr(password, '\r')) return false;
    if (!clean_field(music, 210) || !clean_field(user, 64) || !clean_field(password, 96)) return false;
    if (strncmp(music, "http://", 7) != 0 || strlen(music) < 10) return false;
    // URL is a canonical, percent-encoded Music root, with no query or fragments.
    if (strstr(music, "..") || strchr(music, '?') || strchr(music, '#') ||
        strchr(music, '@') || strchr(music, '\\') || strchr(music, ' ') ||
        strchr(music, '=')) return false;
    size_t music_len = strlen(music);
    if (music[music_len - 1] == '/') music[--music_len] = 0;
    const int written = snprintf(ctx->config, sizeof(ctx->config),
        "base_url=%s/_FakePod/music-index\n"
        "track_url=%s/_FakePod/track\n"
        "music_url=%s\nusername=%s\npassword=%s\n",
        music, music, music, user, password);
    return written > 0 && static_cast<size_t>(written) < sizeof(ctx->config) &&
        music_len + sizeof("/_FakePod/music-index") <= 256;
}
static bool save_pending(const char* text) {
    StorageSdLockGuard sd_lock(pdMS_TO_TICKS(1500));
    if (!sd_lock.locked()) return false;
    const char* temp = "/sdcard/System/nas_catalog.conf.pending.tmp";
    FILE* f = fopen(temp, "wb");
    if (!f) return false;
    const size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len && fflush(f) == 0;
    if (ok) ok = fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok) {
        // FAT-backed VFS may not replace an existing target via rename.
        if (rename(temp, kPending) != 0) {
            ok = unlink(kPending) == 0 && rename(temp, kPending) == 0;
        }
    }
    if (!ok) unlink(temp);
    return ok;
}
static bool serve_once(int fd, Payload* ctx, TickType_t expires) {
    char pin[7] = {};
    portENTER_CRITICAL(&lock);
    memcpy(pin, shown_code, sizeof(pin));
    portEXIT_CRITICAL(&lock);
    if (!active.load() || pin[0] == 0) return false;
    // The device sends a random salt; the PIN and NAS credentials never travel in cleartext.
    uint8_t challenge[20] = {'F', 'P', 'C', '2'};
    esp_fill_random(challenge + 4, 16);
    if (!send_exact(fd, challenge, sizeof(challenge))) { memset(pin, 0, sizeof(pin)); return false; }
    const bool derived = derive_key(pin, challenge + 4, ctx->session_key);
    memset(pin, 0, sizeof(pin));
    if (!derived) return false;
    uint8_t header[6] = {};
    if (!read_exact(fd, header, sizeof(header)) || memcmp(header, "FPN2", 4) != 0) return false;
    const size_t len = static_cast<size_t>((header[4] << 8) | header[5]);
    if (len == 0 || len > MAX_PAYLOAD) return false;
    if (!read_exact(fd, ctx->nonce, 12) || !read_exact(fd, ctx->ciphertext, len) ||
        !read_exact(fd, ctx->tag, 16)) return false;
    static const uint8_t aad[] = "FakePod-NAS-Config-V2";
    bool accepted = crypto(ctx->session_key, false, ctx->nonce, ctx->ciphertext, len,
        aad, sizeof(aad) - 1, ctx->plaintext, ctx->tag);
    if (accepted) accepted = active.load() &&
        static_cast<int32_t>(xTaskGetTickCount() - expires) < 0 &&
        build_config(ctx, len) && save_pending(ctx->config);
    // Authenticate the ACK as well; client must verify it before showing success.
    uint8_t ack[4 + 12 + 1 + 16] = {'F', 'P', 'A', '2'};
    esp_fill_random(ack + 4, 12);
    const uint8_t result = accepted ? 0 : 1;
    static const uint8_t ack_aad[] = "FakePod-NAS-ACK-V2";
    uint8_t one[1] = {};
    if (!crypto(ctx->session_key, true, ack + 4, &result, 1, ack_aad, sizeof(ack_aad) - 1, one, ack + 17)) return false;
    ack[16] = one[0];
    (void)send_exact(fd, ack, sizeof(ack));
    if (accepted) ESP_LOGI(TAG, "收到加密NAS配置；已暂存，准备停止音乐并热切换NAS索引（不记录凭据）");
    else ESP_LOGW(TAG, "NAS配置解密或校验失败；旧配置未修改");
    return accepted;
}
static void pairing_task(void*) {
    const TickType_t expires = xTaskGetTickCount() + SESSION_TICKS;
    int server = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server >= 0) {
        int yes = 1;
        (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
            listen(server, 1) == 0) {
            unsigned attempts = 0;
            while (active.load() && static_cast<int32_t>(xTaskGetTickCount() - expires) < 0 &&
                   attempts < MAX_ATTEMPTS) {
                fd_set set;
                FD_ZERO(&set);
                FD_SET(server, &set);
                timeval timeout = {}; timeout.tv_usec = 300000;
                const int ready = select(server + 1, &set, nullptr, nullptr, &timeout);
                if (ready < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (ready == 0) continue;
                int fd = accept(server, nullptr, nullptr);
                if (fd < 0) continue;
                timeval timeouts = {}; timeouts.tv_sec = 3;
                (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeouts, sizeof(timeouts));
                (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeouts, sizeof(timeouts));
                Payload* ctx = static_cast<Payload*>(heap_caps_calloc(1, sizeof(Payload),
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                const bool success = ctx && serve_once(fd, ctx, expires);
                if (ctx) { secure_wipe_payload(ctx); heap_caps_free(ctx); }
                close(fd);
                if (success) {
                    const esp_err_t ret = nas_catalog_service_request_pending_switch();
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "NAS热切换任务启动失败：%s；旧NAS未切换，需重新配对", esp_err_to_name(ret));
                    }
                    break;
                }
                ++attempts;
            }
        } else ESP_LOGW(TAG, "NAS配置监听启动失败：errno=%d", errno);
        close(server);
    }
    portENTER_CRITICAL(&lock);
    memset(shown_code, 0, sizeof(shown_code));
    portEXIT_CRITICAL(&lock);
    active.store(false);
    ESP_LOGI(TAG, "NAS临时配置窗口已关闭，任务退出");
    vTaskDelete(nullptr);
}
} // namespace

esp_err_t nas_wifi_provisioning_arm(char code[7]) {
    if (!code) return ESP_ERR_INVALID_ARG;
    WifiServiceSnapshot wifi = {};
    if (!wifi_service_get_snapshot(&wifi) || !wifi.connected) return ESP_ERR_INVALID_STATE;
    if (active.load()) {
        portENTER_CRITICAL(&lock);
        memcpy(code, shown_code, 7);
        portEXIT_CRITICAL(&lock);
        return ESP_OK;
    }
    portENTER_CRITICAL(&lock);
    // Rejection sampling avoids modulo bias and preserves leading zeros.
    uint32_t value = 0;
    do { value = esp_random(); } while (value >= 4294000000UL);
    snprintf(shown_code, sizeof(shown_code), "%06lu",
             static_cast<unsigned long>(value % 1000000UL));
    memcpy(code, shown_code, 7);
    portEXIT_CRITICAL(&lock);
    active.store(true);
    if (xTaskCreatePinnedToCore(pairing_task, "nas_pair", 6144, nullptr, 2,
                                nullptr, 1) != pdPASS) {
        nas_wifi_provisioning_cancel();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void nas_wifi_provisioning_cancel() { active.store(false); }
