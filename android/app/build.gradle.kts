import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "io.github.noribow.cdreader"
    compileSdk = 35
    // CI passes the NDK installed on the runner; otherwise AGP's default NDK is used.
    providers.gradleProperty("cdreader.ndkVersion").orNull?.let { ndkVersion = it }

    defaultConfig {
        applicationId = "io.github.noribow.cdreader"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                // The shared core, the USB Bulk-Only Transport and the JNI bridge.
                arguments += listOf("-DCDREADER_BUILD_TESTS=OFF", "-DANDROID_STL=c++_static")
                targets += "cdreader_jni"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
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
