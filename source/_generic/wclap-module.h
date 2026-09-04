// No `#pragma once`, because we deliberately get included multiple times by `../wclap.h`, with different WCLAP_API_NAMESPACE, WCLAP_BRIDGE_NAMESPACE and WCLAP_BRIDGE_IS64 values

#include "./wclap-module-base.h"

#include "./wclap-plugin-factory.h"
#include <clap/factory/preset-discovery.h>
#include <mutex>

namespace WCLAP_BRIDGE_NAMESPACE {

using namespace WCLAP_API_NAMESPACE;

struct WclapModule : public WclapModuleBase {
	
	template<class Return, class ...Args>
	bool registerHost(Instance *instance, Function<Return, Args...> &wasmFn, Return (*fn)(void *, Args...)) {
		auto prevIndex = wasmFn.wasmPointer;
		wasmFn = registerHostFunction(instance, (void *)this, fn); // defined in the non-generic `../wclap-module.h` so that it produces the correct-sized pointer
		if (wasmFn.wasmPointer == -1) {
			setError("failed to register function");
			return false;
		}
		if (prevIndex != 0 && wasmFn.wasmPointer != prevIndex) {
			// This is when we've previously registered it on another thread, and it needs to match
			setError("function index mismatch");
			return false;
		}
		return true;
	}

	WclapModule(InstanceGroup *instanceGroup) : WclapModuleBase(instanceGroup) {
		if (hasError) return; // base class failed
		if (!addHostFunctions(mainThread.get())) return;
		
		instanceGroup->wasiThreadSpawnContext = this;
		instanceGroup->wasiThreadSpawn = staticWasiThreadSpawn;

		mainThread->init();
		if constexpr (WCLAP_BRIDGE_IS64) {
			entryPtr = {Size(mainThread->entry64.wasmPointer)};
		} else {
			entryPtr = {Size(mainThread->entry32.wasmPointer)};
		}
		if (!entryPtr) {
			setError("clap_entry is NULL");
			return;
		}
		
		bindGlobalArena();
		
		auto scoped = arenaPool.scoped();
		auto pathStr = scoped.writeString(mainThread->path());
		auto version = mainThread->get(entryPtr[&wclap_plugin_entry::clap_version]);
		clapVersion = {version.major, version.minor, version.revision};

		if (!mainThread->call(entryPtr[&wclap_plugin_entry::init], pathStr)) {
			setError("clap_entry::init() returned false");
			return;
		}
		
		hasError = false;
	}
	~WclapModule() {
		// Prevent any new threads from spawning after this point
		auto lock = this->threadLock();
		instanceGroup->wasiThreadSpawn = nullptr;
		instanceGroup->wasiThreadSpawnContext = nullptr;
	}


	struct PresetIndexerContext {
		const clap_preset_discovery_indexer_t *indexer = nullptr;
	};
	struct PresetMetadataContext {
		const clap_preset_discovery_metadata_receiver_t *receiver = nullptr;
	};
	struct NativePresetFactoryState {
		clap_preset_discovery_factory_t api{};
		WclapModule *module = nullptr;
	};
	struct NativePresetProviderState {
		clap_preset_discovery_provider_t api{};
		WclapModule *module = nullptr;
		Pointer<const wclap_preset_discovery_provider> remote;
		uint32_t indexerHandle = 0;
		MemoryArenaPtr arena;
	};

	wclap_preset_discovery_indexer presetIndexerTemplate{};
	wclap_preset_discovery_metadata_receiver presetMetadataTemplate{};
	wclap::IndexLookup<PresetIndexerContext> presetIndexerList;
	wclap::IndexLookup<PresetMetadataContext> presetMetadataList;
	NativePresetFactoryState presetFactoryNative{};
	Pointer<wclap_preset_discovery_factory> presetFactoryRemote;
	std::vector<std::unique_ptr<std::string>> presetStrings;
	std::vector<clap_preset_discovery_provider_descriptor_t> presetDescriptors;
	bool presetBridgeRegistered = false;
	bool presetFactoryEnumerated = false;
	std::mutex presetBridgeMutex;

	const char *presetReadStableString(Pointer<const char> ptr, const char *fallback = nullptr) {
		if (!ptr) return fallback;
		auto text = mainThread->getString(ptr, 4096);
		presetStrings.emplace_back(new std::string(std::move(text)));
		return presetStrings.back()->c_str();
	}

	PresetIndexerContext *presetIndexerFrom(Pointer<const wclap_preset_discovery_indexer> indexer) {
		if (!indexer) return nullptr;
		auto data = mainThread->get(indexer[&wclap_preset_discovery_indexer::indexer_data]);
		if (!data || data.wasmPointer == 0) return nullptr;
		return presetIndexerList.get(uint32_t(data.wasmPointer - 1));
	}
	PresetMetadataContext *presetMetadataFrom(Pointer<const wclap_preset_discovery_metadata_receiver> receiver) {
		if (!receiver) return nullptr;
		auto data = mainThread->get(receiver[&wclap_preset_discovery_metadata_receiver::receiver_data]);
		if (!data || data.wasmPointer == 0) return nullptr;
		return presetMetadataList.get(uint32_t(data.wasmPointer - 1));
	}

