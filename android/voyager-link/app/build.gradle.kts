plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "io.github.aviralverma.voyagerlink"
    compileSdk = 34

    defaultConfig {
        applicationId = "io.github.aviralverma.voyagerlink"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    // USB host serial drivers (CP210x, CH34x, FTDI, CDC).
    implementation("com.github.mik3y:usb-serial-for-android:3.8.0")
    // One small server for both the HTTP /info probe and the /serial WebSocket.
    implementation("org.nanohttpd:nanohttpd-websocket:2.3.1")

    testImplementation("junit:junit:4.13.2")
}
