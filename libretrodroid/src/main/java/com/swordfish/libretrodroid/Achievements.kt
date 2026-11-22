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

package com.swordfish.libretrodroid

/**
 * How RetroAchievements is reached. The app makes the HTTP requests, with its own client and its
 * own User-Agent (RetroAchievements asks every emulator to name itself: "App/version" followed by
 * [GLRetroView.achievementsUserAgentClause]).
 */
fun interface AchievementsServer {
    /**
     * Makes [request] and calls [respond] once with the answer, from any thread. This is called on
     * the emulation thread: start the request and return without waiting for it.
     */
    fun call(request: AchievementsRequest, respond: (AchievementsResponse) -> Unit)
}

/** A request to RetroAchievements: POST [postData] (as [contentType]) to [url], or GET [url] if there is no body. */
class AchievementsRequest(val url: String, val postData: String?, val contentType: String?)

class AchievementsResponse(val httpStatus: Int, val body: ByteArray) {
    companion object {
        /** No answer came (no connection, a timeout): an unlock is kept and sent again later. */
        const val RETRYABLE_FAILURE = -2

        fun failed() = AchievementsResponse(RETRYABLE_FAILURE, ByteArray(0))
    }
}

/**
 * Plays the game with RetroAchievements: signs in with [token] (from a sign-in made elsewhere, never
 * a password), identifies the game by its hash and loads its achievements. Put it in
 * [GLRetroViewData.achievements].
 */
class AchievementsConfig(
    val server: AchievementsServer,
    val username: String,
    val token: String,
    /** rcheevos' RC_CONSOLE_* id for the game's system. */
    val consoleId: Int,
    val hardcore: Boolean,
)

/**
 * Something rc_client reported. [type] is one of "login", "load", "achievement_triggered",
 * "challenge_show", "challenge_hide", "progress_show", "progress_update", "progress_hide",
 * "leaderboard_started", "leaderboard_failed", "leaderboard_submitted", "tracker_show",
 * "tracker_update", "tracker_hide", "scoreboard", "reset", "game_completed", "subset_completed",
 * "server_error", "disconnected", "reconnected" or "hardcore_changed"; [json] carries its details
 * (see achievements.cpp for each shape).
 */
class AchievementsEvent(val type: String, val json: String) {
    override fun toString() = "AchievementsEvent($type, $json)"
}

/** The native side's way back up. Called from JNI by name: not for apps. */
class AchievementsBridge internal constructor(
    private val server: AchievementsServer,
    private val onEmulationThread: (Runnable) -> Unit,
    private val onEvent: (AchievementsEvent) -> Unit,
) {
    fun onAchievementsServerCall(requestId: Long, url: String, postData: String?, contentType: String?) {
        val respond = { response: AchievementsResponse ->
            onEmulationThread(Runnable {
                LibretroDroid.achievementsServerResponse(requestId, response.httpStatus, response.body)
            })
        }
        // An answer has to come back whatever happens, or rc_client waits for it forever.
        try {
            server.call(AchievementsRequest(url, postData, contentType), respond)
        } catch (e: Exception) {
            respond(AchievementsResponse.failed())
        }
    }

    fun onAchievementsEvent(type: String, json: ByteArray) {
        onEvent(AchievementsEvent(type, json.decodeToString()))
    }
}
