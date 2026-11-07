package org.tasks.pebble

import android.content.Context
import android.content.Intent
import co.touchlab.kermit.Logger
import java.time.Duration
import java.time.Instant
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter
import java.util.UUID
import org.json.JSONObject
import org.tasks.watch.WatchPinTask
import org.tasks.watch.WatchService

/**
 * Pushes a small set of focus tasks to the watch's timeline using the classic
 * PebbleKit companion pin broadcast (com.getpebble.action.pin.SEND with a
 * pinData JSON extra).
 *
 * The Core Devices Pebble app does not consume this intent yet, so pins will
 * only start appearing on the watch once it does; the selection, scheduling
 * and broadcast all live here either way.
 */
class PebblePins(
    private val context: Context,
    private val watchService: WatchService,
) {
    companion object {
        private const val TAG = "PEBBLE"
        private const val PREFS = "pebble_pins"
        private const val PREF_MODE = "pinMode"
        private const val ACTION_PIN_SEND = "com.getpebble.action.pin.SEND"
        private const val MAX_PINS = 3
        private val ISO_UTC = DateTimeFormatter
            .ofPattern("yyyy-MM-dd'T'HH:mm:ss'Z'")
            .withZone(ZoneOffset.UTC)
        private val BODY_FORMAT = DateTimeFormatter.ofPattern("EEE, MMM d")
    }

    fun mode(): Int =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getInt(PREF_MODE, PebbleProtocol.PIN_MODE_OFF)

    suspend fun push(mode: Int) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putInt(PREF_MODE, mode)
            .apply()
        if (mode == PebbleProtocol.PIN_MODE_OFF) {
            // Pins are dated and age out at the end of the day they were
            // pushed for; there is no legacy delete-only intent to broadcast
            return
        }
        val tasks = watchService.getPinnableTasks(mode).take(MAX_PINS)
        val zone = ZoneId.systemDefault()
        tasks.forEach { task -> pushPin(task, zone) }
        Logger.d(TAG) { "timeline pins: pushed ${tasks.size} (mode $mode)" }
    }

    private fun pushPin(task: WatchPinTask, zone: ZoneId) {
        val now = ZonedDateTime.now(zone)
        val endOfDay = now.toLocalDate().plusDays(1).atStartOfDay(zone)
        // An overdue task pins into the start of today rather than the day
        // it was originally due, so the pin lands in "for the day"
        val start = when {
            task.dueDate > 0 -> maxOf(task.dueDate, endOfDay.minusDays(1).toInstant().toEpochMilli())
            else -> now.toInstant().toEpochMilli()
        }
        val pinData = JSONObject()
            .put("id", pinId(task.id, now.toLocalDate().toEpochDay()))
            .put("time", ISO_UTC.format(Instant.ofEpochMilli(start)))
            .put(
                "duration",
                Duration.between(now.toInstant(), endOfDay.toInstant()).toString(),
            )
            .put(
                "layout",
                JSONObject()
                    .put("type", "genericPin")
                    .put("title", task.title)
                    .put(
                        "body",
                        if (task.dueDate > 0) {
                            "Due ${BODY_FORMAT.withZone(zone).format(Instant.ofEpochMilli(task.dueDate))}"
                        } else {
                            "No due date"
                        },
                    )
                    .put("tinyIcon", "system://images/TIMELINE_CALENDAR"),
            )
        context.sendBroadcast(
            Intent(ACTION_PIN_SEND).putExtra("pinData", pinData.toString()),
        )
    }

    // Stable per task and day, so a re-push updates the same pin instead of
    // stacking duplicates, and tomorrow's selection gets a fresh set
    private fun pinId(taskId: Long, epochDay: Long): String =
        UUID.nameUUIDFromBytes("tasks-pin-$taskId-$epochDay".toByteArray()).toString()
}