	static bool presetIndexerDeclareFiletype(void *context,
		Pointer<const wclap_preset_discovery_indexer> indexer,
		Pointer<const wclap_preset_discovery_filetype> filetype) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetIndexerFrom(indexer);
		if (!ctx || !ctx->indexer || !ctx->indexer->declare_filetype || !filetype) return false;
		auto remote = self.mainThread->get(filetype);
		auto name = remote.name ? self.mainThread->getString(remote.name, 4096) : std::string{};
		auto description = remote.description ? self.mainThread->getString(remote.description, 4096) : std::string{};
		auto extension = remote.file_extension ? self.mainThread->getString(remote.file_extension, 1024) : std::string{};
		clap_preset_discovery_filetype_t native{
			remote.name ? name.c_str() : nullptr,
			remote.description ? description.c_str() : nullptr,
			remote.file_extension ? extension.c_str() : nullptr,
		};
		return ctx->indexer->declare_filetype(ctx->indexer, &native);
	}
	static bool presetIndexerDeclareLocation(void *context,
		Pointer<const wclap_preset_discovery_indexer> indexer,
		Pointer<const wclap_preset_discovery_location> location) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetIndexerFrom(indexer);
		if (!ctx || !ctx->indexer || !ctx->indexer->declare_location || !location) return false;
		auto remote = self.mainThread->get(location);
		auto name = remote.name ? self.mainThread->getString(remote.name, 4096) : std::string{};
		auto path = remote.location ? self.mainThread->getString(remote.location, 4096) : std::string{};
		clap_preset_discovery_location_t native{
			remote.flags,
			remote.name ? name.c_str() : nullptr,
			remote.kind,
			remote.location ? path.c_str() : nullptr,
		};
		return ctx->indexer->declare_location(ctx->indexer, &native);
	}
	static bool presetIndexerDeclareSoundpack(void *context,
		Pointer<const wclap_preset_discovery_indexer> indexer,
		Pointer<const wclap_preset_discovery_soundpack> soundpack) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetIndexerFrom(indexer);
		if (!ctx || !ctx->indexer || !ctx->indexer->declare_soundpack || !soundpack) return false;
		auto remote = self.mainThread->get(soundpack);
		auto read = [&](Pointer<const char> ptr) { return ptr ? self.mainThread->getString(ptr, 4096) : std::string{}; };
		auto id = read(remote.id), name = read(remote.name), description = read(remote.description);
		auto homepage = read(remote.homepage_url), vendor = read(remote.vendor), image = read(remote.image_path);
		clap_preset_discovery_soundpack_t native{
			remote.flags,
			remote.id ? id.c_str() : nullptr,
			remote.name ? name.c_str() : nullptr,
			remote.description ? description.c_str() : nullptr,
			remote.homepage_url ? homepage.c_str() : nullptr,
			remote.vendor ? vendor.c_str() : nullptr,
			remote.image_path ? image.c_str() : nullptr,
			remote.release_timestamp,
		};
		return ctx->indexer->declare_soundpack(ctx->indexer, &native);
	}
	static Pointer<const void> presetIndexerGetExtension(void *,
		Pointer<const wclap_preset_discovery_indexer>, Pointer<const char>) {
		return {0};
	}

	static void presetMetadataOnError(void *context,
		Pointer<const wclap_preset_discovery_metadata_receiver> receiver,
		int32_t osError, Pointer<const char> message) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetMetadataFrom(receiver);
		if (!ctx || !ctx->receiver || !ctx->receiver->on_error) return;
		auto text = message ? self.mainThread->getString(message, 4096) : std::string{};
		ctx->receiver->on_error(ctx->receiver, osError, message ? text.c_str() : nullptr);
	}
	static bool presetMetadataBegin(void *context,
		Pointer<const wclap_preset_discovery_metadata_receiver> receiver,
		Pointer<const char> name, Pointer<const char> loadKey) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetMetadataFrom(receiver);
		if (!ctx || !ctx->receiver || !ctx->receiver->begin_preset) return false;
		auto nameText = name ? self.mainThread->getString(name, 4096) : std::string{};
		auto keyText = loadKey ? self.mainThread->getString(loadKey, 4096) : std::string{};
		return ctx->receiver->begin_preset(ctx->receiver,
			name ? nameText.c_str() : nullptr,
			loadKey ? keyText.c_str() : nullptr);
	}
	static void presetMetadataAddPluginId(void *context,
		Pointer<const wclap_preset_discovery_metadata_receiver> receiver,
		Pointer<const wclap_universal_plugin_id> pluginId) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetMetadataFrom(receiver);
		if (!ctx || !ctx->receiver || !ctx->receiver->add_plugin_id || !pluginId) return;
		auto remote = self.mainThread->get(pluginId);
		auto abi = remote.abi ? self.mainThread->getString(remote.abi, 1024) : std::string{};
		auto id = remote.id ? self.mainThread->getString(remote.id, 4096) : std::string{};
		clap_universal_plugin_id_t native{
			remote.abi ? abi.c_str() : nullptr,
			remote.id ? id.c_str() : nullptr,
		};
		ctx->receiver->add_plugin_id(ctx->receiver, &native);
	}
	static void presetMetadataSetSoundpackId(void *context,
		Pointer<const wclap_preset_discovery_metadata_receiver> receiver,
		Pointer<const char> soundpackId) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetMetadataFrom(receiver);
		if (!ctx || !ctx->receiver || !ctx->receiver->set_soundpack_id) return;
		auto text = soundpackId ? self.mainThread->getString(soundpackId, 4096) : std::string{};
		ctx->receiver->set_soundpack_id(ctx->receiver, soundpackId ? text.c_str() : nullptr);
	}
	static void presetMetadataSetFlags(void *context,
		Pointer<const wclap_preset_discovery_metadata_receiver> receiver, uint32_t flags) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetMetadataFrom(receiver);
		if (ctx && ctx->receiver && ctx->receiver->set_flags) ctx->receiver->set_flags(ctx->receiver, flags);
	}
	static void presetMetadataAddCreator(void *context,
		Pointer<const wclap_preset_discovery_metadata_receiver> receiver, Pointer<const char> creator) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetMetadataFrom(receiver);
		if (!ctx || !ctx->receiver || !ctx->receiver->add_creator) return;
		auto text = creator ? self.mainThread->getString(creator, 4096) : std::string{};
		ctx->receiver->add_creator(ctx->receiver, creator ? text.c_str() : nullptr);
	}
	static void presetMetadataSetDescription(void *context,
		Pointer<const wclap_preset_discovery_metadata_receiver> receiver, Pointer<const char> description) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetMetadataFrom(receiver);
		if (!ctx || !ctx->receiver || !ctx->receiver->set_description) return;
		auto text = description ? self.mainThread->getString(description, 4096) : std::string{};
		ctx->receiver->set_description(ctx->receiver, description ? text.c_str() : nullptr);
	}
	static void presetMetadataSetTimestamps(void *context,
		Pointer<const wclap_preset_discovery_metadata_receiver> receiver,
		wclap_timestamp creation, wclap_timestamp modification) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetMetadataFrom(receiver);
		if (ctx && ctx->receiver && ctx->receiver->set_timestamps)
			ctx->receiver->set_timestamps(ctx->receiver, creation, modification);
	}
	static void presetMetadataAddFeature(void *context,
		Pointer<const wclap_preset_discovery_metadata_receiver> receiver, Pointer<const char> feature) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetMetadataFrom(receiver);
		if (!ctx || !ctx->receiver || !ctx->receiver->add_feature) return;
		auto text = feature ? self.mainThread->getString(feature, 4096) : std::string{};
		ctx->receiver->add_feature(ctx->receiver, feature ? text.c_str() : nullptr);
	}
	static void presetMetadataAddExtraInfo(void *context,
		Pointer<const wclap_preset_discovery_metadata_receiver> receiver,
		Pointer<const char> key, Pointer<const char> value) {
		auto &self = *(WclapModule *)context;
		auto *ctx = self.presetMetadataFrom(receiver);
		if (!ctx || !ctx->receiver || !ctx->receiver->add_extra_info) return;
		auto keyText = key ? self.mainThread->getString(key, 4096) : std::string{};
		auto valueText = value ? self.mainThread->getString(value, 4096) : std::string{};
		ctx->receiver->add_extra_info(ctx->receiver,
			key ? keyText.c_str() : nullptr,
			value ? valueText.c_str() : nullptr);
	}

	bool ensurePresetDiscoveryBridge() {
		if (presetBridgeRegistered) return true;
		if (!registerHost(mainThread.get(), presetIndexerTemplate.declare_filetype, presetIndexerDeclareFiletype) ||
			!registerHost(mainThread.get(), presetIndexerTemplate.declare_location, presetIndexerDeclareLocation) ||
			!registerHost(mainThread.get(), presetIndexerTemplate.declare_soundpack, presetIndexerDeclareSoundpack) ||
			!registerHost(mainThread.get(), presetIndexerTemplate.get_extension, presetIndexerGetExtension) ||
			!registerHost(mainThread.get(), presetMetadataTemplate.on_error, presetMetadataOnError) ||
			!registerHost(mainThread.get(), presetMetadataTemplate.begin_preset, presetMetadataBegin) ||
			!registerHost(mainThread.get(), presetMetadataTemplate.add_plugin_id, presetMetadataAddPluginId) ||
			!registerHost(mainThread.get(), presetMetadataTemplate.set_soundpack_id, presetMetadataSetSoundpackId) ||
			!registerHost(mainThread.get(), presetMetadataTemplate.set_flags, presetMetadataSetFlags) ||
			!registerHost(mainThread.get(), presetMetadataTemplate.add_creator, presetMetadataAddCreator) ||
			!registerHost(mainThread.get(), presetMetadataTemplate.set_description, presetMetadataSetDescription) ||
			!registerHost(mainThread.get(), presetMetadataTemplate.set_timestamps, presetMetadataSetTimestamps) ||
			!registerHost(mainThread.get(), presetMetadataTemplate.add_feature, presetMetadataAddFeature) ||
			!registerHost(mainThread.get(), presetMetadataTemplate.add_extra_info, presetMetadataAddExtraInfo))
			return false;

		presetFactoryNative.module = this;
		presetFactoryNative.api.count = presetFactoryCount;
		presetFactoryNative.api.get_descriptor = presetFactoryGetDescriptor;
		presetFactoryNative.api.create = presetFactoryCreate;
		presetBridgeRegistered = true;
		return true;
	}

	bool ensurePresetFactory(const char *factoryId) {
		if (!ensurePresetDiscoveryBridge()) return false;
		if (!presetFactoryRemote) {
			auto scoped = arenaPool.scoped();
			auto id = scoped.writeString(factoryId);
			auto remote = mainThread->call(entryPtr[&wclap_plugin_entry::get_factory], id);
			presetFactoryRemote = remote.cast<wclap_preset_discovery_factory>();
		}
		if (!presetFactoryRemote) return false;
		if (presetFactoryEnumerated) return true;

		auto count = mainThread->call(presetFactoryRemote[&wclap_preset_discovery_factory::count], presetFactoryRemote);
		presetDescriptors.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			auto descPtr = mainThread->call(presetFactoryRemote[&wclap_preset_discovery_factory::get_descriptor], presetFactoryRemote, i);
			if (!descPtr) continue;
			auto remote = mainThread->get(descPtr);
			presetDescriptors.push_back({
				{remote.clap_version.major, remote.clap_version.minor, remote.clap_version.revision},
				presetReadStableString(remote.id, "unknown-preset-provider"),
				presetReadStableString(remote.name, "Unknown preset provider"),
				presetReadStableString(remote.vendor),
			});
		}
		presetFactoryEnumerated = true;
		return true;
	}

	static uint32_t CLAP_ABI presetFactoryCount(const clap_preset_discovery_factory_t *factory) {
		auto *state = (const NativePresetFactoryState *)factory;
		return state && state->module ? uint32_t(state->module->presetDescriptors.size()) : 0u;
	}
	static const clap_preset_discovery_provider_descriptor_t *CLAP_ABI presetFactoryGetDescriptor(
		const clap_preset_discovery_factory_t *factory, uint32_t index) {
		auto *state = (const NativePresetFactoryState *)factory;
		if (!state || !state->module || index >= state->module->presetDescriptors.size()) return nullptr;
		return &state->module->presetDescriptors[index];
	}
	static const clap_preset_discovery_provider_t *CLAP_ABI presetFactoryCreate(
		const clap_preset_discovery_factory_t *factory,
		const clap_preset_discovery_indexer_t *indexer,
		const char *providerId) {
		auto *state = (const NativePresetFactoryState *)factory;
		if (!state || !state->module || !indexer || !providerId) return nullptr;
		auto &self = *state->module;
		std::lock_guard<std::mutex> presetLock(self.presetBridgeMutex);
		const clap_preset_discovery_provider_descriptor_t *descriptor = nullptr;
		for (auto &candidate : self.presetDescriptors) {
			if (candidate.id && std::strcmp(candidate.id, providerId) == 0) {
				descriptor = &candidate;
				break;
			}
		}
		if (!descriptor) return nullptr;

		auto handle = self.presetIndexerList.retain(new PresetIndexerContext{indexer}) + 1u;
		auto scoped = self.arenaPool.scoped();
		auto copyString = [&](const char *text) -> Pointer<const char> {
			return text ? scoped.writeString(text) : Pointer<const char>{0};
		};
		wclap_preset_discovery_indexer remoteIndexer{};
		remoteIndexer.clap_version = {indexer->clap_version.major, indexer->clap_version.minor, indexer->clap_version.revision};
		remoteIndexer.name = copyString(indexer->name);
		remoteIndexer.vendor = copyString(indexer->vendor);
		remoteIndexer.url = copyString(indexer->url);
		remoteIndexer.version = copyString(indexer->version);
		remoteIndexer.indexer_data = Pointer<void>{Size(handle)};
		remoteIndexer.declare_filetype = self.presetIndexerTemplate.declare_filetype;
		remoteIndexer.declare_location = self.presetIndexerTemplate.declare_location;
		remoteIndexer.declare_soundpack = self.presetIndexerTemplate.declare_soundpack;
		remoteIndexer.get_extension = self.presetIndexerTemplate.get_extension;
		auto remoteIndexerPtr = scoped.copyAcross(remoteIndexer);
		auto remoteProviderId = scoped.writeString(providerId);
		auto remoteProvider = self.mainThread->call(
			self.presetFactoryRemote[&wclap_preset_discovery_factory::create],
			self.presetFactoryRemote, remoteIndexerPtr, remoteProviderId);
		if (!remoteProvider) {
			self.presetIndexerList.release(handle - 1u);
			return nullptr;
		}

		auto *provider = new NativePresetProviderState{};
		provider->module = &self;
		provider->remote = remoteProvider;
		provider->indexerHandle = handle;
		provider->arena = scoped.commit();
		provider->api.desc = descriptor;
		provider->api.provider_data = provider;
		provider->api.init = presetProviderInit;
		provider->api.destroy = presetProviderDestroy;
		provider->api.get_metadata = presetProviderGetMetadata;
		provider->api.get_extension = presetProviderGetExtension;
		return &provider->api;
	}

	static NativePresetProviderState *presetProviderState(const clap_preset_discovery_provider_t *provider) {
		return provider ? static_cast<NativePresetProviderState *>(provider->provider_data) : nullptr;
	}
	static bool CLAP_ABI presetProviderInit(const clap_preset_discovery_provider_t *provider) {
		auto *state = presetProviderState(provider);
		if (!state || !state->module || !state->remote) return false;
		auto &self = *state->module;
		std::lock_guard<std::mutex> presetLock(self.presetBridgeMutex);
		return self.mainThread->call(
			state->remote[&wclap_preset_discovery_provider::init], state->remote);
	}
	static void CLAP_ABI presetProviderDestroy(const clap_preset_discovery_provider_t *provider) {
		auto *state = presetProviderState(provider);
		if (!state) return;
		if (state->module && state->remote) {
			auto &self = *state->module;
			std::lock_guard<std::mutex> presetLock(self.presetBridgeMutex);
			self.mainThread->call(
				state->remote[&wclap_preset_discovery_provider::destroy], state->remote);
			if (state->indexerHandle != 0)
				self.presetIndexerList.release(state->indexerHandle - 1u);
		}
		delete state;
	}
	static bool CLAP_ABI presetProviderGetMetadata(
		const clap_preset_discovery_provider_t *provider,
		uint32_t locationKind,
		const char *location,
		const clap_preset_discovery_metadata_receiver_t *receiver) {
		auto *state = presetProviderState(provider);
		if (!state || !state->module || !state->remote || !receiver) return false;
		auto &self = *state->module;
		std::lock_guard<std::mutex> presetLock(self.presetBridgeMutex);
		auto handle = self.presetMetadataList.retain(new PresetMetadataContext{receiver}) + 1u;
		auto scoped = self.arenaPool.scoped();
		wclap_preset_discovery_metadata_receiver remoteReceiver{};
		remoteReceiver.receiver_data = Pointer<void>{Size(handle)};
		remoteReceiver.on_error = self.presetMetadataTemplate.on_error;
		remoteReceiver.begin_preset = self.presetMetadataTemplate.begin_preset;
		remoteReceiver.add_plugin_id = self.presetMetadataTemplate.add_plugin_id;
		remoteReceiver.set_soundpack_id = self.presetMetadataTemplate.set_soundpack_id;
		remoteReceiver.set_flags = self.presetMetadataTemplate.set_flags;
		remoteReceiver.add_creator = self.presetMetadataTemplate.add_creator;
		remoteReceiver.set_description = self.presetMetadataTemplate.set_description;
		remoteReceiver.set_timestamps = self.presetMetadataTemplate.set_timestamps;
		remoteReceiver.add_feature = self.presetMetadataTemplate.add_feature;
		remoteReceiver.add_extra_info = self.presetMetadataTemplate.add_extra_info;
		auto remoteReceiverPtr = scoped.copyAcross(remoteReceiver);
		auto remoteLocation = location ? scoped.writeString(location) : Pointer<const char>{0};
		auto result = self.mainThread->call(
			state->remote[&wclap_preset_discovery_provider::get_metadata],
			state->remote, locationKind, remoteLocation, remoteReceiverPtr);
		self.presetMetadataList.release(handle - 1u);
		return result;
	}
	static const void *CLAP_ABI presetProviderGetExtension(
		const clap_preset_discovery_provider_t *, const char *) {
		return nullptr;
	}

	std::optional<PluginFactory> pluginFactory;

	void * getFactory(const char *factoryId) {
		if (!factoryId) return nullptr;
		if (!std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID)) {
			if (!pluginFactory) {
				auto scoped = arenaPool.scoped();
				auto wclapStr = scoped.writeString(CLAP_PLUGIN_FACTORY_ID);
				auto factoryPtr = mainThread->call(entryPtr[&wclap_plugin_entry::get_factory], wclapStr);
				pluginFactory.emplace(*this, factoryPtr.cast<wclap_plugin_factory>());
			}
			if (!pluginFactory->ptr) return nullptr;
			return &pluginFactory->clapFactory;
		}
		if (!std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) ||
			!std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT)) {
			std::lock_guard<std::mutex> presetLock(presetBridgeMutex);
			if (!ensurePresetFactory(factoryId)) return nullptr;
			return &presetFactoryNative.api;
		}
		return nullptr;
	}

	bool addHostFunctions(Instance *instance) {
#define HOST_METHOD(obj, name) \
		if (!registerHost(instance, obj.name, obj##_##name)) return false;
		HOST_METHOD(hostTemplate, get_extension);
		HOST_METHOD(hostTemplate, request_restart);
		HOST_METHOD(hostTemplate, request_process);
		HOST_METHOD(hostTemplate, request_callback);

		// Other host-owned structures, which probably only exist temporarily
		HOST_METHOD(inputEventsTemplate, size);
		HOST_METHOD(inputEventsTemplate, get);
		HOST_METHOD(outputEventsTemplate, try_push);
		HOST_METHOD(istreamTemplate, read);
		HOST_METHOD(ostreamTemplate, write);

		//---- Extensions ----
		// There's no global arena at this point, so the pointers get copied across later

		HOST_METHOD(hostAmbisonic, changed);

		HOST_METHOD(hostAudioPortsConfig, rescan);

		HOST_METHOD(hostAudioPorts, is_rescan_flag_supported);
		HOST_METHOD(hostAudioPorts, rescan);

		// Skip this for now, because it needs a few more host structs
		// TODO: implement later
		/*
		HOST_METHOD(hostContextMenu, populate);
		HOST_METHOD(hostContextMenu, perform);
		HOST_METHOD(hostContextMenu, can_popup);
		HOST_METHOD(hostContextMenu, popup);
		*/

		// event-registry extension is skipped because we whitelist events for safety

		HOST_METHOD(hostGui, resize_hints_changed);
		HOST_METHOD(hostGui, request_resize);
		HOST_METHOD(hostGui, request_show);
		HOST_METHOD(hostGui, request_hide);
		HOST_METHOD(hostGui, closed);

		HOST_METHOD(hostLatency, changed);

		HOST_METHOD(hostLog, log);

		HOST_METHOD(hostNoteName, changed);

		HOST_METHOD(hostNotePorts, supported_dialects);
		HOST_METHOD(hostNotePorts, rescan);

		HOST_METHOD(hostParams, rescan);
		HOST_METHOD(hostParams, clear);
		HOST_METHOD(hostParams, request_flush);
		
		// posix-fd-support.h skipped, unless we figure out a way to make it portable

		HOST_METHOD(hostPresetLoad, on_error);
		HOST_METHOD(hostPresetLoad, loaded);

		HOST_METHOD(hostRemoteControls, changed);
		HOST_METHOD(hostRemoteControls, suggest_page);

		HOST_METHOD(hostState, mark_dirty);

		HOST_METHOD(hostSurround, changed);

		HOST_METHOD(hostTail, changed);

		HOST_METHOD(hostThreadCheck, is_main_thread);
		HOST_METHOD(hostThreadCheck, is_audio_thread);

		HOST_METHOD(hostThreadPool, request_exec);

		HOST_METHOD(hostTimerSupport, register_timer);
		HOST_METHOD(hostTimerSupport, unregister_timer);

		HOST_METHOD(hostTrackInfo, get);

		HOST_METHOD(hostVoiceInfo, changed);
		
		//---- Draft extensions ----
		// The webview one is essential for WCLAP GUIs
		// We skip the others because the versioning seems like a compatibility headache

		HOST_METHOD(hostWebview, send);

#undef HOST_METHOD
		return true;
	}
	bool bindGlobalArena() {
		auto scoped = arenaPool.scoped();
		
		// The global arena holds all the extensions, for the lifetime of the module
		hostAmbisonicPtr = scoped.copyAcross(hostAmbisonic);
		hostAudioPortsConfigPtr = scoped.copyAcross(hostAudioPortsConfig);
		hostAudioPortsPtr = scoped.copyAcross(hostAudioPorts);
		//hostContextMenuPtr = scoped.copyAcross(hostContextMenu);
		hostGuiPtr = scoped.copyAcross(hostGui);
		hostLatencyPtr = scoped.copyAcross(hostLatency);
		hostLogPtr = scoped.copyAcross(hostLog);
		hostNoteNamePtr = scoped.copyAcross(hostNoteName);
		hostNotePortsPtr = scoped.copyAcross(hostNotePorts);
		hostParamsPtr = scoped.copyAcross(hostParams);
		hostPresetLoadPtr = scoped.copyAcross(hostPresetLoad);
		hostRemoteControlsPtr = scoped.copyAcross(hostRemoteControls);
		hostStatePtr = scoped.copyAcross(hostState);
		hostSurroundPtr = scoped.copyAcross(hostSurround);
		hostTailPtr = scoped.copyAcross(hostTail);
		hostThreadCheckPtr = scoped.copyAcross(hostThreadCheck);
		hostThreadPoolPtr = scoped.copyAcross(hostThreadPool);
		hostTimerSupportPtr = scoped.copyAcross(hostTimerSupport);
		// need to be able to point to these constants
		wclapPortMonoPtr = scoped.writeString(CLAP_PORT_MONO);
		wclapPortStereoPtr = scoped.writeString(CLAP_PORT_STEREO);
		wclapPortSurroundPtr = scoped.writeString(CLAP_PORT_SURROUND);
		wclapPortAmbisonicPtr = scoped.writeString(CLAP_PORT_AMBISONIC);
		wclapPortOtherPtr = scoped.writeString("(unknown host port type)");
		hostTrackInfoPtr = scoped.copyAcross(hostTrackInfo);
		hostVoiceInfoPtr = scoped.copyAcross(hostVoiceInfo);
		
		hostWebviewPtr = scoped.copyAcross(hostWebview);
		
		globalArena = scoped.commit();
		return true;
	}

	static int32_t staticWasiThreadSpawn(void *context, uint64_t threadArg) {
		auto *module = (WclapModule *)context;
		return module->wasiThreadSpawn(threadArg);
	}
	int32_t wasiThreadSpawn(uint64_t threadArg) {
		if (hasError) return -1;

		auto locked = threadLock();

		auto instance = instanceGroup->startInstance();
		if (!instance) {
			setError("failed to start instance for new WCLAP thread");
			return -1;
		}

		if (!addHostFunctions(instance.get())) {
			setError("failed to register host functions for new WCLAP thread");
			return -1;
		}

		// Use empty thread or start new one
		size_t index = threads.size();
		for (size_t i = 1; i < threads.size(); ++i) {
			if (!threads[i]) {
				index = i;
				break;
			}
		}
		if (index == threads.size()) threads.emplace_back();
		threads[index] = std::unique_ptr<Thread>{new Thread{
			.index=uint32_t(index),
			.threadArg=threadArg,
			.thread=std::thread{runThread, this, index},
			.instance=std::move(instance)
		}};

		return index;
	}
	
	// Host methods
	static Pointer<const void> hostTemplate_get_extension(void *context, Pointer<const wclap_host> wHost, Pointer<const char> extId) {
		auto &self = *(WclapModule *)context;
		auto hostExtStr = self.mainThread->getString(extId, 1024);

		auto *plugin = getPlugin(context, wHost);
		if (!plugin) return {0};
		
		if (hostExtStr == CLAP_EXT_WEBVIEW) {
			// Special-cased because we provide it to the plugin even if the host doesn't
			return self.hostWebviewPtr.cast<const void>();
		}
		
		const void *nativeHostExt = plugin->host->get_extension(plugin->host, hostExtStr.c_str());
		if (!nativeHostExt) return {0};
		
		if (hostExtStr == CLAP_EXT_AMBISONIC) {
			return self.hostAmbisonicPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_AUDIO_PORTS_CONFIG) {
			return self.hostAudioPortsConfigPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_AUDIO_PORTS) {
			return self.hostAudioPortsPtr.cast<const void>();
		//} else if (hostExtStr == CLAP_EXT_CONTEXT_MENU) {
		//	return self.hostContextMenuPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_GUI) {
			return self.hostGuiPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_LATENCY) {
			return self.hostLatencyPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_LOG) {
			return self.hostLogPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_NOTE_NAME) {
			return self.hostNoteNamePtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_NOTE_PORTS) {
			return self.hostNotePortsPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_PARAMS) {
			return self.hostParamsPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_PRESET_LOAD) {
			return self.hostPresetLoadPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_REMOTE_CONTROLS) {
			return self.hostRemoteControlsPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_STATE) {
			return self.hostStatePtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_SURROUND) {
			return self.hostSurroundPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_TAIL) {
			return self.hostTailPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_THREAD_CHECK) {
			return self.hostThreadCheckPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_THREAD_POOL) {
			return self.hostThreadPoolPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_TIMER_SUPPORT) {
			return self.hostTimerSupportPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_TRACK_INFO) {
			return self.hostTrackInfoPtr.cast<const void>();
		} else if (hostExtStr == CLAP_EXT_VOICE_INFO) {
			return self.hostVoiceInfoPtr.cast<const void>();
		}
		// null, no extensions for now
LOG_EXPR(hostExtStr);
		return {0};
	}
	static void hostTemplate_request_restart(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->host->request_restart(plugin->host);
	}
	static void hostTemplate_request_process(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->host->request_process(plugin->host);
	}
	static void hostTemplate_request_callback(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->host->request_callback(plugin->host);
	}

	static uint32_t inputEventsTemplate_size(void *context, Pointer<const wclap_input_events> obj) {
		auto *plugin = getPlugin(context, obj);
		if (plugin) return plugin->inputEventsSize();
		return 0;
	}
	static Pointer<const wclap_event_header> inputEventsTemplate_get(void *context, Pointer<const wclap_input_events> obj, uint32_t index) {
		auto *plugin = getPlugin(context, obj);
		if (plugin) return plugin->inputEventsGet(index);
		return {0};
	}
	static bool outputEventsTemplate_try_push(void *context, Pointer<const wclap_output_events> obj, Pointer<const wclap_event_header> event) {
		auto *plugin = getPlugin(context, obj);
		if (plugin) return plugin->outputEventsTryPush(event);
		return false;
	}
	static int64_t istreamTemplate_read(void *context, Pointer<const wclap_istream> obj, Pointer<void> buffer, uint64_t size) {
		auto *plugin = getPlugin(context, obj);
		if (plugin) return plugin->istreamRead(buffer, size);
		return -1;
	}
	static int64_t ostreamTemplate_write(void *context, Pointer<const wclap_ostream> obj, Pointer<const void> buffer, uint64_t size) {
		auto *plugin = getPlugin(context, obj);
		if (plugin) return plugin->ostreamWrite(buffer, size);
		return -1;
	}

	wclap_host_ambisonic hostAmbisonic;
	Pointer<wclap_host_ambisonic> hostAmbisonicPtr;
	static void hostAmbisonic_changed(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostAmbisonic->changed(plugin->host);
	}

	wclap_host_audio_ports_config hostAudioPortsConfig;
	Pointer<wclap_host_audio_ports_config> hostAudioPortsConfigPtr;
	static void hostAudioPortsConfig_rescan(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostAudioPortsConfig->rescan(plugin->host);
	}

	wclap_host_audio_ports hostAudioPorts;
	Pointer<wclap_host_audio_ports> hostAudioPortsPtr;
	static bool hostAudioPorts_is_rescan_flag_supported(void *context, Pointer<const wclap_host> wHost, uint32_t flag) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostAudioPorts->is_rescan_flag_supported(plugin->host, flag);
		return false;
	}
	static void hostAudioPorts_rescan(void *context, Pointer<const wclap_host> wHost, uint32_t flags) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostAudioPorts->rescan(plugin->host, flags);
	}

	wclap_host_gui hostGui;
	Pointer<wclap_host_gui> hostGuiPtr;
	static void hostGui_resize_hints_changed(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostGui->resize_hints_changed(plugin->host);
	}
	static bool hostGui_request_resize(void *context, Pointer<const wclap_host> wHost, uint32_t width, uint32_t height) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostGui->request_resize(plugin->host, width, height);
		return false;
	}
	static bool hostGui_request_show(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostGui->request_show(plugin->host);
		return false;
	}
	static bool hostGui_request_hide(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostGui->request_hide(plugin->host);
		return false;
	}
	static void hostGui_closed(void *context, Pointer<const wclap_host> wHost, bool was_destroyed) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostGui->closed(plugin->host, was_destroyed);
	}

	wclap_host_latency hostLatency;
	Pointer<wclap_host_latency> hostLatencyPtr;
	static void hostLatency_changed(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostLatency->changed(plugin->host);
	}

	wclap_host_log hostLog;
	Pointer<wclap_host_log> hostLogPtr;
	static void hostLog_log(void *context, Pointer<const wclap_host> wHost, int32_t severity, Pointer<const char> msg) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) {
			auto msgString = plugin->mainThread->getString(msg, wclap_bridge::maxLogStringLength);
			return plugin->hostLog->log(plugin->host, severity, msgString.c_str());
		}
	}

	wclap_host_note_name hostNoteName;
	Pointer<wclap_host_note_name> hostNoteNamePtr;
	static void hostNoteName_changed(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostNoteName->changed(plugin->host);
	}

	wclap_host_note_ports hostNotePorts;
	Pointer<wclap_host_note_ports> hostNotePortsPtr;
	static uint32_t hostNotePorts_supported_dialects(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostNotePorts->supported_dialects(plugin->host);
		return false;
	}
	static void hostNotePorts_rescan(void *context, Pointer<const wclap_host> wHost, uint32_t flags) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostNotePorts->rescan(plugin->host, flags);
	}

	wclap_host_params hostParams;
	Pointer<wclap_host_params> hostParamsPtr;
	static void hostParams_rescan(void *context, Pointer<const wclap_host> wHost, uint32_t flags) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostParams->rescan(plugin->host, flags);
	}
	static void hostParams_clear(void *context, Pointer<const wclap_host> wHost, uint32_t paramId, uint32_t flags) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostParams->clear(plugin->host, paramId, flags);
	}
	static void hostParams_request_flush(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostParams->request_flush(plugin->host);
	}

	wclap_host_preset_load hostPresetLoad;
	Pointer<wclap_host_preset_load> hostPresetLoadPtr;
	static void hostPresetLoad_on_error(void *context, Pointer<const wclap_host> wHost, uint32_t location_kind, Pointer<const char> location, Pointer<const char> load_key, int32_t os_error, Pointer<const char> msg) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) {
			auto locationString = location ? plugin->mainThread->getString(location, wclap_bridge::maxLogStringLength) : std::string{};
			auto loadKeyString = load_key ? plugin->mainThread->getString(load_key, wclap_bridge::maxLogStringLength) : std::string{};
			auto msgString = msg ? plugin->mainThread->getString(msg, wclap_bridge::maxLogStringLength) : std::string{};
			return plugin->hostPresetLoad->on_error(
				plugin->host,
				location_kind,
				location ? locationString.c_str() : nullptr,
				load_key ? loadKeyString.c_str() : nullptr,
				os_error,
				msg ? msgString.c_str() : nullptr);
		}
	}
	static void hostPresetLoad_loaded(void *context, Pointer<const wclap_host> wHost, uint32_t location_kind, Pointer<const char> location, Pointer<const char> load_key) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) {
			auto locationString = location ? plugin->mainThread->getString(location, wclap_bridge::maxLogStringLength) : std::string{};
			auto loadKeyString = load_key ? plugin->mainThread->getString(load_key, wclap_bridge::maxLogStringLength) : std::string{};
			return plugin->hostPresetLoad->loaded(
				plugin->host,
				location_kind,
				location ? locationString.c_str() : nullptr,
				load_key ? loadKeyString.c_str() : nullptr);
		}
	}

	wclap_host_remote_controls hostRemoteControls;
	Pointer<wclap_host_remote_controls> hostRemoteControlsPtr;
	static void hostRemoteControls_changed(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostRemoteControls->changed(plugin->host);
	}
	static void hostRemoteControls_suggest_page(void *context, Pointer<const wclap_host> wHost, uint32_t page_id) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostRemoteControls->suggest_page(plugin->host, page_id);
	}

	wclap_host_state hostState;
	Pointer<wclap_host_state> hostStatePtr;
	static void hostState_mark_dirty(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostState->mark_dirty(plugin->host);
	}

	wclap_host_surround hostSurround;
	Pointer<wclap_host_surround> hostSurroundPtr;
	static void hostSurround_changed(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostSurround->changed(plugin->host);
	}

	wclap_host_tail hostTail;
	Pointer<wclap_host_tail> hostTailPtr;
	static void hostTail_changed(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostTail->changed(plugin->host);
	}

	wclap_host_thread_check hostThreadCheck;
	Pointer<wclap_host_thread_check> hostThreadCheckPtr;
	static bool hostThreadCheck_is_main_thread(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostThreadCheck->is_main_thread(plugin->host);
		return true;
	}
	static bool hostThreadCheck_is_audio_thread(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostThreadCheck->is_audio_thread(plugin->host);
		return true;
	}

	wclap_host_thread_pool hostThreadPool;
	Pointer<wclap_host_thread_pool> hostThreadPoolPtr;
	static bool hostThreadPool_request_exec(void *context, Pointer<const wclap_host> wHost, uint32_t num_tasks) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostThreadPool->request_exec(plugin->host, num_tasks);
		return false;
	}

	wclap_host_timer_support hostTimerSupport;
	Pointer<wclap_host_timer_support> hostTimerSupportPtr;
	static bool hostTimerSupport_register_timer(void *context, Pointer<const wclap_host> wHost, uint32_t period_ms, Pointer<uint32_t> timer_id) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) {
			uint32_t nativeTimerId = 0;
			if (plugin->hostTimerSupport->register_timer(plugin->host, period_ms, &nativeTimerId)) {
				plugin->mainThread->set(timer_id, nativeTimerId);
				return true;
			}
			return false;
		}
		return false;
	}
	static bool hostTimerSupport_unregister_timer(void *context, Pointer<const wclap_host> wHost, uint32_t timer_id) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostTimerSupport->unregister_timer(plugin->host, timer_id);
		return false;
	}

	wclap_host_track_info hostTrackInfo;
	Pointer<wclap_host_track_info> hostTrackInfoPtr;
	static bool hostTrackInfo_get(void *context, Pointer<const wclap_host> wHost, Pointer<wclap_track_info> infoPtr) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) {
			clap_track_info info{.flags=0, .name={}, .color={0, 0, 0, 0}, .audio_channel_count=0, .audio_port_type=nullptr};
			if (plugin->hostTrackInfo->get(plugin->host, &info)) {
				wclap_track_info wclapInfo;
				wclapInfo.flags = info.flags;
				std::memcpy(wclapInfo.name, info.name, CLAP_NAME_SIZE);
				wclapInfo.color = {info.color.alpha, info.color.red, info.color.green, info.color.blue};
				wclapInfo.audio_channel_count = info.audio_channel_count;
				wclapInfo.audio_port_type = plugin->module.wclapPortOtherPtr;
				// Only assign port-type string if it's one of the known values
				if (info.flags&CLAP_TRACK_INFO_HAS_AUDIO_CHANNEL) {
					wclapInfo.audio_port_type = plugin->module.translatePortType(info.audio_port_type);
				};
				plugin->mainThread->set(infoPtr, wclapInfo);
				return true;
			}
			return false;
		}
		return false;
	}

	wclap_host_voice_info hostVoiceInfo;
	Pointer<wclap_host_voice_info> hostVoiceInfoPtr;
	static void hostVoiceInfo_changed(void *context, Pointer<const wclap_host> wHost) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->hostVoiceInfo->changed(plugin->host);
	}

	wclap_host_webview hostWebview;
	Pointer<wclap_host_webview> hostWebviewPtr;
	static bool hostWebview_send(void *context, Pointer<const wclap_host> wHost, Pointer<const void> buffer, uint32_t size) {
		auto *plugin = getPlugin(context, wHost);
		if (plugin) return plugin->webviewSend(buffer, size);
		return false;
	}
};

}; // namespace
