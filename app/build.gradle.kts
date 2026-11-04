import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.paintengine"
    compileSdk = 35
    // NDK r27c (LTS). Any r26+ works; change here if a different one is installed.
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.paintengine"
        minSdk = 26          // Android 8.0: guaranteed libvulkan loader + GLES 3.x
        targetSdk = 35
        versionCode = 11
        versionName = "0.11.0-block11"

        ndk {
            // arm64-v8a: Galaxy A33 and virtually every current device.
            // x86_64: Android Studio emulator.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
    }

    buildTypes {
        debug {
            isJniDebuggable = true
        }
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        buildConfig = true
    }

    lint {
        // Only the API-level checks gate the build: the app is framework-only
        // with minSdk 26, so every newer call must be version-guarded.
        checkOnly += listOf("NewApi", "InlinedApi")
        abortOnError = true
        checkReleaseBuilds = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

// No library dependencies: the app only uses the Android framework.
// JUnit is test-only (pure-Kotlin PxPaint core: settings, sizes, storage, input math).
dependencies {
    testImplementation("junit:junit:4.13.2")
}
