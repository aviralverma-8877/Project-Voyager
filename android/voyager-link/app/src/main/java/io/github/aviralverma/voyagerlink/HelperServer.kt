package io.github.aviralverma.voyagerlink

import fi.iki.elonen.NanoHTTPD
import fi.iki.elonen.NanoWSD
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.io.IOException
import java.util.Collections

/**
 * The loopback server the device page talks to. Same protocol as the Windows
 * helper (tools/voyager-link/server.go), so data_main/script.js needs no
 * Android-specific code:
 *
 *   GET /info            -> {"app":"voyager-link","version":...,"busy":bool}
 *   WS  /serial          -> text frames are JSON commands, binary frames are serial bytes
 *     page -> helper: {"op":"list"} | {"op":"open","port":name,"baud":n} | {"op":"close"}
 *     helper -> page: {"op":"hello"} | {"op":"ports","ports":[...]} | {"op":"opened","port":name}
 *                     | {"op":"closed","port":name,"reason":text} | {"op":"error","msg":text}
 *
 * Listens on 127.0.0.1 only, and only answers pages that pass [OriginRules].
 */
class HelperServer(
    port: Int,
    private val version: String,
    private val ports: SerialPorts,
    private val onStatus: (pages: Int, openPort: String?) -> Unit,
) : NanoWSD("127.0.0.1", port) {

    companion object {
        const val APP_NAME = "voyager-link"
        const val MAX_SESSIONS = 8

        /** Ports the page probes, in order. Keep in sync with HELPER_PORTS in data_main/script.js. */
        val HELPER_PORTS = intArrayOf(47321, 47322, 47323, 47324, 47325)
    }

    private val sessions: MutableSet<Session> = Collections.synchronizedSet(HashSet())

    /** port name -> the page holding it. Each port belongs to at most one page. */
    private val owners = HashMap<String, Session>()

    private fun busy(): Boolean = synchronized(owners) { owners.isNotEmpty() }

    private fun reportStatus() {
        val open = synchronized(owners) { owners.keys.firstOrNull() }
        onStatus(sessions.size, open)
    }

    /** Closes every open serial link (called when the service stops). */
    fun shutdown() {
        val copy = synchronized(sessions) { ArrayList(sessions) }
        for (s in copy) s.closePort("Voyager Link stopped", notifyPage = false)
        stop()
    }

    // ------------------------------------------------------------------ HTTP

    private fun text(status: NanoHTTPD.Response.Status, body: String): NanoHTTPD.Response =
        NanoHTTPD.newFixedLengthResponse(status, "text/plain", body)

    override fun serve(session: NanoHTTPD.IHTTPSession): NanoHTTPD.Response {
        if (session.uri == "/serial") {
            val origin = session.headers["origin"]
            if (!OriginRules.allowed(origin)) return text(NanoHTTPD.Response.Status.FORBIDDEN, "origin not allowed")
            if (!isWebsocketRequested(session)) return text(NanoHTTPD.Response.Status.BAD_REQUEST, "websocket expected")
            if (sessions.size >= MAX_SESSIONS) return text(NanoHTTPD.Response.Status.FORBIDDEN, "too many pages connected")
            return super.serve(session) // WebSocket handshake -> openWebSocket()
        }
        return serveHttp(session)
    }

    override fun serveHttp(session: NanoHTTPD.IHTTPSession): NanoHTTPD.Response {
        if (session.uri != "/info") return text(NanoHTTPD.Response.Status.NOT_FOUND, "not found")
        val origin = session.headers["origin"]
        val allowed = OriginRules.allowed(origin)

        val response: NanoHTTPD.Response
        if (session.method == NanoHTTPD.Method.OPTIONS) {
            response = text(
                if (allowed) NanoHTTPD.Response.Status.NO_CONTENT else NanoHTTPD.Response.Status.FORBIDDEN,
                ""
            )
        } else if (!allowed && !origin.isNullOrEmpty()) {
            // No Origin at all (a probe from the app itself) is fine: /info reveals nothing.
            return text(NanoHTTPD.Response.Status.FORBIDDEN, "origin not allowed")
        } else {
            val body = JSONObject()
                .put("app", APP_NAME)
                .put("version", version)
                .put("platform", "android")
                .put("busy", busy())
            response = NanoHTTPD.newFixedLengthResponse(
                NanoHTTPD.Response.Status.OK, "application/json", body.toString()
            )
        }
        if (allowed) {
            response.addHeader("Access-Control-Allow-Origin", origin)
            response.addHeader("Vary", "Origin")
            // Private / Local Network Access: a page on a LAN address talking to
            // loopback needs this on the preflight.
            response.addHeader("Access-Control-Allow-Private-Network", "true")
            response.addHeader("Access-Control-Allow-Methods", "GET, OPTIONS")
        }
        return response
    }

    // ------------------------------------------------------------- WebSocket

    override fun openWebSocket(handshake: NanoHTTPD.IHTTPSession): NanoWSD.WebSocket = Session(handshake)

    inner class Session(handshake: NanoHTTPD.IHTTPSession) : NanoWSD.WebSocket(handshake) {
        private val lock = Any()
        private var transport: SerialTransport? = null
        private var portName: String? = null

        override fun onOpen() {
            sessions.add(this)
            sendJson(JSONObject().put("op", "hello").put("app", APP_NAME).put("version", version))
            reportStatus()
        }

        override fun onClose(
            code: NanoWSD.WebSocketFrame.CloseCode?,
            reason: String?,
            initiatedByRemote: Boolean,
        ) {
            closePort("page disconnected", notifyPage = false)
            sessions.remove(this)
            reportStatus()
        }

        override fun onMessage(message: NanoWSD.WebSocketFrame) {
            if (message.opCode == NanoWSD.WebSocketFrame.OpCode.Binary) {
                val t = synchronized(lock) { transport }
                if (t == null) {
                    sendError("no serial port open")
                    return
                }
                try {
                    t.write(message.binaryPayload)
                } catch (e: Exception) {
                    closePort("write failed: ${e.message}")
                }
                return
            }
            val req = try {
                JSONObject(message.textPayload ?: "")
            } catch (e: JSONException) {
                sendError("bad request")
                return
            }
            when (val op = req.optString("op")) {
                "list" -> {
                    val arr = JSONArray()
                    for (p in ports.list()) {
                        arr.put(
                            JSONObject()
                                .put("name", p.name)
                                .put("desc", p.desc)
                                .put("likely", p.likely)
                                .put("usb", p.usb)
                        )
                    }
                    sendJson(JSONObject().put("op", "ports").put("ports", arr))
                }
                "open" -> {
                    val name = req.optString("port")
                    val baud = req.optInt("baud", 115200).let { if (it > 0) it else 115200 }
                    try {
                        openPort(name, baud)
                        sendJson(JSONObject().put("op", "opened").put("port", name))
                    } catch (e: IOException) {
                        sendError(e.message ?: "cannot open the port")
                    }
                    reportStatus()
                }
                "close" -> {
                    closePort("closed by page")
                    reportStatus()
                }
                else -> sendError("unknown op $op")
            }
        }

        override fun onPong(pong: NanoWSD.WebSocketFrame?) {}

        override fun onException(exception: IOException?) {}

        @Throws(IOException::class)
        private fun openPort(name: String, baud: Int) {
            if (name.isEmpty()) throw IOException("no port given")
            closePort("reopening", notifyPage = false)
            synchronized(owners) {
                val owner = owners[name]
                if (owner != null && owner !== this) throw IOException("$name is in use by another page")
                owners[name] = this
            }
            val opened: SerialTransport
            try {
                opened = ports.open(
                    name, baud,
                    onData = { bytes -> sendBytes(bytes) },
                    // Runs on the transport's reader thread: hand over so close() is
                    // never called from the thread it has to stop.
                    onLost = { reason ->
                        Thread({
                            closePort("port lost: $reason")
                            reportStatus()
                        }, "voyager-port-lost").start()
                    },
                )
            } catch (e: IOException) {
                synchronized(owners) { if (owners[name] === this) owners.remove(name) }
                throw e
            }
            synchronized(lock) {
                transport = opened
                portName = name
            }
        }

        fun closePort(reason: String, notifyPage: Boolean = true) {
            val held = synchronized(lock) {
                val pair = Pair(transport, portName)
                transport = null
                portName = null
                pair
            }
            val t = held.first ?: return
            val name = held.second ?: return
            synchronized(owners) { if (owners[name] === this) owners.remove(name) }
            t.close()
            if (notifyPage)
                sendJson(JSONObject().put("op", "closed").put("port", name).put("reason", reason))
        }

        private fun sendError(msg: String) {
            sendJson(JSONObject().put("op", "error").put("msg", msg))
        }

        private fun sendJson(obj: JSONObject) {
            try {
                send(obj.toString())
            } catch (e: Exception) {
                // The page went away; onClose cleans up.
            }
        }

        private fun sendBytes(bytes: ByteArray) {
            try {
                send(bytes)
            } catch (e: Exception) {
            }
        }
    }
}
