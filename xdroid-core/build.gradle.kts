plugins {
    alias(libs.plugins.android.library)
}

// :xdroid-core owns the native build (glue CMake -> libxdroid360.so wrapping
// the upstream static libs) and the JNI boundary classes. Built once,
// consumed transitively by :xdroid-app.
android {
    namespace = "xdroid360.core"
    compileSdk = 35
    ndkVersion = "29.0.14206865"

    defaultConfig {
        // API 29+: native ELF TLS (thread_local is a register read).
        minSdk = 29

        externalNativeBuild {
            cmake {
                abiFilters += listOf("arm64-v8a")
                // Baseline ISA every arm64 Android 10+ phone has (LSE atomics,
                // RDM, fp16 storage), tuned for current big cores. No -mcpu:
                // that hardcodes one SoC, and LLVM's cortex-x4 implies SVE,
                // which Qualcomm Kryo lacks (SIGILL). Guest JIT output is
                // unaffected: it is emitted at runtime.
                // Gradle debug = CMake Debug, so upstream also defines DEBUG;
                // it only gates gdbstub/desktop-window logging, never a hot
                // path. NDEBUG (asserts off) is what matters here. Frame
                // pointers stay on for simpleperf.
                cFlags += listOf("-O3", "-DNDEBUG", "-g", "-fno-strict-aliasing", "-march=armv8.2-a", "-mtune=cortex-x4")
                cppFlags += listOf("-O3", "-DNDEBUG", "-g", "-fno-strict-aliasing", "-march=armv8.2-a", "-mtune=cortex-x4")
                // ThinLTO (slow first link, fast incremental): NDK tools live
                // outside PATH, so pass them explicitly. Same-toolchain
                // llvm-ar => bitcode always matches NDK clang.
                val llvmBin = System.getenv("ANDROID_NDK_HOME")?.let { "$it/toolchains/llvm/prebuilt/windows-x86_64/bin" }
                    ?: (System.getenv("LOCALAPPDATA") + "\\Android\\Sdk\\ndk\\29.0.14206865\\toolchains\\llvm\\prebuilt\\windows-x86_64\\bin")
                arguments += listOf(
                    "-DXDROID_LTO=ON",
                    "-DLLVM_AR=$llvmBin/llvm-ar.exe",
                    "-DLLVM_RANLIB=$llvmBin/llvm-ranlib.exe",
                )
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.30.3"
        }
    }
}
