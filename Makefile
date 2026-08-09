.PHONY: native full playstore debug release \
	assemble-full-debug assemble-full-release \
	assemble-playstore-debug assemble-playstore-release help

native:
	./build.sh

full:
	./build.sh full

playstore:
	./build.sh playstore

debug:
	./run.sh debug

release:
	./run.sh release

assemble-full-debug:
	./gradlew assembleFullDebug

assemble-full-release:
	./gradlew assembleFullRelease

assemble-playstore-debug:
	./gradlew assemblePlaystoreDebug

assemble-playstore-release:
	./gradlew assemblePlaystoreRelease

help:
	@printf '%s\n' \
		'NNAGA build targets:' \
		'  native                       Build native libraries (./build.sh)' \
		'  full                         Build full-flavor native libraries (./build.sh full)' \
		'  playstore                    Build Play Store native libraries/assets (./build.sh playstore)' \
		'  debug                        Build and optionally install/start full debug (./run.sh debug)' \
		'  release                      Build full release APK/AAB (./run.sh release)' \
		'  assemble-full-debug          Assemble the full debug APK (Gradle)' \
		'  assemble-full-release        Assemble the signed full release APK (Gradle)' \
		'  assemble-playstore-debug     Assemble the Play Store debug APK (Gradle)' \
		'  assemble-playstore-release   Assemble the signed Play Store release APK (Gradle)' \
		'' \
		'All Android packaging targets require RELEASE_* properties in ~/.gradle/gradle.properties.'
