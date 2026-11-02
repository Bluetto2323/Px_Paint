// Toolchain (verified by the CI workflow, .github/workflows/android.yml):
//   Gradle 8.14.3 (wrapper) | AGP 8.7.3 (needs Gradle >= 8.9, JDK 17, API <= 35)
//   Kotlin Gradle plugin 2.2.0 (supports Gradle <= 8.14 and AGP <= 8.10)
//   NDK 27.2.12479018 (r27c) | CMake 3.22.1 | compileSdk/targetSdk 35
plugins {
    id("com.android.application") version "8.7.3" apply false
    id("org.jetbrains.kotlin.android") version "2.2.0" apply false
}
