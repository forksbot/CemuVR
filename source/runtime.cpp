/**
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "log.hpp"
#include "version.h"
#include "runtime.hpp"
#include "runtime_objects.hpp"
#include "input.hpp"
#include "ini_file.hpp"
#include <assert.h>
#include <thread>
#include <algorithm>
#include <stb_image.h>
#include <stb_image_dds.h>
#include <stb_image_write.h>
#include <stb_image_resize.h>
#include <version.h>
#include <openvr.h>

extern volatile long g_network_traffic;
extern std::filesystem::path g_reshade_dll_path;
extern std::filesystem::path g_target_executable_path;

namespace reshade {
	unsigned int runtime::s_vr_system_ref_count = 0;
}

static inline std::filesystem::path absolute_path(std::filesystem::path path)
{
	std::error_code ec;
	// First convert path to an absolute path
	path = std::filesystem::absolute(g_reshade_dll_path.parent_path() / path, ec);
	// Finally try to canonicalize the path too (this may fail though, so it is optional)
	if (auto canonical_path = std::filesystem::canonical(path, ec); !ec)
		path = std::move(canonical_path);
	return path;
}

static inline bool check_preset_path(std::filesystem::path preset_path)
{
	// First make sure the extension matches, before diving into the file system
	if (preset_path.extension() != L".ini" && preset_path.extension() != L".txt")
		return false;

	preset_path = absolute_path(preset_path);

	std::error_code ec;
	const std::filesystem::file_type file_type = std::filesystem::status(preset_path, ec).type();
	if (file_type == std::filesystem::file_type::directory || ec.value() == 0x7b) // 0x7b: ERROR_INVALID_NAME
		return false;
	if (file_type == std::filesystem::file_type::not_found)
		return true; // A non-existent path is valid for a new preset

	return reshade::ini_file::load_cache(preset_path).has("", "Techniques");
}

static bool find_file(const std::vector<std::filesystem::path> &search_paths, std::filesystem::path &path)
{
	std::error_code ec;
	if (path.is_absolute())
		return std::filesystem::exists(path, ec);

	for (std::filesystem::path search_path : search_paths)
	{
		// Append relative file path to absolute search path
		search_path = absolute_path(std::move(search_path)) / path;

		if (std::filesystem::exists(search_path, ec)) {
			path = std::move(search_path);
			return true;
		}
	}
	return false;
}

static std::vector<std::filesystem::path> find_files(const std::vector<std::filesystem::path> &search_paths, std::initializer_list<const char *> extensions)
{
	std::error_code ec;
	std::vector<std::filesystem::path> files;
	for (std::filesystem::path search_path : search_paths)
	{
		// Ignore the working directory and instead start relative paths at the DLL location
		search_path = absolute_path(std::move(search_path));

		for (const auto &entry : std::filesystem::directory_iterator(search_path, ec))
			for (auto ext : extensions)
				if (entry.path().extension() == ext)
					files.push_back(entry.path());
	}
	return files;
}

reshade::runtime::runtime() :
	_start_time(std::chrono::high_resolution_clock::now()),
	_last_present_time(std::chrono::high_resolution_clock::now()),
	_last_frame_duration(std::chrono::milliseconds(1)),
	_effect_search_paths({ ".\\" }),
	_texture_search_paths({ ".\\" }),
	_global_preprocessor_definitions({
		"RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0",
		"RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0",
		"RESHADE_DEPTH_INPUT_IS_REVERSED=1",
		"RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0" }),
	_reload_key_data(),
	_effects_key_data(),
	_screenshot_key_data(),
	_previous_preset_key_data(),
	_next_preset_key_data(),
	_screenshot_path(g_target_executable_path.parent_path())
{
	// Default shortcut PrtScrn
	_screenshot_key_data[0] = 0x2C;

	_configuration_path = g_reshade_dll_path;
	_configuration_path.replace_extension(".ini");
	// First look for an API-named configuration file
	if (std::error_code ec; !std::filesystem::exists(_configuration_path, ec))
		// On failure check for a "ReShade.ini" in the application directory
		_configuration_path = g_target_executable_path.parent_path() / "ReShade.ini";
	if (std::error_code ec; !std::filesystem::exists(_configuration_path, ec))
		// If neither exist create a "ReShade.ini" in the ReShade DLL directory
		_configuration_path = g_reshade_dll_path.parent_path() / "ReShade.ini";

	_needs_update = check_for_update(_latest_version);

	load_config();

	init_vr_system();
}
reshade::runtime::~runtime()
{
	shutdown_vr_system();
	assert(_worker_threads.empty());
	assert(!_is_initialized && _techniques.empty());
}

bool reshade::runtime::on_init(input::window_handle window)
{
	LOG(INFO) << "Recreated runtime environment on runtime " << this << '.';

	_input = input::register_window(window);

	// Reset frame count to zero so effects are loaded in 'update_and_render_effects'
	_framecount = 0;

	_is_initialized = true;
	_last_reload_time = std::chrono::high_resolution_clock::now();

	return true;
}
void reshade::runtime::on_reset()
{

	if (!_is_initialized)
		return;

	LOG(INFO) << "Destroyed runtime environment on runtime " << this << '.';

	_width = _height = 0;
	_is_initialized = false;
	shutdown_vr_system();
	init_vr_system();
}
void reshade::runtime::on_present()
{
	// Get current time and date
	time_t t = std::time(nullptr); tm tm;
	localtime_s(&tm, &t);
	_date[0] = tm.tm_year + 1900;
	_date[1] = tm.tm_mon + 1;
	_date[2] = tm.tm_mday;
	_date[3] = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;

	// Advance various statistics
	_framecount++;
	const auto current_time = std::chrono::high_resolution_clock::now();
	_last_frame_duration = current_time - _last_present_time; _last_present_time = current_time;

	// Get VR headset poses
	vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = { };

	if (vr::VRCompositor() &&
		vr::VRCompositor()->WaitGetPoses(
			poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0) == vr::EVRCompositorError::VRCompositorError_None)
	{
		for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++)
		{
			if (!poses[i].bPoseIsValid)
			{
				continue;
			}

			switch (vr::VRSystem()->GetTrackedDeviceClass(i))
			{
			case vr::TrackedDeviceClass_HMD:
				// @TODO: Add implementation for providing headset tracking as right stick input.
				break;
			case vr::TrackedDeviceClass_Controller:
				break;
			}
		}
	}

	// Lock input so it cannot be modified by other threads while we are reading it here
	const auto input_lock = _input->lock();

	// Handle keyboard shortcuts
	if (!_ignore_shortcuts)
	{
		if (_input->is_key_pressed(_effects_key_data))
			_effects_enabled = !_effects_enabled;

		if (_input->is_key_pressed(_screenshot_key_data))
			_should_save_screenshot = true; // Notify 'update_and_render_effects' that we want to save a screenshot

		// Do not allow the next shortcuts while effects are being loaded or compiled (since they affect that state)
		if (!is_loading() && _reload_compile_queue.empty())
		{
			const bool is_next_preset_key_pressed = _input->is_key_pressed(_next_preset_key_data);
			const bool is_previous_preset_key_pressed = _input->is_key_pressed(_previous_preset_key_data);

			if (is_next_preset_key_pressed || is_previous_preset_key_pressed)
			{
				// The preset shortcut key was pressed down, so start the transition
				if (switch_to_next_preset({}, is_previous_preset_key_pressed))
				{
					_last_preset_switching_time = current_time;
					_is_in_between_presets_transition = true;
					save_config();
				}
			}

			// Continuously update preset values while a transition is in progress
			if (_is_in_between_presets_transition)
				load_current_preset();
		}
	}

	// Reset input status
	_input->next_frame();

	// Save modified INI files
	ini_file::flush_cache();

	// Detect high network traffic
	static int cooldown = 0, traffic = 0;
	if (cooldown-- > 0)
	{
		traffic += g_network_traffic > 0;
	}
	else
	{
		_has_high_network_activity = traffic > 10;
		traffic = 0;
		cooldown = 30;
	}

	// Reset frame statistics
	g_network_traffic = 0;
	_drawcalls = _vertices = 0;
}

void reshade::runtime::load_textures()
{
	LOG(INFO) << "Loading image files for textures ...";

	for (texture &texture : _textures)
	{
		if (texture.impl == nullptr || texture.impl_reference != texture_reference::none)
			continue; // Ignore textures that are not created yet and those that are handled in the runtime implementation

		std::filesystem::path source_path = std::filesystem::u8path(
			texture.annotation_as_string("source"));
		// Ignore textures that have no image file attached to them (e.g. plain render targets)
		if (source_path.empty())
			continue;

		// Search for image file using the provided search paths unless the path provided is already absolute
		if (!find_file(_texture_search_paths, source_path)) {
			LOG(ERROR) << "> Source " << source_path << " for texture '" << texture.unique_name << "' could not be found in any of the texture search paths.";
			continue;
		}

		unsigned char *filedata = nullptr;
		int width = 0, height = 0, channels = 0;

		if (FILE *file; _wfopen_s(&file, source_path.c_str(), L"rb") == 0)
		{
			// Read texture data into memory in one go since that is faster than reading chunk by chunk
			std::vector<uint8_t> mem(static_cast<size_t>(std::filesystem::file_size(source_path)));
			fread(mem.data(), 1, mem.size(), file);
			fclose(file);

			if (stbi_dds_test_memory(mem.data(), static_cast<int>(mem.size())))
				filedata = stbi_dds_load_from_memory(mem.data(), static_cast<int>(mem.size()), &width, &height, &channels, STBI_rgb_alpha);
			else
				filedata = stbi_load_from_memory(mem.data(), static_cast<int>(mem.size()), &width, &height, &channels, STBI_rgb_alpha);
		}

		if (filedata == nullptr) {
			LOG(ERROR) << "> Source " << source_path << " for texture '" << texture.unique_name << "' could not be loaded! Make sure it is of a compatible file format.";
			continue;
		}

		// Need to potentially resize image data to the texture dimensions
		if (texture.width != uint32_t(width) || texture.height != uint32_t(height))
		{
			LOG(INFO) << "> Resizing image data for texture '" << texture.unique_name << "' from " << width << "x" << height << " to " << texture.width << "x" << texture.height << " ...";

			std::vector<uint8_t> resized(texture.width * texture.height * 4);
			stbir_resize_uint8(filedata, width, height, 0, resized.data(), texture.width, texture.height, 0, 4);
			upload_texture(texture, resized.data());
		}
		else
		{
			upload_texture(texture, filedata);
		}

		stbi_image_free(filedata);
	}

	_textures_loaded = true;
}

void reshade::runtime::enable_technique(technique &technique)
{
	if (!_loaded_effects[technique.effect_index].compile_sucess)
		return; // Cannot enable techniques that failed to compile

	const bool status_changed = !technique.enabled;
	technique.enabled = true;
	technique.timeleft = technique.timeout;

	// Queue effect file for compilation if it was not fully loaded yet
	if (technique.impl == nullptr && // Avoid adding the same effect multiple times to the queue if it contains multiple techniques that were enabled simultaneously
		std::find(_reload_compile_queue.begin(), _reload_compile_queue.end(), technique.effect_index) == _reload_compile_queue.end())
	{
		_reload_total_effects++;
		_reload_compile_queue.push_back(technique.effect_index);
	}

	if (status_changed) // Increase rendering reference count
		_loaded_effects[technique.effect_index].rendering++;
}
void reshade::runtime::disable_technique(technique &technique)
{
	const bool status_changed =  technique.enabled;
	technique.enabled = false;
	technique.timeleft = 0;
	technique.average_cpu_duration.clear();
	technique.average_gpu_duration.clear();

	if (status_changed) // Decrease rendering reference count
		_loaded_effects[technique.effect_index].rendering--;
}

void reshade::runtime::subscribe_to_load_config(std::function<void(const ini_file &)> function)
{
	_load_config_callables.push_back(function);

	function(ini_file::load_cache(_configuration_path));
}
void reshade::runtime::subscribe_to_save_config(std::function<void(ini_file &)> function)
{
	_save_config_callables.push_back(function);

	function(ini_file::load_cache(_configuration_path));
}

void reshade::runtime::load_config()
{
	const ini_file &config = ini_file::load_cache(_configuration_path);

	std::filesystem::path current_preset_path;

	config.get("INPUT", "KeyReload", _reload_key_data);
	config.get("INPUT", "KeyEffects", _effects_key_data);
	config.get("INPUT", "KeyScreenshot", _screenshot_key_data);
	config.get("INPUT", "KeyPreviousPreset", _previous_preset_key_data);
	config.get("INPUT", "KeyNextPreset", _next_preset_key_data);
	config.get("INPUT", "PresetTransitionDelay", _preset_transition_delay);

	config.get("GENERAL", "PerformanceMode", _performance_mode);
	config.get("GENERAL", "EffectSearchPaths", _effect_search_paths);
	config.get("GENERAL", "TextureSearchPaths", _texture_search_paths);
	config.get("GENERAL", "PreprocessorDefinitions", _global_preprocessor_definitions);
	config.get("GENERAL", "CurrentPresetPath", current_preset_path);
	config.get("GENERAL", "ScreenshotPath", _screenshot_path);
	config.get("GENERAL", "ScreenshotFormat", _screenshot_format);
	config.get("GENERAL", "ScreenshotIncludePreset", _screenshot_include_preset);
	config.get("GENERAL", "ScreenshotSaveBefore", _screenshot_save_before);
	config.get("GENERAL", "NoReloadOnInit", _no_reload_on_init);

	if (current_preset_path.empty())
	{
		// Convert legacy preset index to new preset path
		size_t preset_index = 0;
		std::vector<std::filesystem::path> preset_files;
		config.get("GENERAL", "PresetFiles", preset_files);
		config.get("GENERAL", "CurrentPreset", preset_index);

		if (preset_index < preset_files.size())
			current_preset_path = preset_files[preset_index];
	}

	if (check_preset_path(current_preset_path))
		_current_preset_path = g_reshade_dll_path.parent_path() / current_preset_path;
	else // Select a default preset file if none exists yet
		_current_preset_path = g_reshade_dll_path.parent_path() / L"DefaultPreset.ini";

	for (const auto &callback : _load_config_callables)
		callback(config);
}
void reshade::runtime::save_config() const
{
	ini_file &config = ini_file::load_cache(_configuration_path);

	config.set("INPUT", "KeyReload", _reload_key_data);
	config.set("INPUT", "KeyEffects", _effects_key_data);
	config.set("INPUT", "KeyScreenshot", _screenshot_key_data);
	config.set("INPUT", "KeyPreviousPreset", _previous_preset_key_data);
	config.set("INPUT", "KeyNextPreset", _next_preset_key_data);
	config.set("INPUT", "PresetTransitionDelay", _preset_transition_delay);

	config.set("GENERAL", "PerformanceMode", _performance_mode);
	config.set("GENERAL", "EffectSearchPaths", _effect_search_paths);
	config.set("GENERAL", "TextureSearchPaths", _texture_search_paths);
	config.set("GENERAL", "PreprocessorDefinitions", _global_preprocessor_definitions);
	config.set("GENERAL", "CurrentPresetPath", _current_preset_path);
	config.set("GENERAL", "ScreenshotPath", _screenshot_path);
	config.set("GENERAL", "ScreenshotFormat", _screenshot_format);
	config.set("GENERAL", "ScreenshotIncludePreset", _screenshot_include_preset);
	config.set("GENERAL", "ScreenshotSaveBefore", _screenshot_save_before);
	config.set("GENERAL", "NoReloadOnInit", _no_reload_on_init);

	config.set("VR", "Enabled", _is_vr_enabled);
	config.set("VR", "AngularVelocityMultiplier", _vr_angular_velocity_multiplier);

	for (const auto &callback : _save_config_callables)
		callback(config);
}

void reshade::runtime::load_current_preset()
{
	const reshade::ini_file &preset = ini_file::load_cache(_current_preset_path);

	std::vector<std::string> technique_list;
	preset.get("", "Techniques", technique_list);
	std::vector<std::string> sorted_technique_list;
	preset.get("", "TechniqueSorting", sorted_technique_list);
	std::vector<std::string> preset_preprocessor_definitions;
	preset.get("", "PreprocessorDefinitions", preset_preprocessor_definitions);

	// Recompile effects if preprocessor definitions have changed or running in performance mode (in which case all preset values are compile-time constants)
	if (_reload_remaining_effects != 0 && // ... unless this is the 'load_current_preset' call in 'update_and_render_effects'
		(_performance_mode || preset_preprocessor_definitions != _preset_preprocessor_definitions))
	{
		_preset_preprocessor_definitions = std::move(preset_preprocessor_definitions);
		return; // Preset values are loaded in 'update_and_render_effects' during effect loading
	}

	// Reorder techniques
	if (sorted_technique_list.empty())
		sorted_technique_list = technique_list;

	std::sort(_techniques.begin(), _techniques.end(),
		[&sorted_technique_list](const auto &lhs, const auto &rhs) {
			return (std::find(sorted_technique_list.begin(), sorted_technique_list.end(), lhs.name) - sorted_technique_list.begin()) <
			       (std::find(sorted_technique_list.begin(), sorted_technique_list.end(), rhs.name) - sorted_technique_list.begin());
		});

	// Compute times since the transition has started and how much is left till it should end
	auto transition_time = std::chrono::duration_cast<std::chrono::microseconds>(_last_present_time - _last_preset_switching_time).count();
	auto transition_ms_left = _preset_transition_delay - transition_time / 1000;
	auto transition_ms_left_from_last_frame = transition_ms_left + std::chrono::duration_cast<std::chrono::microseconds>(_last_frame_duration).count() / 1000;

	if (_is_in_between_presets_transition && transition_ms_left <= 0)
		_is_in_between_presets_transition = false;

	for (uniform &variable : _uniforms)
	{
		reshadefx::constant values, values_old;

		switch (variable.type.base)
		{
		case reshadefx::type::t_int:
			get_uniform_value(variable, values.as_int, 16);
			preset.get(_loaded_effects[variable.effect_index].source_file.filename().u8string(), variable.name, values.as_int);
			set_uniform_value(variable, values.as_int, 16);
			break;
		case reshadefx::type::t_bool:
		case reshadefx::type::t_uint:
			get_uniform_value(variable, values.as_uint, 16);
			preset.get(_loaded_effects[variable.effect_index].source_file.filename().u8string(), variable.name, values.as_uint);
			set_uniform_value(variable, values.as_uint, 16);
			break;
		case reshadefx::type::t_float:
			get_uniform_value(variable, values.as_float, 16);
			values_old = values;
			preset.get(_loaded_effects[variable.effect_index].source_file.filename().u8string(), variable.name, values.as_float);
			if (_is_in_between_presets_transition)
			{
				// Perform smooth transition on floating point values
				for (int i = 0; i < 16; i++)
				{
					const auto transition_ratio = (values.as_float[i] - values_old.as_float[i]) / transition_ms_left_from_last_frame;
					values.as_float[i] = values.as_float[i] - transition_ratio * transition_ms_left;
				}
			}
			set_uniform_value(variable, values.as_float, 16);
			break;
		}
	}

	for (technique &technique : _techniques)
	{
		// Ignore preset if "enabled" annotation is set
		if (technique.annotation_as_int("enabled")
			|| std::find(technique_list.begin(), technique_list.end(), technique.name) != technique_list.end())
			enable_technique(technique);
		else
			disable_technique(technique);

		// Reset toggle key first, since it may not exist in the preset
		memset(technique.toggle_key_data, 0, sizeof(technique.toggle_key_data));
		preset.get("", "Key" + technique.name, technique.toggle_key_data);
	}
}
void reshade::runtime::save_current_preset() const
{
	reshade::ini_file &preset = ini_file::load_cache(_current_preset_path);

	// Build list of active techniques and effects
	std::vector<size_t> effect_list;
	std::vector<std::string> technique_list;
	std::vector<std::string> sorted_technique_list;
	effect_list.reserve(_techniques.size());
	technique_list.reserve(_techniques.size());
	sorted_technique_list.reserve(_techniques.size());

	for (const technique &technique : _techniques)
	{
		if (technique.enabled)
			technique_list.push_back(technique.name);
		if (technique.enabled || technique.toggle_key_data[0] != 0)
			effect_list.push_back(technique.effect_index);

		// Keep track of the order of all techniques and not just the enabled ones
		sorted_technique_list.push_back(technique.name);

		if (technique.toggle_key_data[0] != 0)
			preset.set("", "Key" + technique.name, technique.toggle_key_data);
		else if (int value = 0; preset.get("", "Key" + technique.name, value), value != 0)
			preset.set("", "Key" + technique.name, 0); // Clear toggle key data
	}

	preset.set("", "Techniques", std::move(technique_list));
	preset.set("", "TechniqueSorting", std::move(sorted_technique_list));
	preset.set("", "PreprocessorDefinitions", _preset_preprocessor_definitions);

	// TODO: Do we want to save spec constants here too? The preset will be rather empty in performance mode otherwise.
	for (const uniform &variable : _uniforms)
	{
		if (variable.special != special_uniform::none
			|| std::find(effect_list.begin(), effect_list.end(), variable.effect_index) == effect_list.end())
			continue;

		const std::string section =
			_loaded_effects[variable.effect_index].source_file.filename().u8string();
		reshadefx::constant values;

		assert(variable.type.components() <= 16);

		switch (variable.type.base)
		{
		case reshadefx::type::t_int:
			get_uniform_value(variable, values.as_int, 16);
			preset.set(section, variable.name, values.as_int, variable.type.components());
			break;
		case reshadefx::type::t_bool:
		case reshadefx::type::t_uint:
			get_uniform_value(variable, values.as_uint, 16);
			preset.set(section, variable.name, values.as_uint, variable.type.components());
			break;
		case reshadefx::type::t_float:
			get_uniform_value(variable, values.as_float, 16);
			preset.set(section, variable.name, values.as_float, variable.type.components());
			break;
		}
	}
}

bool reshade::runtime::switch_to_next_preset(const std::filesystem::path &filter_path, bool reversed)
{
	std::error_code ec; // This is here to ignore file system errors below

	std::filesystem::path filter_text = filter_path.filename();
	std::filesystem::path search_path = absolute_path(filter_path);

	if (std::filesystem::is_directory(search_path, ec))
		filter_text.clear();
	else if (!filter_text.empty())
		search_path = search_path.parent_path();

	size_t current_preset_index = std::numeric_limits<size_t>::max();
	std::vector<std::filesystem::path> preset_paths;

	for (const auto &entry : std::filesystem::directory_iterator(search_path, std::filesystem::directory_options::skip_permission_denied, ec))
	{
		// Skip anything that is not a valid preset file
		if (!check_preset_path(entry.path()))
			continue;

		// Keep track of the index of the current preset in the list of found preset files that is being build
		if (std::filesystem::equivalent(entry, _current_preset_path, ec)) {
			current_preset_index = preset_paths.size();
			preset_paths.push_back(entry);
			continue;
		}

		const std::wstring preset_name = entry.path().stem();
		// Only add those files that are matching the filter text
		if (filter_text.empty() || std::search(preset_name.begin(), preset_name.end(), filter_text.native().begin(), filter_text.native().end(),
			[](wchar_t c1, wchar_t c2) { return towlower(c1) == towlower(c2); }) != preset_name.end())
			preset_paths.push_back(entry);
	}

	if (preset_paths.begin() == preset_paths.end())
		return false; // No valid preset files were found, so nothing more to do

	if (current_preset_index == std::numeric_limits<size_t>::max())
	{
		// Current preset was not in the container path, so just use the first or last file
		if (reversed)
			_current_preset_path = preset_paths.back();
		else
			_current_preset_path = preset_paths.front();
	}
	else
	{
		// Current preset was found in the container path, so use the file before or after it
		if (auto it = std::next(preset_paths.begin(), current_preset_index); reversed)
			_current_preset_path = it == preset_paths.begin() ? preset_paths.back() : *--it;
		else
			_current_preset_path = it == std::prev(preset_paths.end()) ? preset_paths.front() : *++it;
	}

	return true;
}

void reshade::runtime::save_screenshot(const std::wstring &postfix, const bool should_save_preset)
{
	const int hour = _date[3] / 3600;
	const int minute = (_date[3] - hour * 3600) / 60;
	const int seconds = _date[3] - hour * 3600 - minute * 60;

	char filename[21];
	sprintf_s(filename, " %.4d-%.2d-%.2d %.2d-%.2d-%.2d", _date[0], _date[1], _date[2], hour, minute, seconds);

	const std::wstring least = (_screenshot_path.is_relative() ? g_target_executable_path.parent_path() / _screenshot_path : _screenshot_path) / g_target_executable_path.stem().concat(filename);
	const std::wstring screenshot_path = least + postfix + (_screenshot_format == 0 ? L".bmp" : L".png");

	LOG(INFO) << "Saving screenshot to " << screenshot_path << " ...";

	_screenshot_save_success = false; // Default to a save failure unless it is reported to succeed below

	if (std::vector<uint8_t> data(_width * _height * 4); capture_screenshot(data.data()))
	{
		if (FILE *file; _wfopen_s(&file, screenshot_path.c_str(), L"wb") == 0)
		{
			const auto write_callback = [](void *context, void *data, int size) {
				fwrite(data, 1, size, static_cast<FILE *>(context));
			};

			switch (_screenshot_format)
			{
			case 0:
				_screenshot_save_success = stbi_write_bmp_to_func(write_callback, file, _width, _height, 4, data.data()) != 0;
				break;
			case 1:
				_screenshot_save_success = stbi_write_png_to_func(write_callback, file, _width, _height, 4, data.data(), 0) != 0;
				break;
			}

			fclose(file);
		}
	}

	_last_screenshot_file = screenshot_path;
	_last_screenshot_time = std::chrono::high_resolution_clock::now();

	if (!_screenshot_save_success)
	{
		LOG(ERROR) << "Failed to write screenshot to " << screenshot_path << '!';
	}
	else if (_screenshot_include_preset && should_save_preset && ini_file::flush_cache(_current_preset_path))
	{
		// Preset was flushed to disk, so can just copy it over to the new location
		std::error_code ec;
		std::filesystem::copy_file(_current_preset_path, least + L".ini", std::filesystem::copy_options::overwrite_existing, ec);
	}
}

void reshade::runtime::get_uniform_value(const uniform &variable, uint8_t *data, size_t size) const
{
	assert(data != nullptr);

	size = std::min(size, size_t(variable.size));

	assert(variable.storage_offset + size <= _uniform_data_storage.size());

	std::memcpy(data, &_uniform_data_storage[variable.storage_offset], size);
}
void reshade::runtime::get_uniform_value(const uniform &variable, bool *values, size_t count) const
{
	count = std::min(count, size_t(variable.size / 4));

	assert(values != nullptr);

	const auto data = static_cast<uint8_t *>(alloca(variable.size));
	get_uniform_value(variable, data, variable.size);

	for (size_t i = 0; i < count; i++)
		values[i] = reinterpret_cast<const uint32_t *>(data)[i] != 0;
}
void reshade::runtime::get_uniform_value(const uniform &variable, int32_t *values, size_t count) const
{
	if (!variable.type.is_floating_point() && _renderer_id != 0x9000)
	{
		get_uniform_value(variable, reinterpret_cast<uint8_t *>(values), count * sizeof(int32_t));
		return;
	}

	count = std::min(count, variable.size / sizeof(float));

	assert(values != nullptr);

	const auto data = static_cast<uint8_t *>(alloca(variable.size));
	get_uniform_value(variable, data, variable.size);

	for (size_t i = 0; i < count; i++)
		values[i] = static_cast<int32_t>(reinterpret_cast<const float *>(data)[i]);
}
void reshade::runtime::get_uniform_value(const uniform &variable, uint32_t *values, size_t count) const
{
	get_uniform_value(variable, reinterpret_cast<int32_t *>(values), count);
}
void reshade::runtime::get_uniform_value(const uniform &variable, float *values, size_t count) const
{
	if (variable.type.is_floating_point() || _renderer_id == 0x9000)
	{
		get_uniform_value(variable, reinterpret_cast<uint8_t *>(values), count * sizeof(float));
		return;
	}

	count = std::min(count, variable.size / sizeof(int32_t));

	assert(values != nullptr);

	const auto data = static_cast<uint8_t *>(alloca(variable.size));
	get_uniform_value(variable, data, variable.size);

	for (size_t i = 0; i < count; ++i)
		if (variable.type.is_signed())
			values[i] = static_cast<float>(reinterpret_cast<const int32_t *>(data)[i]);
		else
			values[i] = static_cast<float>(reinterpret_cast<const uint32_t *>(data)[i]);
}
void reshade::runtime::set_uniform_value(uniform &variable, const uint8_t *data, size_t size)
{
	assert(data != nullptr);

	size = std::min(size, size_t(variable.size));

	assert(variable.storage_offset + size <= _uniform_data_storage.size());

	std::memcpy(&_uniform_data_storage[variable.storage_offset], data, size);
}
void reshade::runtime::set_uniform_value(uniform &variable, const bool *values, size_t count)
{
	const auto data = static_cast<uint8_t *>(alloca(count * 4));
	switch (_renderer_id != 0x9000 ? variable.type.base : reshadefx::type::t_float)
	{
	case reshadefx::type::t_bool:
		for (size_t i = 0; i < count; ++i)
			reinterpret_cast<int32_t *>(data)[i] = values[i] ? -1 : 0;
		break;
	case reshadefx::type::t_int:
	case reshadefx::type::t_uint:
		for (size_t i = 0; i < count; ++i)
			reinterpret_cast<int32_t *>(data)[i] = values[i] ? 1 : 0;
		break;
	case reshadefx::type::t_float:
		for (size_t i = 0; i < count; ++i)
			reinterpret_cast<float *>(data)[i] = values[i] ? 1.0f : 0.0f;
		break;
	}

	set_uniform_value(variable, data, count * 4);
}
void reshade::runtime::set_uniform_value(uniform &variable, const int32_t *values, size_t count)
{
	if (!variable.type.is_floating_point() && _renderer_id != 0x9000)
	{
		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(values), count * sizeof(int));
		return;
	}

	const auto data = static_cast<float *>(alloca(count * sizeof(float)));
	for (size_t i = 0; i < count; ++i)
		data[i] = static_cast<float>(values[i]);

	set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(float));
}
void reshade::runtime::set_uniform_value(uniform &variable, const uint32_t *values, size_t count)
{
	if (!variable.type.is_floating_point() && _renderer_id != 0x9000)
	{
		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(values), count * sizeof(int));
		return;
	}

	const auto data = static_cast<float *>(alloca(count * sizeof(float)));
	for (size_t i = 0; i < count; ++i)
		data[i] = static_cast<float>(values[i]);

	set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(float));
}
void reshade::runtime::set_uniform_value(uniform &variable, const float *values, size_t count)
{
	if (variable.type.is_floating_point() || _renderer_id == 0x9000)
	{
		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(values), count * sizeof(float));
		return;
	}

	const auto data = static_cast<int32_t *>(alloca(count * sizeof(int32_t)));
	for (size_t i = 0; i < count; ++i)
		data[i] = static_cast<int32_t>(values[i]);

	set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(int32_t));
}

void reshade::runtime::reset_uniform_value(uniform &variable)
{
	if (!variable.has_initializer_value)
	{
		memset(_uniform_data_storage.data() + variable.storage_offset, 0, variable.size);
		return;
	}

	if (_renderer_id == 0x9000)
	{
		// Force all uniforms to floating-point in D3D9
		for (size_t i = 0; i < variable.size / sizeof(float); i++)
		{
			switch (variable.type.base)
			{
			case reshadefx::type::t_int:
				reinterpret_cast<float *>(_uniform_data_storage.data() + variable.storage_offset)[i] = static_cast<float>(variable.initializer_value.as_int[i]);
				break;
			case reshadefx::type::t_bool:
			case reshadefx::type::t_uint:
				reinterpret_cast<float *>(_uniform_data_storage.data() + variable.storage_offset)[i] = static_cast<float>(variable.initializer_value.as_uint[i]);
				break;
			case reshadefx::type::t_float:
				reinterpret_cast<float *>(_uniform_data_storage.data() + variable.storage_offset)[i] = variable.initializer_value.as_float[i];
				break;
			}
		}
	}
	else
	{
		memcpy(_uniform_data_storage.data() + variable.storage_offset, variable.initializer_value.as_uint, variable.size);
	}
}

void reshade::runtime::init_vr_system()
{
	if (_is_vr_enabled && s_vr_system_ref_count++ == 0)
	{
		vr::EVRInitError e = vr::VRInitError_None;

		vr::IVRSystem* m_pHMD = vr::VR_Init(&e, vr::EVRApplicationType::VRApplication_Scene);
		vr::TrackedDevicePose_t m_rTrackedDevicePose[vr::k_unMaxTrackedDeviceCount];

		if (e != vr::VRInitError_None || !vr::VRCompositor())
		{
			s_vr_system_ref_count = 0;

			LOG(ERROR) << "Failed to initialize VR system with error code " << e << ".";
		}
	}
}
void reshade::runtime::shutdown_vr_system()
{
	if (s_vr_system_ref_count && --s_vr_system_ref_count == 0)
	{
		vr::VR_Shutdown();
	}
	else
	{
		LOG(ERROR) << "Failed to shutdown VR System!";
	}
}
