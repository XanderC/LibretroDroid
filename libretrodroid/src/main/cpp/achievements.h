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

#ifndef LIBRETRODROID_ACHIEVEMENTS_H
#define LIBRETRODROID_ACHIEVEMENTS_H

#include <jni.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "rc_client.h"
#include "rc_libretro.h"

namespace libretrodroid {

/**
 * RetroAchievements for the game being played, through rcheevos' rc_client: it identifies the game
 * by its hash, fetches its achievements, watches the game's memory every frame and reports unlocks.
 *
 * The frontend does the networking. A request rc_client wants made goes up to Java as
 * onAchievementsServerCall(id, url, postData, contentType), and the answer comes back through
 * onServerResponse(id, ...). Everything else rc_client has to say (sign-in and load results,
 * unlocks, leaderboards, connection trouble) goes up as onAchievementsEvent(type, json), the JSON
 * as UTF-8 bytes.
 *
 * Every call is made on the emulation thread, except stop(), which LibretroDroid::destroy() makes
 * with the core lock held; a lock of its own keeps the two apart. Lock order is always the core
 * lock first, then this one.
 */
class Achievements {
public:
    static Achievements& getInstance() {
        static Achievements instance;
        return instance;
    }
    Achievements(Achievements const&) = delete;
    void operator=(Achievements const&) = delete;

    /** Starts a session reporting to [listener] (kept as a global reference until stop()). */
    void start(JNIEnv* env, jobject listener, bool hardcore);
    /** Ends the session. Answers still on their way for it are dropped when they arrive. */
    void stop();

    void login(const std::string& username, const std::string& token);
    /**
     * Identifies and loads the game the core has loaded. [path] is what the core was given (a real
     * path or a virtual one); [data] is the game itself when the core was handed its bytes.
     */
    void loadGame(uint32_t consoleId, const std::string& path, const uint8_t* data, size_t size);

    void onServerResponse(int64_t requestId, int httpStatus, const char* body, size_t length);

    void setHardcore(bool enabled);
    /**
     * Whether hardcore rules apply right now: hardcore is on and a game RetroAchievements knows is
     * loaded (or still loading). Fast-forward, cheats and loading states are refused while it is.
     */
    bool isHardcoreActive();

    /** Once per emulated frame, after retro_run. */
    void doFrame();
    /** While no frames are being run (paused), every second or so: keeps pings and retries going. */
    void idle();
    /** The game was reset, or a state loaded: progress towards achievements starts over. */
    void onGameReset();
    /**
     * A state was loaded. Not allowed in hardcore, so it turns hardcore off for the rest of the
     * session (and says so), then starts progress over.
     */
    void onStateLoaded();
    /** rc_client asks for the game to be reset when hardcore is turned on mid-game. Taken once. */
    bool takeResetRequest();

    /** Frames still to play before hardcore allows a pause; 0 if a pause is allowed now. */
    uint32_t framesUntilPause();

    std::string userAgentClause();
    /** The game's achievements as JSON, grouped as rc_client groups them (RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_*). */
    std::string listJson(int grouping);

private:
    Achievements() = default;

    static uint32_t readMemory(uint32_t address, uint8_t* buffer, uint32_t numBytes, rc_client_t* client);
    static void serverCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callbackData, rc_client_t* client);
    static void onLog(const char* message, const rc_client_t* client);
    static void onHashMessage(const char* message, const struct rc_hash_iterator* iterator);
    static void onClientEvent(const rc_client_event_t* event, rc_client_t* client);
    static void onLoginResult(int result, const char* errorMessage, rc_client_t* client, void* userdata);
    static void onLoadResult(int result, const char* errorMessage, rc_client_t* client, void* userdata);

    void initializeMemory();
    void emit(const char* type, const std::string& json);
    JNIEnv* env();

    std::recursive_mutex lock;
    rc_client_t* client = nullptr;

    JavaVM* vm = nullptr;
    jobject listener = nullptr;
    jmethodID onServerCallMethod = nullptr;
    jmethodID onEventMethod = nullptr;

    struct PendingCall {
        rc_client_server_callback_t callback;
        void* callbackData;
    };
    std::unordered_map<int64_t, PendingCall> pendingCalls;
    // Never restarts, so an answer meant for an earlier session can't be taken for one of this one.
    int64_t nextRequestId = 1;

    rc_libretro_memory_regions_t memory {};
    uint32_t consoleId = 0;
    bool memoryReady = false;
    unsigned memoryRetryFrames = 0;

    // A game RetroAchievements may know is loading or loaded: hardcore rules apply to it.
    bool gameInSession = false;
    bool resetRequested = false;
};

} // namespace libretrodroid

#endif //LIBRETRODROID_ACHIEVEMENTS_H
