all: bridge-plugin

CMAKE_PARAMS := -DCMAKE_BUILD_TYPE=Release -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=.. -DCMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE=..

out/build: CMakeLists.txt
	cmake -B out/build $(CMAKE_PARAMS)

wclap-bridge-plugin: out/build
	cmake --build out/build --target wclap-bridge-plugin_clap --config Release

clean:
	rm -rf out

# Development

dev: dev-wclap-bridge-plugin

out/%.RPP:
	mkdir -p `dirname "$@"`
	echo "<REAPER_PROJECT 0.1" > "$@"
	echo ">" >> "$@"

dev-%: %
	$(MAKE) out/REAPER/$*.RPP
	pushd ~/Library/Audio/Plug-Ins/CLAP \
		&& rm -f "$*.clap" \
		&& ln -s "$(CURRENT_DIR)/out/Release/$*.clap"
	/Applications/REAPER.app/Contents/MacOS/REAPER out/REAPER/$*.RPP

nodemon:
	nodemon --watch ../source --watch source -e h,hpp,cpp --exec "make bridge-plugin || exit 1"
