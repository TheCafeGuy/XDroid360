pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "XDroid360"
include(":xdroid-app")
include(":xdroid-core")
// The public export has no TV receiver app (see xdroid-app/build.gradle.kts).
if (file("xdroid-tv").isDirectory && !providers.gradleProperty("xdroid.notv").isPresent) include(":xdroid-tv")
