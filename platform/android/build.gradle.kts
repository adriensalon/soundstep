plugins {
    alias(libs.plugins.android.application)
}

val assetsDir = project.findProperty("assetsDir") as String?
val nativeLibDir = project.findProperty("nativeLibDir") as String?
    ?: error("Gradle property 'nativeLibDir' must be provided")
val androidBuildDir = project.findProperty("androidBuildDir") as String?
val packageDir = project.findProperty("packageDir") as String?
    ?: error("Gradle property 'packageDir' must be provided")
val targetName = project.findProperty("targetName") as String? ?: "soundstep"
val gamePackage = "com.soundstep.app"

if (androidBuildDir != null) {
    layout.buildDirectory.set(file(androidBuildDir))
}

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
    sourceSets["main"].jniLibs.srcDir(nativeLibDir)

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

tasks.register<Copy>("stageDebugApk") {
    dependsOn("assembleDebug")
    from(layout.buildDirectory.dir("outputs/apk/debug"))
    include("*.apk")
    into(file(packageDir))
    rename { "$targetName-debug.apk" }
}

tasks.register<Copy>("stageReleaseApk") {
    dependsOn("assembleRelease")
    from(layout.buildDirectory.dir("outputs/apk/release"))
    include("*.apk")
    into(file(packageDir))
    rename { "$targetName-release.apk" }
}
