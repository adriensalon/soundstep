plugins {
    alias(libs.plugins.android.application)
}

val assetsDir = project.findProperty("assetsDir") as String?
val targetName = project.findProperty("targetName") as String? 
    ?: error("Gradle property 'targetName' must be provided (e.g. -PtargetName=mygame)")
val gamePackage = "com.lucaria.lib$targetName"

android {
    namespace = gamePackage
    compileSdk = 35

    defaultConfig {
        applicationId = gamePackage
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
        manifestPlaceholders["NATIVE_TARGET_NAME"] = targetName
    }

    sourceSets["main"].assets {
        if (assetsDir != null) {
            srcDir(assetsDir)
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
        debug {
            // debuggable by default
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
    // You can actually remove everything,
    // but leaving appcompat/material out is good to avoid useless deps.
}
