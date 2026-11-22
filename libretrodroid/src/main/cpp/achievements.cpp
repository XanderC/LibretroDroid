/*
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "achievements.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rc_consoles.h"
#include "rc_hash.h"

#include "environment.h"
#include "libretrodroid.h"
#include "log.h"
#include "vfs/vfs.h"

namespace libretrodroid {

namespace {

// --- JSON, enough for what goes up to Java -------------------------------------------------------

class Json {
public:
    Json& begin() { comma(); out += '{'; first = true; return *this; }
    Json& end() { out += '}'; first = false; return *this; }
    Json& beginArray(const char* key) { name(key); out += '['; first = true; return *this; }
    Json& endArray() { out += ']'; first = false; return *this; }
    Json& key(const char* key) { name(key); first = true; return *this; }

    Json& field(const char* key, const char* value) {
        name(key);
        if (value == nullptr) out += "null"; else quote(value);
        return *this;
    }
    Json& field(const char* key, const std::string& value) { return field(key, value.c_str()); }
    Json& field(const char* key, bool value) { name(key); out += value ? "true" : "false"; return *this; }
    Json& field(const char* key, int64_t value) { name(key); out += std::to_string(value); return *this; }
    Json& field(const char* key, uint32_t value) { return field(key, (int64_t) value); }
    Json& field(const char* key, int value) { return field(key, (int64_t) value); }
    Json& field(const char* key, float value) {
        name(key);
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.2f", value);
        out += buffer;
        return *this;
    }
    Json& null(const char* key) { name(key); out += "null"; return *this; }

    const std::string& str() const { return out; }

private:
    void comma() { if (!first) out += ','; first = false; }
    void name(const char* key) { comma(); quote(key); out += ':'; }
    void quote(const char* text) {
        out += '"';
        for (const char* c = text; *c; c++) {
            auto ch = (unsigned char) *c;
            switch (ch) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (ch < 0x20) {
                        char escaped[8];
                        snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                        out += escaped;
                    } else {
                        out += (char) ch;
                    }
            }
        }
        out += '"';
    }

    std::string out;
    bool first = true;
};

const char* unlockedName(uint8_t unlocked) {
    if (unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE) return "hardcore";
    if (unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE) return "softcore";
    return "none";
}

const char* stateName(uint8_t state) {
    switch (state) {
        case RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE: return "active";
        case RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED: return "unlocked";
        case RC_CLIENT_ACHIEVEMENT_STATE_DISABLED: return "disabled";
        default: return "inactive";
    }
}

const char* typeName(uint8_t type) {
    switch (type) {
        case RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE: return "missable";
        case RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION: return "progression";
        case RC_CLIENT_ACHIEVEMENT_TYPE_WIN: return "win";
        default: return "standard";
    }
}

const char* bucketName(uint8_t bucket) {
    switch (bucket) {
        case RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED: return "locked";
        case RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED: return "unlocked";
        case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED: return "unsupported";
        case RC_CLIENT_ACHIEVEMENT_BUCKET_UNOFFICIAL: return "unofficial";
        case RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED: return "recently_unlocked";
        case RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE: return "active_challenge";
        case RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE: return "almost_there";
        case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSYNCED: return "unsynced";
        default: return "unknown";
    }
}

void writeAchievement(Json& json, const char* key, const rc_client_achievement_t* a) {
    if (key != nullptr) json.key(key);
    json.begin()
        .field("id", a->id)
        .field("title", a->title)
        .field("description", a->description)
        .field("points", a->points)
        .field("badgeUrl", a->badge_url)
        .field("badgeLockedUrl", a->badge_locked_url)
        .field("state", stateName(a->state))
        .field("unlocked", unlockedName(a->unlocked))
        .field("unlockTime", (int64_t) a->unlock_time)
        .field("bucket", bucketName(a->bucket))
        .field("type", typeName(a->type))
        .field("measuredProgress", a->measured_progress)
        .field("measuredPercent", a->measured_percent)
        .field("rarity", a->rarity)
        .field("rarityHardcore", a->rarity_hardcore)
        .end();
}

void writeLeaderboard(Json& json, const rc_client_leaderboard_t* l) {
    json.key("leaderboard").begin()
        .field("id", l->id)
        .field("title", l->title)
        .field("description", l->description)
        .field("trackerValue", l->tracker_value)
        .field("lowerIsBetter", l->lower_is_better != 0)
        .end();
}

void writeGame(Json& json, rc_client_t* client) {
    const rc_client_game_t* game = rc_client_get_game_info(client);
    if (game == nullptr) {
        json.null("game");
        return;
    }
    json.key("game").begin()
        .field("id", game->id)
        .field("consoleId", game->console_id)
        .field("title", game->title)
        .field("hash", game->hash)
        .field("badgeUrl", game->badge_url)
        .end();

    rc_client_user_game_summary_t summary {};
    rc_client_get_user_game_summary(client, &summary);
    json.key("summary").begin()
        .field("achievements", summary.num_core_achievements)
        .field("unlocked", summary.num_unlocked_achievements)
        .field("unsupported", summary.num_unsupported_achievements)
        .field("points", summary.points_core)
        .field("pointsUnlocked", summary.points_unlocked)
        .field("beatenTime", (int64_t) summary.beaten_time)
        .field("completedTime", (int64_t) summary.completed_time)
        .end();
}

// --- Hashing ------------------------------------------------------------------------------------
// The game is read with pread, never through a FILE* or the shared position of a descriptor: a
// virtual file's descriptors are dups, which share one position with the stream the core has open
// on it, and moving that would make the core read from the wrong place.

struct HashFile {
    int fd;
    int64_t position;
    int64_t size;
};

void* hashOpen(const char* path) {
    int fd = VFS::getInstance().duplicateVirtualFD(path);
    if (fd < 0) fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;

    struct stat info {};
    if (fstat(fd, &info) != 0) {
        ::close(fd);
        return nullptr;
    }
    return new HashFile { fd, 0, (int64_t) info.st_size };
}

void hashSeek(void* handle, int64_t offset, int origin) {
    auto* file = static_cast<HashFile*>(handle);
    int64_t base = origin == SEEK_CUR ? file->position : origin == SEEK_END ? file->size : 0;
    file->position = std::max<int64_t>(0, base + offset);
}

int64_t hashTell(void* handle) {
    return static_cast<HashFile*>(handle)->position;
}

size_t hashRead(void* handle, void* buffer, size_t requested) {
    auto* file = static_cast<HashFile*>(handle);
    size_t done = 0;
    while (done < requested) {
        ssize_t got = pread64(file->fd, static_cast<char*>(buffer) + done, requested - done, file->position);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        done += (size_t) got;
        file->position += got;
    }
    return done;
}

void hashClose(void* handle) {
    auto* file = static_cast<HashFile*>(handle);
    ::close(file->fd);
    delete file;
}

void coreMemoryInfo(uint32_t id, rc_libretro_core_memory_info_t* info) {
    info->data = static_cast<uint8_t*>(LibretroDroid::getInstance().coreMemoryData(id));
    info->size = LibretroDroid::getInstance().coreMemorySize(id);
}

void onMemoryMessage(const char* message) {
    LOGI("[rcheevos] %s", message);
}

} // namespace

// --- Session ------------------------------------------------------------------------------------

void Achievements::start(JNIEnv* env, jobject newListener, bool hardcore) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    stop();

    env->GetJavaVM(&vm);
    listener = env->NewGlobalRef(newListener);
    jclass listenerClass = env->GetObjectClass(listener);
    onServerCallMethod = env->GetMethodID(
        listenerClass, "onAchievementsServerCall", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    onEventMethod = env->GetMethodID(
        listenerClass, "onAchievementsEvent", "(Ljava/lang/String;[B)V");
    env->DeleteLocalRef(listenerClass);

    client = rc_client_create(&Achievements::readMemory, &Achievements::serverCall);
    rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_INFO, &Achievements::onLog);
    rc_client_set_event_handler(client, &Achievements::onClientEvent);
    rc_client_set_hardcore_enabled(client, hardcore ? 1 : 0);
    rc_client_set_unofficial_enabled(client, 0);
    rc_client_set_encore_mode_enabled(client, 0);

    rc_hash_callbacks_t hashCallbacks {};
    hashCallbacks.verbose_message = &Achievements::onHashMessage;
    hashCallbacks.error_message = &Achievements::onHashMessage;
    hashCallbacks.filereader.open = &hashOpen;
    hashCallbacks.filereader.seek = &hashSeek;
    hashCallbacks.filereader.tell = &hashTell;
    hashCallbacks.filereader.read = &hashRead;
    hashCallbacks.filereader.close = &hashClose;
    rc_client_set_hash_callbacks(client, &hashCallbacks);

    rc_libretro_init_verbose_message_callback(&onMemoryMessage);
    LOGI("[rcheevos] session started (hardcore %s)", hardcore ? "on" : "off");
}

void Achievements::stop() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (client != nullptr) {
        rc_client_destroy(client);
        client = nullptr;
        LOGI("[rcheevos] session ended");
    }
    pendingCalls.clear();

    rc_libretro_memory_destroy(&memory);
    memory = {};
    memoryReady = false;
    memoryRetryFrames = 0;
    consoleId = 0;
    gameInSession = false;
    resetRequested = false;

    if (listener != nullptr) {
        if (JNIEnv* jni = env()) jni->DeleteGlobalRef(listener);
        listener = nullptr;
    }
    onServerCallMethod = nullptr;
    onEventMethod = nullptr;
}

void Achievements::login(const std::string& username, const std::string& token) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (client == nullptr) return;
    rc_client_begin_login_with_token(client, username.c_str(), token.c_str(), &Achievements::onLoginResult, nullptr);
}

void Achievements::loadGame(uint32_t newConsoleId, const std::string& path, const uint8_t* data, size_t size) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (client == nullptr) return;

    consoleId = newConsoleId;
    initializeMemory();
    gameInSession = true;

    // A game loaded from bytes is hashed from them (the path is then only a name for it); one the
    // core opens itself is read from its path, through the virtual files if it is streamed.
    rc_client_begin_identify_and_load_game(
        client, consoleId, path.empty() ? nullptr : path.c_str(), data, data != nullptr ? size : 0,
        &Achievements::onLoadResult, nullptr);
}

void Achievements::initializeMemory() {
    rc_libretro_memory_destroy(&memory);
    memory = {};
    memoryReady = rc_libretro_memory_init(
        &memory, Environment::getInstance().getMemoryMap(), &coreMemoryInfo, consoleId) != 0
        && memory.total_size > 0;
    LOGI("[rcheevos] memory for console %u: %u regions, %zu bytes%s",
         consoleId, memory.count, memory.total_size,
         Environment::getInstance().getMemoryMap() != nullptr ? " (from the core's memory map)" : "");
}

void Achievements::onServerResponse(int64_t requestId, int httpStatus, const char* body, size_t length) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    auto found = pendingCalls.find(requestId);
    if (found == pendingCalls.end()) return; // for a session that has ended
    PendingCall call = found->second;
    pendingCalls.erase(found);

    rc_api_server_response_t response {};
    response.body = body;
    response.body_length = length;
    response.http_status_code = httpStatus;
    call.callback(&response, call.callbackData);
}

void Achievements::setHardcore(bool enabled) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (client == nullptr) return;
    // Turning it on with a game running raises RC_CLIENT_EVENT_RESET: the game restarts.
    rc_client_set_hardcore_enabled(client, enabled ? 1 : 0);
    Json json;
    json.begin().field("hardcore", enabled).field("reason", "requested").end();
    emit("hardcore_changed", json.str());
}

bool Achievements::isHardcoreActive() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    return client != nullptr && gameInSession && rc_client_get_hardcore_enabled(client);
}

void Achievements::doFrame() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (client == nullptr) return;
    // Some cores only expose their memory once the game has run for a while.
    if (!memoryReady && consoleId != 0 && ++memoryRetryFrames % 60 == 0) initializeMemory();
    rc_client_do_frame(client);
}

void Achievements::idle() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (client == nullptr) return;
    rc_client_idle(client);
}

void Achievements::onGameReset() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (client == nullptr) return;
    rc_client_reset(client);
}

void Achievements::onStateLoaded() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (client == nullptr) return;
    if (isHardcoreActive()) {
        LOGI("[rcheevos] a state was loaded: hardcore is off for the rest of this session");
        rc_client_set_hardcore_enabled(client, 0);
        Json json;
        json.begin().field("hardcore", false).field("reason", "state_loaded").end();
        emit("hardcore_changed", json.str());
    }
    rc_client_reset(client);
}

bool Achievements::takeResetRequest() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    bool requested = resetRequested;
    resetRequested = false;
    return requested;
}

uint32_t Achievements::framesUntilPause() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (client == nullptr || !isHardcoreActive()) return 0;
    uint32_t remaining = 0;
    return rc_client_can_pause(client, &remaining) ? 0 : remaining;
}

std::string Achievements::userAgentClause() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    char buffer[64] = {0};
    // Works without a session too: the clause is rcheevos' version.
    rc_client_get_user_agent_clause(client, buffer, sizeof(buffer));
    return buffer;
}

std::string Achievements::listJson(int grouping) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    Json json;
    json.begin();
    if (client == nullptr) {
        json.null("game").field("hardcore", false).beginArray("buckets").endArray().end();
        return json.str();
    }
    writeGame(json, client);
    json.field("hardcore", isHardcoreActive());
    json.beginArray("buckets");
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, grouping);
    if (list != nullptr) {
        for (uint32_t b = 0; b < list->num_buckets; b++) {
            const rc_client_achievement_bucket_t& bucket = list->buckets[b];
            json.begin()
                .field("label", bucket.label)
                .field("type", bucketName(bucket.bucket_type))
                .field("subsetId", bucket.subset_id)
                .beginArray("achievements");
            for (uint32_t a = 0; a < bucket.num_achievements; a++) {
                writeAchievement(json, nullptr, bucket.achievements[a]);
            }
            json.endArray().end();
        }
        rc_client_destroy_achievement_list(list);
    }
    json.endArray().end();
    return json.str();
}

// --- Callbacks from rc_client --------------------------------------------------------------------

uint32_t Achievements::readMemory(uint32_t address, uint8_t* buffer, uint32_t numBytes, rc_client_t*) {
    // Called from inside rc_client, so under the lock already.
    return rc_libretro_memory_read(&getInstance().memory, address, buffer, numBytes);
}

void Achievements::serverCall(
    const rc_api_request_t* request,
    rc_client_server_callback_t callback,
    void* callbackData,
    rc_client_t*
) {
    Achievements& self = getInstance();
    JNIEnv* jni = self.env();
    if (jni == nullptr || self.listener == nullptr || self.onServerCallMethod == nullptr) {
        rc_api_server_response_t failed {};
        failed.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        failed.body = "";
        callback(&failed, callbackData);
        return;
    }

    int64_t id = self.nextRequestId++;
    self.pendingCalls[id] = PendingCall { callback, callbackData };

    jstring url = jni->NewStringUTF(request->url);
    jstring postData = request->post_data != nullptr ? jni->NewStringUTF(request->post_data) : nullptr;
    jstring contentType = request->content_type != nullptr ? jni->NewStringUTF(request->content_type) : nullptr;
    jni->CallVoidMethod(self.listener, self.onServerCallMethod, (jlong) id, url, postData, contentType);
    if (jni->ExceptionCheck()) {
        jni->ExceptionDescribe();
        jni->ExceptionClear();
    }
    jni->DeleteLocalRef(url);
    if (postData != nullptr) jni->DeleteLocalRef(postData);
    if (contentType != nullptr) jni->DeleteLocalRef(contentType);
}

void Achievements::onLog(const char* message, const rc_client_t*) {
    LOGI("[rcheevos] %s", message);
}

void Achievements::onHashMessage(const char* message, const struct rc_hash_iterator*) {
    LOGI("[rcheevos] hash: %s", message);
}

void Achievements::onLoginResult(int result, const char* errorMessage, rc_client_t* client, void*) {
    Achievements& self = getInstance();
    Json json;
    json.begin().field("result", result).field("error", errorMessage);
    const rc_client_user_t* user = result == RC_OK ? rc_client_get_user_info(client) : nullptr;
    if (user != nullptr) {
        json.key("user").begin()
            .field("username", user->username)
            .field("displayName", user->display_name)
            .field("score", user->score)
            .field("scoreSoftcore", user->score_softcore)
            .field("avatarUrl", user->avatar_url)
            .end();
    } else {
        json.null("user");
    }
    json.end();
    self.emit("login", json.str());
}

void Achievements::onLoadResult(int result, const char* errorMessage, rc_client_t* client, void*) {
    Achievements& self = getInstance();
    const rc_client_game_t* game = rc_client_get_game_info(client);
    // A game RetroAchievements doesn't know loads as a placeholder with id 0: no rules apply to it.
    self.gameInSession = result == RC_OK && game != nullptr && game->id != 0;
    if (game != nullptr) {
        LOGI("[rcheevos] load finished (%d): game %u \"%s\", hash %s",
             result, game->id, game->title ? game->title : "", game->hash ? game->hash : "");
    } else {
        LOGI("[rcheevos] load finished (%d): %s", result, errorMessage ? errorMessage : "");
    }

    Json json;
    json.begin().field("result", result).field("error", errorMessage);
    writeGame(json, client);
    json.field("hardcore", self.isHardcoreActive());
    json.end();
    self.emit("load", json.str());
}

void Achievements::onClientEvent(const rc_client_event_t* event, rc_client_t*) {
    Achievements& self = getInstance();
    Json json;
    json.begin();
    const char* type = nullptr;

    switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            type = "achievement_triggered";
            writeAchievement(json, "achievement", event->achievement);
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
            type = "challenge_show";
            writeAchievement(json, "achievement", event->achievement);
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
            type = "challenge_hide";
            writeAchievement(json, "achievement", event->achievement);
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
            type = "progress_show";
            writeAchievement(json, "achievement", event->achievement);
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
            type = "progress_update";
            writeAchievement(json, "achievement", event->achievement);
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
            type = "progress_hide";
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
            type = "leaderboard_started";
            writeLeaderboard(json, event->leaderboard);
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
            type = "leaderboard_failed";
            writeLeaderboard(json, event->leaderboard);
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
            type = "leaderboard_submitted";
            writeLeaderboard(json, event->leaderboard);
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
            type = event->type == RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW ? "tracker_show"
                 : event->type == RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE ? "tracker_update"
                 : "tracker_hide";
            json.key("tracker").begin()
                .field("id", event->leaderboard_tracker->id)
                .field("display", event->leaderboard_tracker->display)
                .end();
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
            type = "scoreboard";
            writeLeaderboard(json, event->leaderboard);
            json.key("scoreboard").begin()
                .field("submittedScore", event->leaderboard_scoreboard->submitted_score)
                .field("bestScore", event->leaderboard_scoreboard->best_score)
                .field("newRank", event->leaderboard_scoreboard->new_rank)
                .field("entries", event->leaderboard_scoreboard->num_entries)
                .end();
            break;
        case RC_CLIENT_EVENT_RESET:
            // Made from inside a frame, with the core lock held: LibretroDroid::step() resets the
            // game once the frame is done.
            type = "reset";
            self.resetRequested = true;
            break;
        case RC_CLIENT_EVENT_GAME_COMPLETED:
            type = "game_completed";
            break;
        case RC_CLIENT_EVENT_SUBSET_COMPLETED:
            type = "subset_completed";
            json.key("subset").begin()
                .field("id", event->subset->id)
                .field("title", event->subset->title)
                .field("badgeUrl", event->subset->badge_url)
                .end();
            break;
        case RC_CLIENT_EVENT_SERVER_ERROR:
            type = "server_error";
            json.key("serverError").begin()
                .field("message", event->server_error->error_message)
                .field("api", event->server_error->api)
                .field("result", event->server_error->result)
                .field("relatedId", event->server_error->related_id)
                .end();
            break;
        case RC_CLIENT_EVENT_DISCONNECTED:
            type = "disconnected";
            break;
        case RC_CLIENT_EVENT_RECONNECTED:
            type = "reconnected";
            break;
        default:
            return;
    }

    json.end();
    self.emit(type, json.str());
}

// --- Java -----------------------------------------------------------------------------------------

JNIEnv* Achievements::env() {
    if (vm == nullptr) return nullptr;
    JNIEnv* jni = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jni), JNI_VERSION_1_6) == JNI_OK) return jni;
    // Every caller is a Java thread (the GL thread, or the UI thread on destroy), so this is only
    // a fallback.
    if (vm->AttachCurrentThread(&jni, nullptr) == JNI_OK) return jni;
    return nullptr;
}

void Achievements::emit(const char* type, const std::string& json) {
    JNIEnv* jni = env();
    if (jni == nullptr || listener == nullptr || onEventMethod == nullptr) return;
    // The JSON goes up as UTF-8 bytes: NewStringUTF takes modified UTF-8, and a title with a
    // character outside the BMP (an emoji) would abort the app under CheckJNI.
    jstring jType = jni->NewStringUTF(type);
    jbyteArray jJson = jni->NewByteArray((jsize) json.size());
    jni->SetByteArrayRegion(jJson, 0, (jsize) json.size(), reinterpret_cast<const jbyte*>(json.data()));
    jni->CallVoidMethod(listener, onEventMethod, jType, jJson);
    if (jni->ExceptionCheck()) {
        jni->ExceptionDescribe();
        jni->ExceptionClear();
    }
    jni->DeleteLocalRef(jType);
    jni->DeleteLocalRef(jJson);
}

} // namespace libretrodroid
