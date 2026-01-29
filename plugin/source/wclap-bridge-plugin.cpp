#include "wclap-bridge.h"

#include "clap/all.h"

#include "semver/semver.hpp"
#include "cbor-walker/cbor-walker.h"

#include <iostream>
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <vector>
#include <filesystem>

#ifndef LOG_EXPR
#	define LOG_EXPR(expr) std::cout << #expr " = " << (expr) << std::endl;
#endif

void scanWclapDirectory(const std::string &pathStr);

#if __APPLE__ && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
#	include <stdlib.h>
void scanWclapDirectories() {
	std::string wclapPath = "/Library/Audio/Plug-Ins/WCLAP/";
	scanWclapDirectory(wclapPath);
	
	const char *home = getenv("HOME");
	if (home) {
		scanWclapDirectory(home + wclapPath);
	}
}
#elif defined(_WIN32)
#	include <shlobj.h>
#	include <stringapiset.h>
std::string stringFromPWSTR(PWSTR pwstr) {
	auto length = WideCharToMultiByte(CP_UTF8, 0, pwstr, -1, nullptr, 0, nullptr, nullptr);
	std::string result;
	result.resize(length);
	WideCharToMultiByte(CP_UTF8, 0, pwstr, -1, result.data(), length, nullptr, nullptr);
	return result;
}
void scanWclapDirectories() {
	PWSTR knownPath;
	
	if (SHGetKnownFolderPath(FOLDERID_ProgramFilesCommon, 0, nullptr, &knownPath) == S_OK) {
		scanWclapDirectory(stringFromPWSTR(knownPath) + "\\WCLAP\\");
	}
	CoTaskMemFree(knownPath);

	if (SHGetKnownFolderPath(FOLDERID_UserProgramFilesCommon, 0, nullptr, &knownPath) == S_OK) {
		scanWclapDirectory(stringFromPWSTR(knownPath) + "\\WCLAP\\");
	}
	CoTaskMemFree(knownPath);
}
#elif defined(__linux__)
#	include <stdlib.h>
void scanWclapDirectories() {
	scanWclapDirectory("/usr/lib/wclap/");

	// ~/.wclap
	const char *home = getenv("HOME");
	if (home) {
		scanWclapDirectory(home + std::string("/.wclap/"));
	}
}
#else
#	error "Unsupported OS - please add to wclap-bridge-plugin.cpp"
#endif

std::mutex initMutex;
std::atomic<int> initCounter = 0;

static std::vector<std::string> wclapDirs;
static std::vector<clap_plugin_invalidation_source> invalidations;
void makeInvalidations() {
	for (auto &str : wclapDirs) {
		invalidations.push_back(clap_plugin_invalidation_source{
			.directory=str.c_str(),
			.filename_glob="*.wclap",
			.recursive_scan=true
		});
	}
}

struct PluginDescriptor {
	clap_plugin_descriptor clapDesc;
	std::vector<const char *> clapFeatures;
	
	std::string id, name, vendor, url, manual_url, support_url, version, description;
	std::vector<std::string> features;

	PluginDescriptor(const clap_plugin_descriptor *desc) {
		clapDesc = *desc;

		setString(id, clapDesc.id);
		setString(name, clapDesc.name);
		setString(vendor, clapDesc.vendor);
		setString(url, clapDesc.url);
		setString(manual_url, clapDesc.manual_url);
		setString(support_url, clapDesc.support_url);
		setString(version, clapDesc.version);
		setString(description, clapDesc.description);
		
		const char * const *rawFeatures = clapDesc.features;
		while (rawFeatures && *rawFeatures) {
			features.push_back(*rawFeatures);
			++rawFeatures;
		}
		for (auto &s : features) clapFeatures.push_back(s.c_str());
		clapFeatures.push_back(nullptr);
		clapDesc.features = clapFeatures.data();
	}
	
private:
	void setString(std::string &field, const char *&clapField) {
		if (clapField) field = clapField; // some of these fields can be NULL according to the standard, but it causes problems in some hosts/wrappers
		clapField = field.c_str();
	}
};

//----

struct Wclap {
	Wclap(const std::string &wclapPath) : wclapPath(wclapPath) {}
	Wclap(const Wclap &other) = delete;
	Wclap(Wclap &&other) : wclapPath(other.wclapPath), handle(other.handle) {
		other.handle = nullptr;
	}
	~Wclap() {
		if (handle) wclap_close(handle);
	}
	
	const clap_plugin_factory * getPluginFactory() {
		std::lock_guard<std::mutex> lock{mutex};

		if (!handle) {
			handle = wclap_open(wclapPath.c_str());
			if (handle) std::cout << "Opened WCLAP: " << wclapPath << std::endl;
		}
		
		char errorMessage[256] = "";
		if (handle && wclap_get_error(handle, errorMessage, 256)) {
			std::cerr << "WCLAP bridge plugin (" << wclapPath << ") failed to open: " << errorMessage << std::endl;
			wclap_close(handle);
			handle = nullptr;
		}
		if (!handle) return nullptr;
		return (const clap_plugin_factory *)wclap_get_factory(handle, CLAP_PLUGIN_FACTORY_ID);
	}
	
	std::vector<PluginDescriptor> plugins;
	
