pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        // usb-serial-for-android is published on JitPack only.
        maven { url = uri("https://jitpack.io") }
    }
}

rootProject.name = "voyager-link"
include(":app")
