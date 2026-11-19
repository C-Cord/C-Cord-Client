plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

fun computeBaseVersionCode(ver: String): Int {
    val core = ver.trim().removePrefix("v").substringBefore("-").substringBefore("+")
    val parts = core.split(".").map { it.toIntOrNull() ?: 0 }
    val major = parts.getOrElse(0) { 0 }.coerceIn(0, 999)
    val minor = parts.getOrElse(1) { 1 }.coerceIn(0, 99)
    val patch = parts.getOrElse(2) { 0 }.coerceIn(0, 99)
    // Reserve the last digit (0..9) for ABI split offsets (universal=0, x86_64=1, arm64-v8a=2).
    return (major * 1_000_000) + (minor * 10_000) + (patch * 100)
}

val ccordVersionName: String = (project.findProperty("ccordVersionName") as? String)
    ?.trim()?.takeIf { it.isNotEmpty() }
    ?: System.getenv("CCORD_VERSION")?.trim()?.takeIf { it.isNotEmpty() }
    ?: "0.1.0"

val ccordBaseVersionCode: Int = (project.findProperty("ccordVersionCode") as? String)
    ?.toIntOrNull()
    ?: System.getenv("CCORD_VERSION_CODE")?.toIntOrNull()
    ?: computeBaseVersionCode(ccordVersionName).coerceAtLeast(100)

val ccordAbiSplits: Boolean = (project.findProperty("ccordAbiSplits") as? String)
    ?.equals("true", ignoreCase = true)
    ?: (System.getenv("CCORD_ABI_SPLITS") == "1")

val keystorePath: String = System.getenv("CCORD_ANDROID_KEYSTORE")?.trim().orEmpty()
val keystoreFile = keystorePath.takeIf { it.isNotEmpty() }?.let { file(it) }
val hasReleaseKeystore: Boolean = keystoreFile != null && keystoreFile.exists()
val requireReleaseSigning: Boolean = System.getenv("CCORD_REQUIRE_RELEASE_SIGNING") == "1"

if (requireReleaseSigning && !hasReleaseKeystore) {
    throw GradleException(
        "CCORD_REQUIRE_RELEASE_SIGNING=1 is set, but CCORD_ANDROID_KEYSTORE is missing or does not exist."
    )
}

android {
    namespace = "org.ccord.android"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.ccord.android"
        minSdk = 28
        targetSdk = 35
        versionCode = ccordBaseVersionCode
        versionName = ccordVersionName

        if (!ccordAbiSplits) {
            ndk {
                abiFilters += listOf("arm64-v8a", "x86_64")
            }
        }
    }

    if (ccordAbiSplits) {
        splits {
            abi {
                isEnable = true
                reset()
                include("arm64-v8a", "x86_64")
                isUniversalApk = true
            }
        }
    }

    signingConfigs {
        if (hasReleaseKeystore) {
            create("release") {
                storeFile = keystoreFile
                storePassword = System.getenv("CCORD_ANDROID_STORE_PASS") ?: ""
                keyAlias = System.getenv("CCORD_ANDROID_KEY_ALIAS") ?: "ccord-release"
                keyPassword = System.getenv("CCORD_ANDROID_KEY_PASS")
                    ?: System.getenv("CCORD_ANDROID_STORE_PASS") ?: ""
                enableV1Signing = true
                enableV2Signing = true
                enableV3Signing = true
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (hasReleaseKeystore) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
        debug {
            isDebuggable = true
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs {
            // Keep uncompressed 16KB page-aligned .so files for Android 15+ compatibility.
            useLegacyPackaging = false
        }
    }
}

if (ccordAbiSplits) {
    val abiCodes = mapOf(
        "x86_64" to 1,
        "arm64-v8a" to 2
    )
    androidComponents {
        onVariants { variant ->
            variant.outputs.forEach { output ->
                val abiFilter = output.filters.find {
                    it.filterType == com.android.build.api.variant.FilterConfiguration.FilterType.ABI
                }?.identifier
                val offset = abiCodes[abiFilter] ?: 0
                output.versionCode.set(ccordBaseVersionCode + offset)
            }
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
}