	void scanPlugins() {
		plugins.clear();
		
		bool alreadyOpen = !!handle;
		auto *pluginFactory = getPluginFactory();
		if (pluginFactory) {
			auto count = pluginFactory->get_plugin_count(pluginFactory);
			for (size_t i = 0; i < count; ++i) {
				auto *rawDesc = pluginFactory->get_plugin_descriptor(pluginFactory, i);
				if (rawDesc) {
					plugins.emplace_back(rawDesc);
				}
			}
		}

		if (!alreadyOpen) {
			// Forget the module
			if (handle) wclap_close(handle);
			pluginFactory = nullptr;
			handle = nullptr;
		}
	}

private:
	std::mutex mutex;
	std::string wclapPath;
	void *handle = nullptr;
};
static std::vector<Wclap> wclapList;
bool endsWith(const std::string &str, const std::string &end) {
	if (str.size() < end.size()) return false;
	auto strEnd = str.substr(str.size() - end.size());
	for (auto &c : strEnd) {
		if (c >= 'A' && c <= 'Z') c -= ('a' - 'A');
	}
	return strEnd == end;
}
void scanWclapDirectory(const std::string &pathStr) {
	wclapDirs.push_back(pathStr);
	
	if (!std::filesystem::exists(pathStr)) return;
	for (auto &entry : std::filesystem::recursive_directory_iterator(pathStr)) {
		auto wclapPath = entry.path().string();
		if (endsWith(wclapPath, ".wclap") || endsWith(wclapPath, ".wclap.wasm")) {
			wclapList.emplace_back(wclapPath);
		}
	}
}

struct Plugin {
	size_t wclapIndex, pluginIndex;
};
static struct std::vector<Plugin> pluginList;

CLAP_EXPORT bool clap_init(const char *modulePath) {
	std::lock_guard<std::mutex> lock{initMutex};
	if (initCounter++) return true;
	
	auto globalInit = wclap_global_init(250); // allow 250ms for any given function call
	if (!globalInit) return false;
	wclap_set_strings("wclap:", "[WCLAP] ", "");
	
	scanWclapDirectories();
	// TODO: search CLAP_PATH environment variable
	
	for (size_t wclapIndex = 0; wclapIndex < wclapList.size(); ++wclapIndex) {
		auto &wclap = wclapList[wclapIndex];
		wclap.scanPlugins(); // TODO: cache this

		for (size_t pluginIndex = 0; pluginIndex < wclap.plugins.size(); ++pluginIndex) {
			auto &desc = wclap.plugins[pluginIndex];

			// Only add to the list if it's not a duplicate
			bool duplicate = false;
			for (auto &existing : pluginList) {
				auto &existingDesc = wclapList[existing.wclapIndex].plugins[existing.pluginIndex];
				if (desc.id == existingDesc.id) {
					duplicate = true;
					// Check if this one is newer than the one we already found
					bool newer = false;
					if (existingDesc.version.empty()) {
						newer = true;
					} else if (!desc.version.empty()) {
						auto ver = semver::version::parse(desc.version);
						auto existingVer = semver::version::parse(existingDesc.version);
						newer = ver > existingVer;
					}
					if (newer) existing = {wclapIndex, pluginIndex};
					break;
				}
			}

			if (!duplicate) pluginList.push_back({wclapIndex, pluginIndex});
		}
	}
	
	return true;
}

CLAP_EXPORT void clap_deinit() {
	std::lock_guard<std::mutex> lock{initMutex};
	if (--initCounter) return;

	wclapDirs.clear();
	invalidations.clear();
	wclapList.clear();
	pluginList.clear();

	wclap_global_deinit();
}

static uint32_t pluginFactory_get_plugin_count(const struct clap_plugin_factory *factory) {
	return uint32_t(pluginList.size());
}
static const clap_plugin_descriptor_t * pluginFactory_get_plugin_descriptor(const struct clap_plugin_factory *factory, uint32_t index) {
	if (index >= pluginList.size()) return nullptr;
	auto &plugin = pluginList[index];
	auto &desc = wclapList[plugin.wclapIndex].plugins[plugin.pluginIndex];
	return &desc.clapDesc;
}
static const clap_plugin_t * pluginFactory_create_plugin(const struct clap_plugin_factory *factory, const clap_host *host, const char *pluginId) {
	for (auto &plugin : pluginList) {
		auto &wclap = wclapList[plugin.wclapIndex];
		auto &desc = wclap.plugins[plugin.pluginIndex];
		if (desc.id == pluginId) {
			auto *pluginFactory = wclap.getPluginFactory();
			if (!pluginFactory) return nullptr;
			return pluginFactory->create_plugin(pluginFactory, host, pluginId);
		}
	}
	return nullptr;
}

static uint32_t pluginInvalidationFactory_count(const struct clap_plugin_invalidation_factory *factory) {
	return uint32_t(invalidations.size());
}
static const clap_plugin_invalidation_source * pluginInvalidationFactory_get(const struct clap_plugin_invalidation_factory *factory, uint32_t index) {
	if (index >= invalidations.size()) return nullptr;
	return invalidations.data() + index;
}
static bool pluginInvalidationFactory_refresh(const struct clap_plugin_invalidation_factory *factory) {
	return true;
}

CLAP_EXPORT const void * clap_get_factory(const char* factoryId) {
	if (!std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID)) {
		static const clap_plugin_factory factory{
			.get_plugin_count=pluginFactory_get_plugin_count,
			.get_plugin_descriptor=pluginFactory_get_plugin_descriptor,
			.create_plugin=pluginFactory_create_plugin
		};
		return &factory;
	}
	if (!std::strcmp(factoryId, CLAP_PLUGIN_INVALIDATION_FACTORY_ID)) {
		static const clap_plugin_invalidation_factory factory{
			.count=pluginInvalidationFactory_count,
			.get=pluginInvalidationFactory_get,
			.refresh=pluginInvalidationFactory_refresh
		};
		return &factory;
	}
	return nullptr;
}
