plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

// Bundles the license texts listed in docs/licenses/notices.tsv into
// assets/licenses/NOTICES.txt, shown under About > Open-source licenses:
// BSD, MIT and LGPL all require the notices to ship with the binary.
abstract class LicenseNoticesTask : DefaultTask() {
    @get:InputFile
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val manifest: RegularFileProperty

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val licenseFiles: ConfigurableFileCollection

    @get:Internal
    abstract val repoRoot: DirectoryProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun generate() {
        val root = repoRoot.get().asFile
        val out = StringBuilder()
        out.append("XDroid360 includes the following open-source software.\n")
        out.append("Each section names the component, what it is used for, its license and\n")
        out.append("the full license text.\n\n")
        out.append("FFmpeg and libmspack are licensed under the GNU LGPL 2.1 and are\n")
        out.append("statically linked into libxdroid360.so. You may obtain their source code,\n")
        out.append("and the source code of the rest of XDroid360 needed to relink the app\n")
        out.append("against a modified version of them, from the same place you obtained\n")
        out.append("this app. FFmpeg: https://github.com/has207/FFmpeg (the commit pinned\n")
        out.append("by XDroid360's third_party/FFmpeg). libmspack: third_party/mspack.\n")
        for (line in manifest.get().asFile.readLines()) {
            if (line.isBlank() || line.startsWith("#")) continue
            val fields = line.split("\t")
            check(fields.size == 4) { "notices.tsv: expected 4 tab-separated fields: $line" }
            val (name, use, license, files) = fields
            out.append("\n==== ").append(name).append('\n')
            out.append("Used for: ").append(use).append('\n')
            out.append("License: ").append(license).append('\n')
            for (path in files.split(";")) {
                val file = root.resolve(path)
                check(file.isFile) { "notices.tsv: $path does not exist" }
                out.append('\n').append(file.readText().replace("\r\n", "\n").trimEnd()).append('\n')
            }
        }
        val dir = outputDir.get().asFile.resolve("licenses")
        dir.mkdirs()
        dir.resolve("NOTICES.txt").writeText(out.toString())
    }
}

val noticesManifest = rootProject.layout.projectDirectory.file("docs/licenses/notices.tsv")
val generateLicenseNotices = tasks.register<LicenseNoticesTask>("generateLicenseNotices") {
    manifest.set(noticesManifest)
    repoRoot.set(rootProject.layout.projectDirectory)
    licenseFiles.from(
        noticesManifest.asFile.readLines()
            .filter { it.isNotBlank() && !it.startsWith("#") }
            .flatMap { it.split("\t").last().split(";") }
            .map { rootProject.file(it) },
    )
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(
            generateLicenseNotices, LicenseNoticesTask::outputDir,
        )
    }
}

android {
    namespace = "xdroid360.app"
    compileSdk = 35
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "io.github.thecafeguy.xdroid360"
        minSdk = 29
        targetSdk = 35
        versionCode = 2
        versionName = "0.2.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    buildFeatures {
        compose = true
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
        release {
            isMinifyEnabled = false
            isShrinkResources = false
        }
    }
}

dependencies {
    implementation(project(":xdroid-core"))
    val composeBom = platform(libs.compose.bom)
    implementation(composeBom)
    implementation(libs.compose.ui)
    implementation(libs.compose.ui.graphics)
    implementation(libs.compose.ui.tooling.preview)
    implementation(libs.compose.material3)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
}
