package io.github.aviralverma.voyagerlink

import java.net.URI

/**
 * Which web pages may use the helper. Mirrors originAllowed() in
 * tools/voyager-link/server.go: a Voyager device page is served from a private,
 * link-local or loopback IP, from an mDNS (.local) name or from localhost. Any
 * other site open in the browser must not be able to drive the serial port.
 * WebSockets are not covered by CORS, so this check is the gate.
 *
 * Pure JVM code (no Android classes) so it runs in plain unit tests. Host names
 * are never resolved: only IP literals are classified.
 */
object OriginRules {

    fun allowed(origin: String?): Boolean {
        if (origin.isNullOrEmpty()) return false
        val uri = try {
            URI(origin)
        } catch (e: Exception) {
            return false
        }
        val scheme = uri.scheme?.lowercase() ?: return false
        if (scheme != "http" && scheme != "https") return false
        var host = uri.host?.lowercase() ?: return false
        if (host.startsWith("[") && host.endsWith("]")) host = host.substring(1, host.length - 1)
        if (host == "localhost" || host.endsWith(".local")) return true
        return isLocalIpv4(host) || isLocalIpv6(host)
    }

    /** 10/8, 172.16/12, 192.168/16 (private), 127/8 (loopback), 169.254/16 (link-local). */
    private fun isLocalIpv4(host: String): Boolean {
        val parts = host.split(".")
        if (parts.size != 4) return false
        val n = IntArray(4)
        for (i in 0 until 4) {
            val p = parts[i]
            if (p.isEmpty() || p.length > 3 || !p.all { it in '0'..'9' }) return false
            n[i] = p.toInt()
            if (n[i] > 255) return false
        }
        return when {
            n[0] == 10 -> true
            n[0] == 172 && n[1] in 16..31 -> true
            n[0] == 192 && n[1] == 168 -> true
            n[0] == 127 -> true
            n[0] == 169 && n[1] == 254 -> true
            else -> false
        }
    }

    /** ::1 (loopback), fc00::/7 (unique local), fe80::/10 (link-local). */
    private fun isLocalIpv6(host: String): Boolean {
        if (!host.contains(":")) return false
        val h = host.substringBefore("%") // drop a zone id
        if (h == "::1" || h == "0:0:0:0:0:0:0:1") return true
        val first = h.substringBefore(":")
        if (first.isEmpty() || first.length > 4) return false
        val v = first.toIntOrNull(16) ?: return false
        return (v and 0xfe00) == 0xfc00 || (v and 0xffc0) == 0xfe80
    }
}
