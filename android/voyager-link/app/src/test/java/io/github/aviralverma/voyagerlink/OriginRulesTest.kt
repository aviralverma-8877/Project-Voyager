package io.github.aviralverma.voyagerlink

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OriginRulesTest {

    @Test
    fun devicePagesAreAllowed() {
        val ok = listOf(
            "http://192.168.4.1",
            "http://192.168.29.14",
            "http://192.168.1.50:8080",
            "http://10.0.0.7",
            "http://172.16.0.1",
            "http://172.31.255.254",
            "http://169.254.10.20",
            "http://127.0.0.1:5500",
            "http://localhost",
            "http://project-voyager-AC-67-B2-C5-16-F0.local",
            "https://voyager.local",
            "http://[::1]:8080",
            "http://[fe80::1]",
            "http://[fd12:3456::1]",
        )
        for (o in ok) assertTrue(o, OriginRules.allowed(o))
    }

    @Test
    fun otherSitesAreRefused() {
        val bad = listOf(
            null,
            "",
            "null",
            "http://example.com",
            "https://evil.example",
            "http://8.8.8.8",
            "http://172.15.0.1",
            "http://172.32.0.1",
            "http://192.169.1.1",
            "http://1.2.3",
            "http://999.1.1.1",
            "http://192.168.4.1.evil.com",
            "http://localhost.evil.com",
            "http://evil.local.com",
            "ftp://192.168.4.1",
            "file:///sdcard/page.html",
            "http://[2001:db8::1]",
            "not a url",
        )
        for (o in bad) assertFalse(o ?: "null", OriginRules.allowed(o))
    }
}
