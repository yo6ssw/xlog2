plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "ro.scripca.xlog2"
    compileSdk = 35
    ndkVersion = "26.3.11579264"

    defaultConfig {
        applicationId = "ro.scripca.xlog2"
        minSdk = 26
        targetSdk = 35
        // Versioned in lockstep with the ecosystem release tags (vX.Y.Z) so
        // F-Droid autoupdate (UpdateCheckMode: Tags + AutoUpdateMode: Version v%v)
        // can map a tag to this build. versionCode = major*1000000 + minor*10000 + patch*100.
        versionCode = 60900
        versionName = "0.6.9"

        ndk {
            // ABIs we vendor deps for (see android/third_party/build-deps.sh).
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                // Static libc++ so the APK ships ONE self-contained native lib
                // (no libc++_shared.so to also 16 KB-align). Safe here: our JNI
                // lib never passes STL objects across a .so boundary.
                arguments += "-DANDROID_STL=c++_static"
                // QRZ qrz.com tier is done in Kotlin/OkHttp, so the native core
                // is built WITHOUT libcurl/TLS (only libsodium + sqlite vendored).
                // Flip to ON only after building libcurl per ABI (build-deps.sh).
                arguments += "-DXLOG_MOBILE_QRZ=OFF"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Release signing is configured only when a keystore is supplied via the
    // environment (CI decodes the KEYSTORE_FILE secret before the build). With
    // no keystore set — e.g. a local `assembleRelease` — the block is skipped
    // and Gradle produces an unsigned APK, so nothing breaks without secrets.
    // Empty (not just null) when CI has no keystore secret: the workflow passes
    // KEYSTORE_FILE=${{ steps.keystore.outputs.path }}, which resolves to "".
    val keystorePath = System.getenv("KEYSTORE_FILE")?.takeIf { it.isNotBlank() }
    signingConfigs {
        if (keystorePath != null && file(keystorePath).exists()) {
            create("release") {
                storeFile = file(keystorePath)
                storePassword = System.getenv("KEYSTORE_PASSWORD")
                keyAlias = System.getenv("KEY_ALIAS") ?: "xlog2"
                keyPassword = System.getenv("KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // null when no keystore is present -> unsigned local build.
            signingConfig = signingConfigs.findByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures {
        compose = true
        // Generates BuildConfig.VERSION_NAME / VERSION_CODE from defaultConfig,
        // so the UI can show the running version without hardcoding it.
        buildConfig = true
    }
    packaging {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }

    // AGP embeds a "dependency information" block in the APK signing block by
    // default (for Play). F-Droid rejects extra signing blocks, and it also
    // leaks a dependency list, so omit it. Applies to both our release build and
    // F-Droid's from-source build, so the reproducible match is preserved.
    dependenciesInfo {
        includeInApk = false
        includeInBundle = false
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.09.02")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.compose.ui:ui-tooling-preview")
    debugImplementation("androidx.compose.ui:ui-tooling")

    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.6")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.6")
    implementation("androidx.navigation:navigation-compose:2.8.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    // Direct qrz.com XML-API lookups (the native core is built without libcurl).
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
}
